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

GatheredCandidate GatherBestLocalCandidate()
{
	try {

	rtc::Configuration cfg;
	// A public STUN server here is only ever used to learn this
	// machine's own reflexive address for the probe report - unrelated
	// to which STUN server the eventual P2P PeerConnection uses (see
	// docs/design/ppobs-p2p-nat-probe-gap.zh-CN.md R2, and its note on
	// pplayer's nat-probe.mjs using the same public STUN for the same
	// probe-only reason).
	cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");

	auto pc = std::make_shared<rtc::PeerConnection>(cfg);
	// A PeerConnection with no track/channel has nothing to negotiate,
	// so it never starts ICE gathering - an empty DataChannel is the
	// same trigger ppplayer's nat-probe.mjs uses (createDataChannel
	// before createOffer/setLocalDescription).
	auto probeChannel = pc->createDataChannel("probe");

	auto state = std::make_shared<GatheringState>();

	pc->onGatheringStateChange([state](rtc::PeerConnection::GatheringState gatheringState) {
		if (gatheringState == rtc::PeerConnection::GatheringState::Complete) {
			std::lock_guard<std::mutex> lock(state->mutex);
			state->done = true;
			state->cv.notify_all();
		}
	});

	pc->onLocalCandidate([state](const rtc::Candidate &candidate) {
		rtc::Candidate resolved = candidate;
		if (!resolved.resolve(rtc::Candidate::ResolveMode::Simple))
			return;
		auto address = resolved.address();
		auto port = resolved.port();
		if (!address || !port)
			return;
		const bool isSrflx = resolved.type() == rtc::Candidate::Type::ServerReflexive;
		const bool isHost = resolved.type() == rtc::Candidate::Type::Host;
		if (!isSrflx && !isHost)
			return;
		if (isHost && !IsPublicIPAddress(*address))
			return;

		std::lock_guard<std::mutex> lock(state->mutex);
		// Prefer srflx (actual public address) over host; keep the
		// first candidate of the winning kind rather than the last.
		if (state->best.found && (state->best.isSrflx || !isSrflx))
			return;
		state->best.found = true;
		state->best.isSrflx = isSrflx;
		state->best.address = *address;
		state->best.port = *port;
	});

	try {
		pc->setLocalDescription();
	} catch (const std::exception &e) {
		probe_log(LOG_DEBUG, "probe PeerConnection setLocalDescription failed: %s", e.what());
		return GatheredCandidate{};
	}

	{
		std::unique_lock<std::mutex> lock(state->mutex);
		state->cv.wait_for(lock, std::chrono::milliseconds(ICE_GATHER_TIMEOUT_MS), [&] { return state->done; });
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
				 const std::string &appSecret, const std::string &streamName)
{
	NATProbeResult result;

	const std::string probeUrl = DeriveNatProbeUrl(ppcenterUrl);
	if (probeUrl.empty() || appId.empty() || appSecret.empty() || streamName.empty())
		return result;

	const std::string clientId = GetOrCreateP2PClientId();
	if (clientId.empty())
		return result;

	auto candidate = GatherBestLocalCandidate();
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
