#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Operator-facing diagnostics sent to ppcenter so a publisher's crash or
// abnormal reconnect can be explained later:
//
//   POST {ppcenter}/v1/ppobs/crashes     one report per OBS crash log
//   POST {ppcenter}/v1/ppobs/reconnects  one best-effort report per reconnect
//
// Both authenticate with appId/appSecret inside the JSON body, exactly like
// /v1/encoders/report (see encoder-report.h). This file holds the pure logic
// (parsing, JSON building, dedup, crash-log state machine, the blocking HTTP
// POST) so it can be unit-tested without OBS running; the obs-facing glue
// (service-settings lookup, device id, detached threads, proc handlers) lives
// in incident-reporter.cpp.

// ---------------------------------------------------------------------------
// Reconnect reports
// ---------------------------------------------------------------------------

// Closed set accepted by ppcenter. Call sites pass the enum explicitly so the
// reason is always decided where the reconnect is triggered, never guessed.
enum class ReconnectReason {
	PpmmxReconnectCommand,    // mmx TARGET_STATE with reconnect=true
	HighPacketLoss,           // local/mmx loss window crossed the threshold
	SrtConnectionLost,        // SRT broken / write failure / generic SRT drop
	WhipConnectionLost,       // WHIP peer connection failed / stalled
	ErtmpConnectionLost,      // RTMP/eRTMP drop (libobs reconnect signal)
	PpcenterTokenRefreshFailed,
	NetworkChanged, // reserved: ppobs has no network-change signal yet
	Unknown,
};

const char *ReconnectReasonName(ReconnectReason reason);
// Unknown / empty names map to ReconnectReason::Unknown.
ReconnectReason ReconnectReasonFromName(const std::string &name);

// Where to send, and who we are. Resolved from the output's service settings
// (see incident-reporter.cpp) or filled in directly by in-module callers.
struct IncidentTarget {
	std::string publish_url; // ppcenter publish URL, "https://host/v1/publish/requests"
	std::string app_id;
	std::string app_secret;
	std::string stream_name;
	std::string device_id; // empty -> resolved lazily on the worker thread
	std::string protocol;  // "srt" | "whip" | "ertmp"
};

struct ReconnectEvent {
	ReconnectReason reason = ReconnectReason::Unknown;
	std::string detail;     // free text, truncated to 512 chars on the wire
	std::string codec;      // "h264" | "hevc" | "av1" or empty
	double loss_pct = -1;   // <0 = unavailable
	int bitrate_kbps = -1;  // <0 = unavailable
	int latency_ms = -1;    // <0 = unavailable
	int reconnect_ms = -1;  // <0 = unavailable (reports are sent at trigger time)
	int success = -1;       // -1 unknown, 0 failed, 1 succeeded
};

// Stream name carried by a publish URL: SRT "...?streamid=publish:<appId>/<stream>"
// or an RTMP-style ".../<appId>/<stream>[?...]". "" when none can be found.
std::string DeriveStreamNameFromServerUrl(const std::string &url, const std::string &app_id);

// Pure: request body for /v1/ppobs/reconnects. Never throws; "" on failure.
std::string BuildReconnectReportJson(const IncidentTarget &target, const ReconnectEvent &event,
				     const std::string &occurred_at, const std::string &client_version);

// Suppresses duplicate reports for one actual reconnect. Two code paths can
// fire for the same reconnect (e.g. the SRT output reports a loss reconnect,
// then libobs' own "reconnect" signal reaches the UI): any second report for
// the same stream inside kWindowMs is dropped. ppcenter_token_refresh_failed
// can repeat every few seconds while ppcenter is unreachable, so it gets a
// much longer per-stream floor. A global cap also protects ppcenter's
// per-device rate limit if something loops.
class ReconnectDedup {
public:
	static constexpr int64_t kWindowMs = 3000;
	static constexpr int64_t kTokenRefreshWindowMs = 60000;
	static constexpr int64_t kGlobalWindowMs = 60000;
	static constexpr size_t kGlobalMaxPerWindow = 10;

	// True when this report should be sent (and records it as sent).
	bool ShouldReport(const std::string &stream_key, ReconnectReason reason, int64_t now_ms);

private:
	std::map<std::string, int64_t> last_any_ms;
	std::map<std::string, int64_t> last_token_ms;
	std::vector<int64_t> recent_ms;
};

// ---------------------------------------------------------------------------
// Crash reports
// ---------------------------------------------------------------------------

struct ParsedCrashLog {
	std::string exc_code; // "0xC0000005" or ""
	std::string exception_name; // "ACCESS_VIOLATION" or ""
	std::string fault_module;   // module named on the "Fault address" line
	std::string os;             // "Windows 11 10.0.26200 x64" or ""
	std::string top_frame;      // "obs-ffmpeg.dll!func+0x12" or ""
	std::string symbol_frame;   // first frame that has a symbol name, or ""
	std::string summary;        // <= 512 chars, one line
	std::string stack;          // <= 8000 chars, top ~30 frames
};

// Best-effort and total: any text (empty, binary, foreign format) yields a
// usable summary; it never throws.
ParsedCrashLog ParseCrashLogText(const std::string &text);

// "Crash 2026-10-22 08-30-00.txt" (local time, see main_crash_handler in
// obs-main.cpp) -> unix seconds UTC. False when the name does not match.
bool ParseCrashFileNameTime(const std::string &file_name, int64_t *unix_utc);

// RFC3339 UTC ("2026-10-22T08:30:00Z").
std::string FormatRfc3339Utc(int64_t unix_utc);

// First 32 lowercase hex chars of SHA-256(deviceId + "|" + fileName + "|" +
// crash timestamp parsed from the name, "0" when it does not parse).
// Identical for the same crash log on every retry.
std::string CrashFingerprint(const std::string &device_id, const std::string &file_name, int64_t name_time_unix);

struct CrashReportFields {
	std::string app_id;
	std::string app_secret;
	std::string device_id;
	std::string fingerprint;
	int64_t crashed_at_unix = 0;
	std::string client_version;
	std::string os; // fallback used when the log has none
	ParsedCrashLog parsed;
	std::string last_app_id;
	std::string last_stream;
	std::string last_protocol;
	// Base64(gzip(full crash log)); uploaded so ppcenter can archive the
	// complete file and return a download URL. Empty when compression failed.
	std::string dump_gz_base64;
};

// Pure: request body for /v1/ppobs/crashes. Never throws; "" on failure.
std::string BuildCrashReportJson(const CrashReportFields &fields);

// Truncates to at most max_bytes without splitting a UTF-8 sequence.
std::string TruncateUtf8(const std::string &s, size_t max_bytes);

// gzip(text) followed by standard base64. Empty on compression failure or
// when the compressed payload exceeds the server's artifact limit.
std::string GzipToBase64(const std::string &text);

enum class PostOutcome {
	Ok,       // HTTP 200 (including duplicate=true): never send again
	Rejected, // HTTP 400: permanently malformed, never send again
	Retry,    // network error / 401 / 402 / 429 / 5xx: try on the next startup
};

struct PostResult {
	PostOutcome outcome = PostOutcome::Retry;
	std::string note; // for logging: "HTTP 200 duplicate", "HTTP 503", curl error...
};

using PostFn = std::function<PostResult(const std::string &body)>;

struct CrashScanOptions {
	std::string crash_dir;
	int64_t now_unix = 0;
	int max_age_days = 30;
	size_t max_reports = 5;   // newest unreported logs handled per startup
	int max_attempts = 10;    // then the crash is marked done ("gave_up")
	size_t max_log_bytes = 1024 * 1024;
};

struct CrashScanContext {
	std::string app_id;
	std::string app_secret;
	std::string client_version;
	std::string fallback_os;
	// Called at most once, only when at least one crash needs sending.
	std::function<std::string()> device_id;
	PostFn post;
};

// State file next to the crash logs: crash_dir/.ppcenter-reported.json.
// Keyed by crash log file name (known before any network access) so a
// reported crash is skipped without needing the device id.
extern const char *const kCrashReportedStateFile;
// Last publish target, written when a publish starts and deactivated on a
// clean stop: crash_dir/.ppcenter-last-publish.json.
extern const char *const kLastPublishStateFile;

// Scans crash_dir for "Crash *.txt", reports the (up to max_reports) newest
// unreported ones from the last max_age_days and persists the outcome.
// Returns the number of crash logs sent successfully. Stops at the first
// retryable failure so an unreachable ppcenter is not hammered.
size_t ProcessCrashLogs(const CrashScanOptions &options, const CrashScanContext &context);

// Writes (active=true: new publish started) or deactivates (active=false:
// clean stop) the last-publish state file. Best-effort.
void RecordLastPublish(const std::string &crash_dir, const std::string &app_id, const std::string &stream_name,
		       const std::string &protocol, bool active, int64_t now_unix);

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

struct HttpPostResult {
	bool transport_ok = false; // false: curl-level failure (no HTTP status)
	long status = 0;
	std::string body;
	std::string error;
};

// Blocking JSON POST with explicit timeouts. Runs on the caller's thread:
// callers must be on a worker thread.
HttpPostResult PostJsonBlocking(const std::string &url, const std::string &json_body, long connect_timeout_sec,
				long total_timeout_sec);

// Maps an HTTP result to a crash-report PostResult (see PostOutcome).
PostResult ClassifyCrashPost(const HttpPostResult &http);
