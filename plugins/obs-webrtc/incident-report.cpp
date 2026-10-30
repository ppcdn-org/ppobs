#include "incident-report.h"
#include "whip-utils.h"

#include <obs-module.h>
#include <util/platform.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <sys/types.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <sstream>

#define inc_log(level, format, ...) blog(level, "[obs-webrtc] [incident-report] " format, ##__VA_ARGS__)

const char *const kCrashReportedStateFile = ".ppcenter-reported.json";
const char *const kLastPublishStateFile = ".ppcenter-last-publish.json";

namespace {

constexpr size_t kMaxSummaryBytes = 512;
constexpr size_t kMaxStackBytes = 8000;
constexpr size_t kMaxDetailBytes = 512;
constexpr size_t kMaxDumpGzBytes = 1024 * 1024;
constexpr size_t kMaxIdentBytes = 128;
constexpr size_t kMaxStackFrames = 30;
constexpr size_t kMaxResponseSize = 8 * 1024;

std::string DumpJson(const nlohmann::json &j)
{
	// A crash log can contain arbitrary bytes; never let a stray invalid UTF-8
	// sequence turn into an exception (nlohmann's default) and lose the report.
	return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string Trim(const std::string &s)
{
	size_t b = 0;
	size_t e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
		b++;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
		e--;
	return s.substr(b, e - b);
}

bool StartsWith(const std::string &s, const char *prefix)
{
	return s.compare(0, strlen(prefix), prefix) == 0;
}

std::vector<std::string> SplitLines(const std::string &text)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size()) {
		size_t nl = text.find('\n', start);
		std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(std::move(line));
		if (nl == std::string::npos)
			break;
		start = nl + 1;
	}
	return lines;
}

std::vector<std::string> SplitWhitespace(const std::string &line)
{
	std::vector<std::string> tokens;
	std::istringstream in(line);
	std::string tok;
	while (in >> tok)
		tokens.push_back(tok);
	return tokens;
}

bool IsHexToken(const std::string &s)
{
	if (s.empty())
		return false;
	for (char c : s) {
		if (!std::isxdigit(static_cast<unsigned char>(c)))
			return false;
	}
	return true;
}

const char *ExceptionName(uint32_t code)
{
	switch (code) {
	case 0xC0000005:
		return "ACCESS_VIOLATION";
	case 0xC00000FD:
		return "STACK_OVERFLOW";
	case 0xC0000094:
		return "INT_DIVIDE_BY_ZERO";
	case 0xC000001D:
		return "ILLEGAL_INSTRUCTION";
	case 0xC0000096:
		return "PRIVILEGED_INSTRUCTION";
	case 0xC0000409:
		return "STACK_BUFFER_OVERRUN";
	case 0xC0000374:
		return "HEAP_CORRUPTION";
	case 0xC0000006:
		return "IN_PAGE_ERROR";
	case 0xC0000017:
		return "NO_MEMORY";
	case 0xC0000025:
		return "NONCONTINUABLE_EXCEPTION";
	case 0xC000008C:
		return "ARRAY_BOUNDS_EXCEEDED";
	case 0xE06D7363:
		return "CPP_EXCEPTION";
	case 0x80000003:
		return "BREAKPOINT";
	case 0x80000001:
		return "GUARD_PAGE";
	default:
		return "";
	}
}

std::string ReadFileLimited(const std::string &path, size_t max_bytes)
{
	std::string out;
	FILE *f = os_fopen(path.c_str(), "rb");
	if (!f)
		return out;
	char buf[8192];
	while (out.size() < max_bytes) {
		const size_t want = std::min(sizeof(buf), max_bytes - out.size());
		const size_t n = fread(buf, 1, want, f);
		if (n == 0)
			break;
		out.append(buf, n);
	}
	fclose(f);
	return out;
}

bool WriteFileSafe(const std::string &path, const std::string &content)
{
	// temp-file + rename so a crash mid-write can't leave truncated state that
	// would make every previously reported crash look unreported.
	return os_quick_write_utf8_file_safe(path.c_str(), content.data(), content.size(), false, "tmp", nullptr);
}

std::string JoinPath(const std::string &dir, const std::string &name)
{
	if (dir.empty())
		return name;
	const char last = dir.back();
	if (last == '/' || last == '\\')
		return dir + name;
	return dir + "/" + name;
}

#ifdef _WIN32
const char *const kFallbackOs = "Windows x64";
#elif defined(__APPLE__)
const char *const kFallbackOs = "macOS";
#else
const char *const kFallbackOs = "Linux";
#endif

size_t WriteResponse(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *buf = static_cast<std::string *>(userdata);
	const size_t len = size * nmemb;
	if (buf->size() + len > kMaxResponseSize)
		return 0;
	buf->append(ptr, len);
	return len;
}

struct StateEntry {
	std::string fingerprint;
	int attempts = 0;
	bool done = false;
	std::string outcome;
	int64_t updated_at = 0;
};

using StateMap = std::map<std::string, StateEntry>;

StateMap LoadReportedState(const std::string &path)
{
	StateMap state;
	const std::string text = ReadFileLimited(path, 1024 * 1024);
	if (text.empty())
		return state;
	try {
		auto j = nlohmann::json::parse(text);
		if (!j.is_object() || !j.contains("entries") || !j["entries"].is_object())
			return state;
		for (auto it = j["entries"].begin(); it != j["entries"].end(); ++it) {
			if (!it.value().is_object())
				continue;
			StateEntry e;
			e.fingerprint = it.value().value("fingerprint", "");
			e.attempts = it.value().value("attempts", 0);
			e.done = it.value().value("done", false);
			e.outcome = it.value().value("outcome", "");
			e.updated_at = it.value().value("updatedAt", static_cast<int64_t>(0));
			state[it.key()] = std::move(e);
		}
	} catch (const std::exception &e) {
		inc_log(LOG_WARNING, "ignoring unreadable crash-report state: %s", e.what());
		state.clear();
	}
	return state;
}

void SaveReportedState(const std::string &path, const StateMap &state)
{
	try {
		nlohmann::json entries = nlohmann::json::object();
		for (const auto &kv : state) {
			nlohmann::json e;
			e["fingerprint"] = kv.second.fingerprint;
			e["attempts"] = kv.second.attempts;
			e["done"] = kv.second.done;
			e["outcome"] = kv.second.outcome;
			e["updatedAt"] = kv.second.updated_at;
			entries[kv.first] = std::move(e);
		}
		nlohmann::json root;
		root["version"] = 1;
		root["entries"] = std::move(entries);
		if (!WriteFileSafe(path, DumpJson(root)))
			inc_log(LOG_WARNING, "failed to persist crash-report state to %s", path.c_str());
	} catch (const std::exception &e) {
		inc_log(LOG_WARNING, "failed to serialize crash-report state: %s", e.what());
	}
}

struct CrashCandidate {
	std::string name;
	std::string path;
	int64_t crashed_at = 0;
	int64_t name_time = 0; // 0 when the file name did not parse
};

std::string FormatOsFromWindowsLine(const std::string &line)
{
	// "Windows version: 10.0 build 26200 (release: 25H2; revision: 1; 64-bit)"
	int major = 0, minor = 0, build = 0;
	if (sscanf(line.c_str(), "Windows version: %d.%d build %d", &major, &minor, &build) != 3)
		return "";
	const bool x64 = line.find("64-bit") != std::string::npos;
	const bool x86 = !x64 && line.find("32-bit") != std::string::npos;
	char buf[96];
	const char *product = (major == 10 && build >= 22000) ? "Windows 11" : (major == 10 ? "Windows 10" : "Windows");
	snprintf(buf, sizeof(buf), "%s %d.%d.%d%s", product, major, minor, build, x64 ? " x64" : (x86 ? " x86" : ""));
	return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Generic helpers
// ---------------------------------------------------------------------------

std::string TruncateUtf8(const std::string &s, size_t max_bytes)
{
	if (s.size() <= max_bytes)
		return s;
	size_t cut = max_bytes;
	// Back up over UTF-8 continuation bytes so a multi-byte char isn't split.
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
		cut--;
	return s.substr(0, cut);
}

std::string GzipToBase64(const std::string &text)
{
	if (text.empty())
		return "";

	z_stream zs{};
	if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		return "";

	std::string compressed;
	compressed.resize(deflateBound(&zs, static_cast<uLong>(text.size())));
	zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(text.data()));
	zs.avail_in = static_cast<uInt>(text.size());
	zs.next_out = reinterpret_cast<Bytef *>(compressed.data());
	zs.avail_out = static_cast<uInt>(compressed.size());

	const int ret = deflate(&zs, Z_FINISH);
	if (ret != Z_STREAM_END) {
		deflateEnd(&zs);
		return "";
	}
	compressed.resize(compressed.size() - zs.avail_out);
	deflateEnd(&zs);

	if (compressed.empty() || compressed.size() > kMaxDumpGzBytes)
		return "";

	std::string encoded;
	encoded.resize(4 * ((compressed.size() + 2) / 3) + 1);
	const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
				      reinterpret_cast<const unsigned char *>(compressed.data()),
				      static_cast<int>(compressed.size()));
	if (n <= 0)
		return "";
	encoded.resize(static_cast<size_t>(n));
	return encoded;
}

std::string FormatRfc3339Utc(int64_t unix_utc)
{
	const std::time_t t = static_cast<std::time_t>(unix_utc);
	std::tm tm{};
#ifdef _WIN32
	if (gmtime_s(&tm, &t) != 0)
		return "";
#else
	if (!gmtime_r(&t, &tm))
		return "";
#endif
	char buf[32] = {};
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

// ---------------------------------------------------------------------------
// Reconnect reports
// ---------------------------------------------------------------------------

const char *ReconnectReasonName(ReconnectReason reason)
{
	switch (reason) {
	case ReconnectReason::PpmmxReconnectCommand:
		return "ppmmx_reconnect_command";
	case ReconnectReason::HighPacketLoss:
		return "high_packet_loss";
	case ReconnectReason::SrtConnectionLost:
		return "srt_connection_lost";
	case ReconnectReason::WhipConnectionLost:
		return "whip_connection_lost";
	case ReconnectReason::ErtmpConnectionLost:
		return "ertmp_connection_lost";
	case ReconnectReason::PpcenterTokenRefreshFailed:
		return "ppcenter_token_refresh_failed";
	case ReconnectReason::NetworkChanged:
		return "network_changed";
	case ReconnectReason::Unknown:
	default:
		return "unknown";
	}
}

ReconnectReason ReconnectReasonFromName(const std::string &name)
{
	static const ReconnectReason all[] = {
		ReconnectReason::PpmmxReconnectCommand,    ReconnectReason::HighPacketLoss,
		ReconnectReason::SrtConnectionLost,        ReconnectReason::WhipConnectionLost,
		ReconnectReason::ErtmpConnectionLost,      ReconnectReason::PpcenterTokenRefreshFailed,
		ReconnectReason::NetworkChanged,
	};
	for (ReconnectReason r : all) {
		if (name == ReconnectReasonName(r))
			return r;
	}
	return ReconnectReason::Unknown;
}

std::string DeriveStreamNameFromServerUrl(const std::string &url, const std::string &app_id)
{
	auto percent_decode = [](const std::string &in) {
		std::string out;
		for (size_t i = 0; i < in.size(); i++) {
			if (in[i] == '%' && i + 2 < in.size() + 0 && std::isxdigit((unsigned char)in[i + 1]) &&
			    std::isxdigit((unsigned char)in[i + 2])) {
				out.push_back((char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
				i += 2;
			} else {
				out.push_back(in[i]);
			}
		}
		return out;
	};

	std::string value;
	const size_t sid = url.find("streamid=");
	if (sid != std::string::npos) {
		const size_t start = sid + strlen("streamid=");
		const size_t end = url.find('&', start);
		value = percent_decode(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (StartsWith(value, "publish:"))
			value = value.substr(strlen("publish:"));
		if (!app_id.empty() && StartsWith(value, (app_id + "/").c_str()))
			value = value.substr(app_id.size() + 1);
		else if (const size_t slash = value.find_last_of('/'); slash != std::string::npos)
			value = value.substr(slash + 1);
		return Trim(value);
	}

	// scheme://host[:port]/<appId>/<stream>[?query]
	const size_t scheme_end = url.find("://");
	if (scheme_end == std::string::npos)
		return "";
	size_t path_start = url.find('/', scheme_end + 3);
	if (path_start == std::string::npos)
		return "";
	size_t path_end = url.find_first_of("?#", path_start);
	std::string path = url.substr(path_start, path_end == std::string::npos ? std::string::npos : path_end - path_start);
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	const size_t slash = path.find_last_of('/');
	if (slash == std::string::npos || slash + 1 >= path.size())
		return "";
	return percent_decode(path.substr(slash + 1));
}

std::string BuildReconnectReportJson(const IncidentTarget &target, const ReconnectEvent &event,
				     const std::string &occurred_at, const std::string &client_version)
{
	try {
		nlohmann::json body;
		body["appId"] = target.app_id;
		body["appSecret"] = target.app_secret;
		if (!target.device_id.empty())
			body["deviceId"] = target.device_id;
		body["streamName"] = TruncateUtf8(target.stream_name, kMaxIdentBytes);
		body["occurredAt"] = occurred_at;
		body["reason"] = ReconnectReasonName(event.reason);
		if (!event.detail.empty())
			body["detail"] = TruncateUtf8(event.detail, kMaxDetailBytes);
		if (!target.protocol.empty())
			body["protocol"] = target.protocol;
		if (!event.codec.empty())
			body["codec"] = event.codec;
		if (event.loss_pct >= 0.0)
			body["lossPct"] = event.loss_pct;
		if (event.bitrate_kbps >= 0)
			body["bitrateKbps"] = event.bitrate_kbps;
		if (event.latency_ms >= 0)
			body["latencyMs"] = event.latency_ms;
		if (event.reconnect_ms >= 0)
			body["reconnectMs"] = event.reconnect_ms;
		if (event.success >= 0)
			body["success"] = event.success != 0;
		if (!client_version.empty())
			body["clientVersion"] = TruncateUtf8(client_version, kMaxIdentBytes);
		return DumpJson(body);
	} catch (const std::exception &e) {
		inc_log(LOG_WARNING, "failed to build reconnect report JSON: %s", e.what());
		return "";
	}
}

bool ReconnectDedup::ShouldReport(const std::string &stream_key, ReconnectReason reason, int64_t now_ms)
{
	auto any = last_any_ms.find(stream_key);
	if (any != last_any_ms.end() && now_ms - any->second < kWindowMs)
		return false;

	if (reason == ReconnectReason::PpcenterTokenRefreshFailed) {
		auto tok = last_token_ms.find(stream_key);
		if (tok != last_token_ms.end() && now_ms - tok->second < kTokenRefreshWindowMs)
			return false;
	}

	recent_ms.erase(std::remove_if(recent_ms.begin(), recent_ms.end(),
				       [&](int64_t t) { return now_ms - t >= kGlobalWindowMs; }),
			recent_ms.end());
	if (recent_ms.size() >= kGlobalMaxPerWindow)
		return false;

	recent_ms.push_back(now_ms);
	last_any_ms[stream_key] = now_ms;
	if (reason == ReconnectReason::PpcenterTokenRefreshFailed)
		last_token_ms[stream_key] = now_ms;
	return true;
}

// ---------------------------------------------------------------------------
// Crash log parsing
// ---------------------------------------------------------------------------

bool ParseCrashFileNameTime(const std::string &file_name, int64_t *unix_utc)
{
	int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
	int consumed = 0;
	// GenerateTimeDateFilename(): "%d-%02d-%02d %02d-%02d-%02d.txt", local time.
	if (sscanf(file_name.c_str(), "Crash %d-%d-%d %d-%d-%d%n", &y, &mo, &d, &h, &mi, &s, &consumed) < 6)
		return false;
	if (y < 2000 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
	    s < 0 || s > 60)
		return false;
	std::tm tm{};
	tm.tm_year = y - 1900;
	tm.tm_mon = mo - 1;
	tm.tm_mday = d;
	tm.tm_hour = h;
	tm.tm_min = mi;
	tm.tm_sec = s;
	tm.tm_isdst = -1;
	const std::time_t t = std::mktime(&tm);
	if (t == static_cast<std::time_t>(-1))
		return false;
	if (unix_utc)
		*unix_utc = static_cast<int64_t>(t);
	return true;
}

ParsedCrashLog ParseCrashLogText(const std::string &text)
{
	ParsedCrashLog out;
	try {
		const std::vector<std::string> lines = SplitLines(text);
		std::string first_line;
		for (const std::string &raw : lines) {
			const std::string line = Trim(raw);
			if (line.empty())
				continue;
			if (first_line.empty())
				first_line = line;

			if (out.exc_code.empty() && StartsWith(line, "Unhandled exception:")) {
				const std::string tok = Trim(line.substr(strlen("Unhandled exception:")));
				if (IsHexToken(tok) && tok.size() <= 8) {
					const uint32_t code = static_cast<uint32_t>(strtoul(tok.c_str(), nullptr, 16));
					char buf[16];
					snprintf(buf, sizeof(buf), "0x%08X", code);
					out.exc_code = buf;
					out.exception_name = ExceptionName(code);
				}
			} else if (out.fault_module.empty() && StartsWith(line, "Fault address:")) {
				const size_t open = line.rfind('(');
				const size_t close = line.rfind(')');
				if (open != std::string::npos && close != std::string::npos && close > open + 1)
					out.fault_module = Trim(line.substr(open + 1, close - open - 1));
			} else if (out.os.empty() && StartsWith(line, "Windows version:")) {
				out.os = FormatOsFromWindowsLine(line);
			}
		}

		// Crashed-thread stack: the "Thread XXXX: (Crashed)" block that the
		// Windows handler writes first, rows of six hex columns then
		// "module!symbol+0xoffset". Only the trailing symbol part is kept.
		std::vector<std::string> frames;
		for (size_t i = 0; i < lines.size() && frames.size() < kMaxStackFrames; i++) {
			if (!StartsWith(lines[i], "Thread ") || lines[i].find("(Crashed)") == std::string::npos)
				continue;
			for (size_t j = i + 1; j < lines.size() && frames.size() < kMaxStackFrames; j++) {
				const std::string row = Trim(lines[j]);
				if (row.empty())
					break;
				if (StartsWith(row, "Stack"))
					continue;
				const std::vector<std::string> toks = SplitWhitespace(row);
				if (toks.size() < 7)
					continue;
				bool hex6 = true;
				for (size_t k = 0; k < 6; k++)
					hex6 = hex6 && IsHexToken(toks[k]);
				if (!hex6)
					continue;
				std::string frame = toks[6];
				for (size_t k = 7; k < toks.size(); k++)
					frame += " " + toks[k];
				frames.push_back(std::move(frame));
			}
			break;
		}

		if (!frames.empty()) {
			out.top_frame = frames.front();
			// The faulting frame is often an unsymbolized system DLL ("msvcp140.dll!0x7ffe...");
			// the first frame with a real symbol is what points at our code.
			for (const std::string &f : frames) {
				const size_t bang = f.find('!');
				if (bang != std::string::npos && f.compare(bang + 1, 2, "0x") != 0) {
					out.symbol_frame = f;
					break;
				}
			}
			std::string joined;
			for (const std::string &f : frames) {
				if (!joined.empty())
					joined += "\n";
				joined += f;
			}
			out.stack = TruncateUtf8(joined, kMaxStackBytes);
		} else {
			// Foreign/unknown format: ship the head of the log, which is where
			// any handler puts its headline and first frames.
			out.stack = TruncateUtf8(text, kMaxStackBytes);
		}

		std::string summary;
		if (!out.exc_code.empty()) {
			summary = "Unhandled exception: ";
			if (!out.exception_name.empty())
				summary += out.exception_name + " ";
			summary += "(" + out.exc_code + ")";
			if (!out.top_frame.empty()) {
				summary += " at " + out.top_frame;
				if (!out.symbol_frame.empty() && out.symbol_frame != out.top_frame)
					summary += " via " + out.symbol_frame;
			} else if (!out.fault_module.empty())
				summary += " at " + out.fault_module;
		} else if (!first_line.empty()) {
			summary = "Crash log: " + first_line;
		} else {
			summary = "Crash log (empty or unreadable)";
		}
		out.summary = TruncateUtf8(summary, kMaxSummaryBytes);
	} catch (const std::exception &) {
		// Malformed input must never take the reporter (or OBS) down.
		if (out.summary.empty())
			out.summary = "Crash log (unparseable)";
	}
	return out;
}

std::string CrashFingerprint(const std::string &device_id, const std::string &file_name, int64_t name_time_unix)
{
	const std::string material = device_id + "|" + file_name + "|" + std::to_string(name_time_unix);
	unsigned char md[EVP_MAX_MD_SIZE] = {};
	unsigned int len = 0;
	if (EVP_Digest(material.data(), material.size(), md, &len, EVP_sha256(), nullptr) != 1 || len < 16)
		return "";
	static const char hex[] = "0123456789abcdef";
	std::string out;
	for (unsigned int i = 0; i < 16; i++) { // 16 bytes = 32 hex chars
		out.push_back(hex[md[i] >> 4]);
		out.push_back(hex[md[i] & 0xF]);
	}
	return out;
}

std::string BuildCrashReportJson(const CrashReportFields &f)
{
	try {
		nlohmann::json body;
		body["appId"] = f.app_id;
		body["appSecret"] = f.app_secret;
		body["deviceId"] = f.device_id;
		body["fingerprint"] = f.fingerprint;
		body["crashedAt"] = FormatRfc3339Utc(f.crashed_at_unix);
		body["clientVersion"] = TruncateUtf8(f.client_version, kMaxIdentBytes);
		const std::string &os = f.parsed.os.empty() ? f.os : f.parsed.os;
		body["os"] = TruncateUtf8(os, kMaxIdentBytes);
		body["exceptionCode"] = f.parsed.exc_code;
		body["summary"] = TruncateUtf8(f.parsed.summary, kMaxSummaryBytes);
		body["stack"] = TruncateUtf8(f.parsed.stack, kMaxStackBytes);
		if (!f.dump_gz_base64.empty())
			body["dumpGzBase64"] = f.dump_gz_base64;
		if (!f.last_app_id.empty())
			body["lastAppId"] = TruncateUtf8(f.last_app_id, kMaxIdentBytes);
		if (!f.last_stream.empty())
			body["lastStream"] = TruncateUtf8(f.last_stream, kMaxIdentBytes);
		if (!f.last_protocol.empty())
			body["lastProtocol"] = f.last_protocol;
		return DumpJson(body);
	} catch (const std::exception &e) {
		inc_log(LOG_WARNING, "failed to build crash report JSON: %s", e.what());
		return "";
	}
}

// ---------------------------------------------------------------------------
// Crash scan + persisted "already reported" state
// ---------------------------------------------------------------------------

void RecordLastPublish(const std::string &crash_dir, const std::string &app_id, const std::string &stream_name,
		       const std::string &protocol, bool active, int64_t now_unix)
{
	if (crash_dir.empty())
		return;
	const std::string path = JoinPath(crash_dir, kLastPublishStateFile);
	try {
		nlohmann::json j;
		if (active) {
			j["appId"] = TruncateUtf8(app_id, kMaxIdentBytes);
			j["stream"] = TruncateUtf8(stream_name, kMaxIdentBytes);
			j["protocol"] = protocol;
			j["startedAt"] = now_unix;
			j["active"] = true;
		} else {
			const std::string text = ReadFileLimited(path, 64 * 1024);
			if (text.empty())
				return; // nothing recorded, nothing to deactivate
			j = nlohmann::json::parse(text);
			if (!j.is_object())
				return;
			j["active"] = false;
			j["stoppedAt"] = now_unix;
		}
		WriteFileSafe(path, DumpJson(j));
	} catch (const std::exception &e) {
		inc_log(LOG_DEBUG, "last-publish record skipped: %s", e.what());
	}
}

size_t ProcessCrashLogs(const CrashScanOptions &options, const CrashScanContext &context)
{
	if (options.crash_dir.empty() || !context.post)
		return 0;

	std::vector<CrashCandidate> found;
	os_dir_t *dir = os_opendir(options.crash_dir.c_str());
	if (!dir)
		return 0; // no crashes directory = no crashes
	struct os_dirent *entry;
	while ((entry = os_readdir(dir)) != nullptr) {
		if (entry->directory || entry->d_name[0] == '.')
			continue;
		const std::string name = entry->d_name;
		if (!StartsWith(name, "Crash ") || name.size() < 10 || name.compare(name.size() - 4, 4, ".txt") != 0)
			continue;
		CrashCandidate c;
		c.name = name;
		c.path = JoinPath(options.crash_dir, name);
		int64_t t = 0;
		if (ParseCrashFileNameTime(name, &t)) {
			c.name_time = t;
			c.crashed_at = t;
		} else {
			struct stat st;
			if (os_stat(c.path.c_str(), &st) != 0)
				continue;
			c.crashed_at = static_cast<int64_t>(st.st_mtime);
		}
		found.push_back(std::move(c));
	}
	os_closedir(dir);

	const std::string state_path = JoinPath(options.crash_dir, kCrashReportedStateFile);
	StateMap state = LoadReportedState(state_path);

	// Forget entries whose crash log was deleted (OBS rotates old logs), so
	// the state file does not grow forever.
	bool state_dirty = false;
	for (auto it = state.begin(); it != state.end();) {
		const bool present = std::any_of(found.begin(), found.end(),
						 [&](const CrashCandidate &c) { return c.name == it->first; });
		if (!present) {
			it = state.erase(it);
			state_dirty = true;
		} else {
			++it;
		}
	}

	const int64_t oldest_allowed = options.now_unix - static_cast<int64_t>(options.max_age_days) * 86400;
	std::vector<CrashCandidate> todo;
	for (const CrashCandidate &c : found) {
		auto s = state.find(c.name);
		if (s != state.end() && s->second.done)
			continue;
		if (c.crashed_at < oldest_allowed)
			continue;
		todo.push_back(c);
	}
	std::sort(todo.begin(), todo.end(),
		  [](const CrashCandidate &a, const CrashCandidate &b) { return a.crashed_at > b.crashed_at; });
	if (todo.size() > options.max_reports)
		todo.resize(options.max_reports);

	if (todo.empty()) {
		if (state_dirty)
			SaveReportedState(state_path, state);
		return 0;
	}

	const std::string device_id = context.device_id ? context.device_id() : std::string();
	if (device_id.empty()) {
		inc_log(LOG_WARNING, "crash report skipped: no device id available (will retry next startup)");
		if (state_dirty)
			SaveReportedState(state_path, state);
		return 0;
	}

	// Which publish (if any) was live when the previous session died.
	std::string last_app_id, last_stream, last_protocol;
	int64_t last_started_at = 0;
	bool last_active = false;
	{
		const std::string text = ReadFileLimited(JoinPath(options.crash_dir, kLastPublishStateFile), 64 * 1024);
		if (!text.empty()) {
			try {
				auto j = nlohmann::json::parse(text);
				if (j.is_object()) {
					last_app_id = j.value("appId", "");
					last_stream = j.value("stream", "");
					last_protocol = j.value("protocol", "");
					last_started_at = j.value("startedAt", static_cast<int64_t>(0));
					last_active = j.value("active", false);
				}
			} catch (const std::exception &) {
			}
		}
	}

	size_t sent = 0;
	for (const CrashCandidate &c : todo) {
		StateEntry &entry = state[c.name];
		const std::string text = ReadFileLimited(c.path, options.max_log_bytes);

		CrashReportFields fields;
		fields.app_id = context.app_id;
		fields.app_secret = context.app_secret;
		fields.device_id = device_id;
		fields.fingerprint = CrashFingerprint(device_id, c.name, c.name_time);
		fields.crashed_at_unix = c.crashed_at;
		fields.client_version = context.client_version;
		fields.os = context.fallback_os.empty() ? kFallbackOs : context.fallback_os;
		fields.parsed = ParseCrashLogText(text);
		// Archive the complete crash log, not just the summary/stack fields.
		// The server stores it and puts the download URL in the incident's
		// detail field. Best-effort: an unsupported host still reports.
		fields.dump_gz_base64 = GzipToBase64(text);
		if (last_active && last_started_at != 0 && c.crashed_at >= last_started_at) {
			fields.last_app_id = last_app_id;
			fields.last_stream = last_stream;
			fields.last_protocol = last_protocol;
		}
		entry.fingerprint = fields.fingerprint;

		const std::string body = BuildCrashReportJson(fields);
		PostResult result;
		if (fields.fingerprint.empty() || body.empty()) {
			// Cannot ever be built for this file: do not retry forever.
			result.outcome = PostOutcome::Rejected;
			result.note = "could not build report";
		} else {
			result = context.post(body);
		}

		entry.updated_at = options.now_unix;
		switch (result.outcome) {
		case PostOutcome::Ok:
			entry.done = true;
			entry.outcome = "ok";
			sent++;
			inc_log(LOG_INFO, "crash log '%s' reported (%s)", c.name.c_str(), result.note.c_str());
			break;
		case PostOutcome::Rejected:
			entry.done = true;
			entry.outcome = "rejected";
			inc_log(LOG_WARNING, "crash log '%s' rejected by ppcenter, not retrying (%s)", c.name.c_str(),
				result.note.c_str());
			break;
		case PostOutcome::Retry:
		default:
			entry.attempts++;
			if (entry.attempts >= options.max_attempts) {
				entry.done = true;
				entry.outcome = "gave_up";
				inc_log(LOG_WARNING, "crash log '%s' not reported after %d attempts, giving up (%s)",
					c.name.c_str(), entry.attempts, result.note.c_str());
			} else {
				inc_log(LOG_WARNING, "crash log '%s' report failed (attempt %d/%d): %s", c.name.c_str(),
					entry.attempts, options.max_attempts, result.note.c_str());
			}
			break;
		}
		SaveReportedState(state_path, state);
		state_dirty = false;

		if (result.outcome == PostOutcome::Retry)
			break; // ppcenter unreachable/limiting: try again next startup
	}
	if (state_dirty)
		SaveReportedState(state_path, state);
	return sent;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

HttpPostResult PostJsonBlocking(const std::string &url, const std::string &json_body, long connect_timeout_sec,
				long total_timeout_sec)
{
	HttpPostResult result;
	CURL *curl = curl_easy_init();
	if (!curl) {
		result.error = "failed to init HTTP client";
		return result;
	}

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	const std::string user_agent = generate_user_agent();
	headers = curl_slist_append(headers, ("User-Agent: " + user_agent).c_str());

	char curl_error[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteResponse);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout_sec);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_timeout_sec);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	const CURLcode rc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK) {
		result.transport_ok = false;
		result.error = curl_error[0] ? curl_error : curl_easy_strerror(rc);
		return result;
	}
	result.transport_ok = true;
	return result;
}

PostResult ClassifyCrashPost(const HttpPostResult &http)
{
	PostResult r;
	if (!http.transport_ok) {
		r.outcome = PostOutcome::Retry;
		r.note = http.error.empty() ? "network error" : http.error;
		return r;
	}
	char status[32];
	snprintf(status, sizeof(status), "HTTP %ld", http.status);
	r.note = status;
	if (http.status == 200) {
		r.outcome = PostOutcome::Ok;
		try {
			auto j = nlohmann::json::parse(http.body);
			if (j.is_object() && j.value("duplicate", false))
				r.note += " duplicate";
		} catch (const std::exception &) {
		}
	} else if (http.status == 400) {
		r.outcome = PostOutcome::Rejected;
	} else {
		r.outcome = PostOutcome::Retry; // 401/402/429/5xx/anything else
	}
	return r;
}
