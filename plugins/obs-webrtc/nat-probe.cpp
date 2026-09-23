#include "nat-probe.h"
#include "whip-utils.h"

#include <obs-module.h>
#include <obs.h>
#include <util/platform.h>
#include <util/util.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define probe_log(level, format, ...) blog(level, "[obs-webrtc] [nat-probe] " format, ##__VA_ARGS__)

namespace {

constexpr long NAT_PROBE_TIMEOUT_SECONDS = 5;
constexpr int ICE_GATHER_TIMEOUT_MS = 4000;
constexpr size_t MAX_RESPONSE_SIZE = 8 * 1024;

// /v1/nat/probe lives alongside /v1/publish/requests on the same ppcenter
// host - ppobs's "ppcenter_url" setting is the full publish endpoint (e.g.
// "https://host/v1/publish/requests"), so the probe endpoint is derived by
// truncating after the "/v1/" segment and appending "nat/probe", rather
// than asking the user to configure a second URL. See
// ppcenter/internal/apis/route.go: both are mounted under the same "/v1"
// router group.
std::string DeriveNatProbeUrl(const std::string &ppcenterUrl)
{
	const std::string marker = "/v1/";
	auto pos = ppcenterUrl.find(marker);
	if (pos == std::string::npos)
		return "";
	return ppcenterUrl.substr(0, pos + marker.size()) + "nat/probe";
}

struct ResponseBuffer {
	std::string data;
	bool exceeded = false;
};

size_t write_response(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *buffer = static_cast<ResponseBuffer *>(userdata);
	const size_t length = size * nmemb;
	if (buffer->data.size() + length > MAX_RESPONSE_SIZE) {
		buffer->exceeded = true;
		return 0;
	}
	buffer->data.append(ptr, length);
	return length;
}

// Gathers this machine's local ICE candidates with a short-lived,
// media-less PeerConnection (mirrors ppplayer's nat-probe.mjs: an empty
// DataChannel is enough to start ICE gathering) and returns the best
// candidate found - preferring a server-reflexive (STUN-derived public)
// candidate over a bare host candidate, matching nat-probe.mjs's
// `c.type === 'srflx' || c.type === 'host'` preference order.
struct GatheredCandidate {
	bool found = false;
	bool isSrflx = false;
	bool isIPv4 = false;
	std::string address;
	uint16_t port = 0;
};

struct GatheringState {
	std::mutex mutex;
	std::condition_variable cv;
	bool done = false;
	GatheredCandidate best;
};

bool IsPublicIPAddress(const std::string &address)
{
	in_addr ipv4{};
	if (inet_pton(AF_INET, address.c_str(), &ipv4) == 1) {
		const uint32_t value = ntohl(ipv4.s_addr);
		const uint8_t first = static_cast<uint8_t>(value >> 24);
		const uint8_t second = static_cast<uint8_t>(value >> 16);
		return first != 0 && first != 10 && first != 127 &&
		       !(first == 100 && second >= 64 && second <= 127) &&
		       !(first == 169 && second == 254) && !(first == 172 && second >= 16 && second <= 31) &&
		       !(first == 192 && second == 168) && first < 224;
	}

	in6_addr ipv6{};
	if (inet_pton(AF_INET6, address.c_str(), &ipv6) != 1)
		return false;
	const auto *bytes = reinterpret_cast<const uint8_t *>(&ipv6);
	const bool unspecifiedOrLoopback = IN6_IS_ADDR_UNSPECIFIED(&ipv6) || IN6_IS_ADDR_LOOPBACK(&ipv6);
	const bool linkLocal = bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
	const bool uniqueLocal = (bytes[0] & 0xfe) == 0xfc;
	const bool multicast = bytes[0] == 0xff;
	return !unspecifiedOrLoopback && !linkLocal && !uniqueLocal && !multicast;
}

bool IsIPv4Address(const std::string &address)
{
	in_addr ipv4{};
	return inet_pton(AF_INET, address.c_str(), &ipv4) == 1;
}

// Ranks candidates the same way ppplayer's nat-probe.mjs pickProbeCandidate
// does: IPv4 server-reflexive best, then any server-reflexive, then an IPv4
// host address, then an IPv6 host address. Lower is better.
int CandidateRank(bool isSrflx, bool isIPv4)
{
	if (isSrflx && isIPv4)
		return 0;
	if (isSrflx)
		return 1;
	if (isIPv4)
		return 2;
	return 3;
}

// ppcenter runs its own STUN server (internal/stun) on the same host as the
// API, conventionally at port 3478. Mirrors pplayer's nat-probe.mjs
// deriveStunIceServers(), string-sliced instead of URL-parsed to match this
// file's own DeriveNatProbeUrl() style rather than pulling in a URL library
// for one host extraction.
std::string DeriveStunHost(const std::string &ppcenterUrl)
{
	auto schemeEnd = ppcenterUrl.find("://");
	const std::string rest = schemeEnd == std::string::npos ? ppcenterUrl : ppcenterUrl.substr(schemeEnd + 3);
	const auto hostEnd = rest.find_first_of(":/");
	return hostEnd == std::string::npos ? rest : rest.substr(0, hostEnd);
}

GatheredCandidate GatherBestLocalCandidate(const std::string &ppcenterUrl)
{
	try {

	// Temporary: route libdatachannel's own (libjuice) internal logging into
	// the OBS log, so an ICE gathering that yields no candidates at all can
	// be explained rather than guessed at. Process-global and verbose -
	// remove alongside the other [diag] logging below once the 2026-09-22
	// STUN investigation concludes. See docs/test/ppcdn-debug-log.md.
	static std::once_flag rtcLoggerOnce;
	std::call_once(rtcLoggerOnce, [] {
		rtc::InitLogger(rtc::LogLevel::Debug, [](rtc::LogLevel level, std::string message) {
			probe_log(LOG_INFO, "[diag][rtc:%d] %s", (int)level, message.c_str());
		});
	});

	rtc::Configuration cfg;
	// EXACTLY ONE STUN server - do not add a second one as a "fallback".
	// libdatachannel/libjuice only ever uses the first STUN entry and
	// silently discards the rest, logging "Only TURN servers are supported
	// as additional ICE servers" (rtc::impl::IceTransport::addIceServer).
	// Confirmed from the library's own logs in production 2026-09-22, after
	// a commit here added Google's public STUN as a second entry believing
	// it gave reachability insurance: it never ran at all, and the probe
	// still failed because the only server actually used was ppcenter's.
	// If a real fallback is ever needed it has to be a *second sequential*
	// probe with a different single server, not two entries in one
	// Configuration - note that costs another ICE_GATHER_TIMEOUT_MS on the
	// publish path, since Setup() calls this synchronously.
	//
	// ppcenter's own STUN is the deliberate choice here (ppcenter ships one,
	// internal/stun, on the same host as the API): it shares this
	// publisher's network path to ppcenter, and using the same server on
	// both sides keeps the publisher's and a player's observed address
	// family consistent - api.pp-cdn.org has no AAAA record so it can only
	// answer IPv4, whereas a public provider often answers IPv6, and
	// ppcenter's eligibility check rejects a mixed pair outright as
	// address_family_mismatch. It must be reachable on UDP 3478 for any of
	// this to work: that means both the cloud security group *and* the
	// host firewall (ufw on the prod box) - a ufw default-deny with only
	// 22/80/443 allowed silently blackholed every probe for hours on
	// 2026-09-22 while the process itself looked healthy in `ss -ulnp`.
	// See docs/test/ppcdn-debug-log.md's 2026-09-22 entry for the full trail.
	const std::string stunHost = DeriveStunHost(ppcenterUrl);
	if (!stunHost.empty())
		cfg.iceServers.emplace_back("stun:" + stunHost + ":3478");
	// Temporary diagnostic logging (LOG_INFO, not LOG_DEBUG, so it's
	// guaranteed to land in the log file) - remove once the 2026-09-22
	// STUN-reachability investigation concludes. See
	// docs/test/ppcdn-debug-log.md's 2026-09-22 entry.
	for (const auto &s : cfg.iceServers)
		probe_log(LOG_INFO, "[diag] configured ICE server: %s", s.hostname.c_str());

	auto pc = std::make_shared<rtc::PeerConnection>(cfg);
	// A PeerConnection with no track/channel has nothing to negotiate,
	// so it never starts ICE gathering - an empty DataChannel is the
	// same trigger ppplayer's nat-probe.mjs uses (createDataChannel
	// before createOffer/setLocalDescription).
	auto probeChannel = pc->createDataChannel("probe");

	auto state = std::make_shared<GatheringState>();

	pc->onGatheringStateChange([state](rtc::PeerConnection::GatheringState gatheringState) {
		probe_log(LOG_INFO, "[diag] gathering state: %d", (int)gatheringState);
		if (gatheringState == rtc::PeerConnection::GatheringState::Complete) {
			std::lock_guard<std::mutex> lock(state->mutex);
			state->done = true;
			state->cv.notify_all();
		}
	});
	pc->onStateChange([](rtc::PeerConnection::State s) { probe_log(LOG_INFO, "[diag] pc state: %d", (int)s); });

	pc->onLocalCandidate([state](const rtc::Candidate &candidate) {
		// Temporary diagnostic logging - see this function's iceServers
		// comment; remove alongside it once the investigation concludes.
		probe_log(LOG_INFO, "[diag] raw candidate: %s", candidate.candidate().c_str());
		rtc::Candidate resolved = candidate;
		if (!resolved.resolve(rtc::Candidate::ResolveMode::Simple)) {
			probe_log(LOG_INFO, "[diag] candidate failed to resolve");
			return;
		}
		auto address = resolved.address();
		auto port = resolved.port();
		if (!address || !port) {
			probe_log(LOG_INFO, "[diag] resolved candidate has no address/port");
			return;
		}
		probe_log(LOG_INFO, "[diag] resolved candidate: address=%s port=%u type=%d", address->c_str(), *port,
			  (int)resolved.type());
		const bool isSrflx = resolved.type() == rtc::Candidate::Type::ServerReflexive;
		const bool isHost = resolved.type() == rtc::Candidate::Type::Host;
		if (!isSrflx && !isHost)
			return;
		if (isHost && !IsPublicIPAddress(*address))
			return;
		const bool isIPv4 = IsIPv4Address(*address);

		std::lock_guard<std::mutex> lock(state->mutex);
		// Keep the better-ranked candidate; on a tie, keep the first
		// seen rather than the last. See CandidateRank().
		if (state->best.found && CandidateRank(isSrflx, isIPv4) >= CandidateRank(state->best.isSrflx, state->best.isIPv4))
			return;
		state->best.found = true;
		state->best.isSrflx = isSrflx;
		state->best.isIPv4 = isIPv4;
		state->best.address = *address;
		state->best.port = *port;
		// Wake the waiter as soon as an IPv4 srflx candidate shows up -
		// the top rank - rather than only on full gathering completion.
		// Specifically IPv4, not "any srflx": with two STUN servers
		// racing, an IPv6 srflx from whichever server answers first must
		// not short-circuit gathering before an IPv4 one (from either
		// server) has a chance to arrive, or this probe and a player's
		// own could disagree on address family (see this function's
		// iceServers comment on address_family_mismatch). If IPv4 never
		// shows up, the wait_for below still falls back to the best
		// candidate found by the time gathering completes or the
		// timeout elapses. Same fix as pplayer's nat-probe.mjs.
		if (isSrflx && isIPv4) state->cv.notify_all();
	});

	try {
		pc->setLocalDescription();
	} catch (const std::exception &e) {
		probe_log(LOG_DEBUG, "probe PeerConnection setLocalDescription failed: %s", e.what());
		return GatheredCandidate{};
	}

	{
		std::unique_lock<std::mutex> lock(state->mutex);
		const bool predicateMet = state->cv.wait_for(
			lock, std::chrono::milliseconds(ICE_GATHER_TIMEOUT_MS),
			[&] { return state->done || (state->best.found && state->best.isSrflx && state->best.isIPv4); });
		probe_log(LOG_INFO,
			  "[diag] wait finished: predicateMet=%d done=%d best.found=%d best.isSrflx=%d best.isIPv4=%d",
			  (int)predicateMet, (int)state->done, (int)state->best.found, (int)state->best.isSrflx,
			  (int)state->best.isIPv4);
	}

	pc->resetCallbacks();
	pc->close();
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->best;
	} catch (const std::exception &e) {
		probe_log(LOG_DEBUG, "probe PeerConnection failed: %s", e.what());
		return GatheredCandidate{};
	}
}

} // namespace

std::string GetOrCreateP2PClientId()
{
	static std::mutex m;
	static std::string cached;
	std::lock_guard<std::mutex> lock(m);
	if (!cached.empty())
		return cached;

	BPtr<char> configPath = obs_module_config_path("p2p_client_id");
	if (!configPath)
		return "";

	BPtr<char> existing = os_quick_read_utf8_file(configPath);
	if (existing && *existing.Get()) {
		cached = trim_string(existing.Get());
		if (!cached.empty())
			return cached;
	}

	BPtr<char> generated = os_generate_uuid();
	if (!generated)
		return "";
	cached = generated.Get();

	BPtr<char> dir = obs_module_config_path("");
	if (dir)
		os_mkdirs(dir);
	if (!os_quick_write_utf8_file(configPath, cached.c_str(), cached.size(), false))
		probe_log(LOG_WARNING, "failed to persist P2P client ID");

	return cached;
}

NATProbeResult ProbePublisherNAT(const std::string &ppcenterUrl, const std::string &appId,
				 const std::string &appSecret, const std::string &streamName,
				 const std::string &clientId)
{
	NATProbeResult result;

	const std::string probeUrl = DeriveNatProbeUrl(ppcenterUrl);
	if (probeUrl.empty() || appId.empty() || appSecret.empty() || streamName.empty())
		return result;

	if (clientId.empty())
		return result;

	auto candidate = GatherBestLocalCandidate(ppcenterUrl);
	if (!candidate.found)
		return result;

	// Matches nat-probe.mjs's mapping: a server-reflexive candidate
	// means the local NAT (if any) allows an outbound-mapped public
	// address, so "cone" is the honest guess here (not "public" -
	// that's reserved for when the srflx address matches a directly
	// bound host address, which this probe doesn't verify). A bare host
	// candidate implies no NAT was detected between this machine and
	// the STUN server at all - "public" per nat-probe.mjs's own
	// srflx/host split.
	const std::string natType = candidate.isSrflx ? "cone" : "public";

	nlohmann::json body = {
		{"appId", appId},         {"appSecret", appSecret}, {"kind", "publisher"},
		{"streamName", streamName}, {"clientId", clientId},   {"natType", natType},
		{"publicIp", candidate.address}, {"publicPort", candidate.port},
	};
	const std::string jsonBody = body.dump();

	CURL *curl = curl_easy_init();
	if (!curl)
		return result;

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	const std::string userAgent = generate_user_agent();

	ResponseBuffer buffer;
	curl_easy_setopt(curl, CURLOPT_URL, probeUrl.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, NAT_PROBE_TIMEOUT_SECONDS);

	const CURLcode rc = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK || status != 200) {
		probe_log(LOG_DEBUG, "NAT probe submit failed (curl=%d http=%ld)", (int)rc, status);
		return result;
	}

	try {
		auto decoded = nlohmann::json::parse(buffer.data);
		result.probe_id = decoded.value("probeId", "");
		result.succeeded = !result.probe_id.empty();
	} catch (const std::exception &e) {
		probe_log(LOG_DEBUG, "NAT probe response parse failed: %s", e.what());
	}

	return result;
}
