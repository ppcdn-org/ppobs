#include "incident-reporter.h"
#include "encoder-report.h"
#include "device-registration.h"
#include "nat-probe.h"
#include "ppcenter-time-sync-utils.h"

#include <obs-module.h>
#include <obs.hpp>
#include <util/dstr.h>
#include <util/platform.h>
#include <callback/calldata.h>
#include <callback/proc.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#define rep_log(level, format, ...) blog(level, "[obs-webrtc] [incident-reporter] " format, ##__VA_ARGS__)

namespace {

// Reconnect reports are best-effort evidence: short timeouts, and a slow or
// dead ppcenter must cost the stream nothing (everything here is on a
// detached thread).
constexpr long kReconnectConnectTimeoutSec = 3;
constexpr long kReconnectTotalTimeoutSec = 5;
constexpr long kCrashConnectTimeoutSec = 5;
constexpr long kCrashTotalTimeoutSec = 10;
constexpr int64_t kDeviceRegisterRetryMs = 60 * 1000;

std::mutex g_dedup_mutex;
ReconnectDedup g_dedup;

std::mutex g_device_cache_mutex;
std::string g_cached_device_id;
int64_t g_last_register_fail_ms = 0;
std::mutex g_device_register_mutex; // serializes the (blocking) registration

int64_t NowMs()
{
	return static_cast<int64_t>(os_gettime_ns() / 1000000ULL);
}

// The device id every other ppcenter call uses: the id from
// /v1/ppobs-devices/register, falling back to the generated per-installation
// client id exactly like WHIPOutput::Setup. May block on the network (up to
// the registration timeout), so only worker threads may call it.
std::string ResolveDeviceId(const IncidentTarget &target)
{
	{
		std::lock_guard<std::mutex> lk(g_device_cache_mutex);
		if (!g_cached_device_id.empty())
			return g_cached_device_id;
	}

	std::lock_guard<std::mutex> reg(g_device_register_mutex);
	{
		std::lock_guard<std::mutex> lk(g_device_cache_mutex);
		if (!g_cached_device_id.empty())
			return g_cached_device_id;
		if (g_last_register_fail_ms != 0 && NowMs() - g_last_register_fail_ms < kDeviceRegisterRetryMs)
			return GetOrCreateP2PClientId();
	}

	std::string id = RegisterPpobsDevice(target.publish_url, target.app_id, target.app_secret);
	std::lock_guard<std::mutex> lk(g_device_cache_mutex);
	if (!id.empty()) {
		g_cached_device_id = id;
		g_last_register_fail_ms = 0;
		return id;
	}
	g_last_register_fail_ms = NowMs();
	return GetOrCreateP2PClientId();
}

std::string ProtocolFromService(obs_service_t *service)
{
	const char *p = service ? obs_service_get_protocol(service) : nullptr;
	if (!p)
		return "";
	if (astrcmpi_n(p, "SRT", 3) == 0)
		return "srt";
	if (astrcmpi_n(p, "WHIP", 4) == 0)
		return "whip";
	if (astrcmpi_n(p, "RTMP", 4) == 0)
		return "ertmp";
	return "";
}

ReconnectReason LostReasonForProtocol(const std::string &protocol)
{
	if (protocol == "srt")
		return ReconnectReason::SrtConnectionLost;
	if (protocol == "whip")
		return ReconnectReason::WhipConnectionLost;
	if (protocol == "ertmp")
		return ReconnectReason::ErtmpConnectionLost;
	return ReconnectReason::Unknown;
}

const char *StrOrEmpty(const char *s)
{
	return s ? s : "";
}

bool IsKnownCodec(const char *codec)
{
	return codec && (!strcmp(codec, "h264") || !strcmp(codec, "hevc") || !strcmp(codec, "av1"));
}

// Snapshot of what this output is publishing: first video encoder's codec and
// the summed configured video bitrate (kbps) of all its video encoders.
void FillEncoderSnapshot(obs_output_t *output, ReconnectEvent &event)
{
	long long total_kbps = 0;
	for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		obs_encoder_t *enc = obs_output_get_video_encoder2(output, idx);
		if (!enc)
			continue;
		if (event.codec.empty()) {
			const char *codec = obs_encoder_get_codec(enc);
			if (IsKnownCodec(codec))
				event.codec = codec;
		}
		OBSDataAutoRelease settings = obs_encoder_get_settings(enc);
		if (settings)
			total_kbps += obs_data_get_int(settings, "bitrate");
	}
	if (event.bitrate_kbps < 0 && total_kbps > 0)
		event.bitrate_kbps = static_cast<int>(total_kbps);
}

// -- proc handlers -----------------------------------------------------------

void ProcReportReconnect(void *, calldata_t *cd)
{
	try {
		auto *output = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
		if (!output)
			return;

		const std::string protocol_hint = StrOrEmpty(calldata_string(cd, "protocol"));
		ReconnectEvent event;
		event.detail = StrOrEmpty(calldata_string(cd, "detail"));
		event.codec = StrOrEmpty(calldata_string(cd, "codec"));
		const double loss = calldata_float(cd, "loss_pct");
		event.loss_pct = loss >= 0.0 ? loss : -1;
		event.bitrate_kbps = static_cast<int>(calldata_int(cd, "bitrate_kbps"));
		event.latency_ms = static_cast<int>(calldata_int(cd, "latency_ms"));

		const std::string reason = StrOrEmpty(calldata_string(cd, "reason"));
		if (reason.empty()) {
			// "The connection of this output's protocol dropped" - the caller
			// (frontend's generic libobs reconnect hook) does not know which
			// protocol it is looking at, the service does.
			obs_service_t *service = obs_output_get_service(output);
			event.reason = LostReasonForProtocol(protocol_hint.empty() ? ProtocolFromService(service)
										 : protocol_hint);
		} else {
			event.reason = ReconnectReasonFromName(reason);
		}

		ReportReconnectForOutput(output, protocol_hint, std::move(event));
	} catch (...) {
	}
}

void ProcReportCrashes(void *, calldata_t *cd)
{
	bool started = false;
	try {
		const std::string crash_dir = StrOrEmpty(calldata_string(cd, "crash_dir"));
		auto *service = static_cast<obs_service_t *>(calldata_ptr(cd, "service"));
		started = ReportPendingCrashesAsync(crash_dir, service);
	} catch (...) {
	}
	calldata_set_bool(cd, "started", started);
}

void ProcRecordPublish(void *, calldata_t *cd)
{
	try {
		const std::string crash_dir = StrOrEmpty(calldata_string(cd, "crash_dir"));
		auto *service = static_cast<obs_service_t *>(calldata_ptr(cd, "service"));
		const bool active = calldata_bool(cd, "active");
		if (crash_dir.empty())
			return;
		if (!active) {
			RecordLastPublish(crash_dir, "", "", "", false, static_cast<int64_t>(std::time(nullptr)));
			return;
		}
		IncidentTarget target;
		if (!service || !ResolveIncidentTargetFromService(service, "", target))
			return;
		RecordLastPublish(crash_dir, target.app_id, target.stream_name, target.protocol, true,
				  static_cast<int64_t>(std::time(nullptr)));
	} catch (...) {
	}
}

} // namespace

void RegisterIncidentProcs()
{
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return;
	proc_handler_add(ph,
			 "void ppobs_report_reconnect(ptr output, string reason, string detail, string protocol, "
			 "string codec, float loss_pct, int bitrate_kbps, int latency_ms)",
			 ProcReportReconnect, nullptr);
	proc_handler_add(ph, "void ppobs_report_crashes(string crash_dir, ptr service, out bool started)",
			 ProcReportCrashes, nullptr);
	proc_handler_add(ph, "void ppobs_record_publish(string crash_dir, ptr service, bool active)",
			 ProcRecordPublish, nullptr);
}

bool ResolveIncidentTargetFromService(obs_service_t *service, const std::string &protocol_hint,
				      IncidentTarget &target)
{
	if (!service)
		return false;
	OBSDataAutoRelease settings = obs_service_get_settings(service);
	if (!settings)
		return false;

	// The setting may be the bare origin or the full publish endpoint; every
	// consumer normalizes it the same way (see DerivePpcenterPublishUrl).
	target.publish_url = DerivePpcenterPublishUrl(obs_data_get_string(settings, "ppcenter_url"));
	target.app_id = obs_data_get_string(settings, "ppcenter_appid");
	target.app_secret = obs_data_get_string(settings, "ppcenter_secret");
	if (target.publish_url.empty() || target.app_id.empty() || target.app_secret.empty())
		return false;

	target.protocol = protocol_hint.empty() ? ProtocolFromService(service) : protocol_hint;

	// SRT / RTMP: the stream name lives in the publish URL (the saved
	// "ppcenter_stream" is parsed WHIP-style and unreliable for them). WHIP:
	// "ppcenter_stream" is authoritative.
	const std::string configured_stream = obs_data_get_string(settings, "ppcenter_stream");
	std::string from_url;
	if (target.protocol != "whip") {
		const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		if (url && *url)
			from_url = DeriveStreamNameFromServerUrl(url, target.app_id);
	}
	target.stream_name = !from_url.empty() ? from_url : configured_stream;
	return true;
}

void SeedIncidentDeviceId(const std::string &device_id)
{
	if (device_id.empty())
		return;
	std::lock_guard<std::mutex> lk(g_device_cache_mutex);
	g_cached_device_id = device_id;
	g_last_register_fail_ms = 0;
}

void ReportReconnect(const IncidentTarget &target, const ReconnectEvent &event)
{
	try {
		if (target.publish_url.empty() || target.app_id.empty() || target.app_secret.empty() ||
		    target.stream_name.empty())
			return;
		const std::string url = DeriveReconnectReportUrl(target.publish_url);
		if (url.empty())
			return;

		{
			std::lock_guard<std::mutex> lk(g_dedup_mutex);
			if (!g_dedup.ShouldReport(target.app_id + "/" + target.stream_name, event.reason, NowMs())) {
				rep_log(LOG_DEBUG, "reconnect report (%s) suppressed (duplicate/rate limit)",
					ReconnectReasonName(event.reason));
				return;
			}
		}

		const std::string occurred_at = FormatRfc3339Utc(static_cast<int64_t>(std::time(nullptr)));
		IncidentTarget owned_target = target; // the thread outlives the caller's reference
		std::thread([url, owned_target, event, occurred_at]() mutable {
			try {
				if (owned_target.device_id.empty())
					owned_target.device_id = ResolveDeviceId(owned_target);
				const std::string body = BuildReconnectReportJson(owned_target, event, occurred_at,
										  obs_get_version_string());
				if (body.empty())
					return;
				const HttpPostResult http = PostJsonBlocking(url, body, kReconnectConnectTimeoutSec,
									      kReconnectTotalTimeoutSec);
				if (!http.transport_ok)
					rep_log(LOG_WARNING, "reconnect report (%s) failed: %s",
						ReconnectReasonName(event.reason), http.error.c_str());
				else if (http.status != 200)
					rep_log(LOG_WARNING, "reconnect report (%s) rejected by ppcenter (HTTP %ld)",
						ReconnectReasonName(event.reason), http.status);
				else
					rep_log(LOG_DEBUG, "reconnect reported (%s)", ReconnectReasonName(event.reason));
			} catch (...) {
			}
		}).detach();
	} catch (...) {
	}
}

void ReportReconnectForOutput(obs_output_t *output, const std::string &protocol_hint, ReconnectEvent event)
{
	try {
		if (!output)
			return;
		obs_service_t *service = obs_output_get_service(output);
		IncidentTarget target;
		if (!ResolveIncidentTargetFromService(service, protocol_hint, target))
			return;
		FillEncoderSnapshot(output, event);
		ReportReconnect(target, event);
	} catch (...) {
	}
}

bool ReportPendingCrashesAsync(const std::string &crash_dir, obs_service_t *service)
{
	static std::atomic<bool> started{false};
	if (started.load())
		return true;
	if (crash_dir.empty())
		return false;

	IncidentTarget target;
	if (!ResolveIncidentTargetFromService(service, "", target))
		return false; // ppcenter not configured yet: caller retries later
	const std::string report_url = DeriveCrashReportUrl(target.publish_url);
	if (report_url.empty())
		return false;

	bool expected = false;
	if (!started.compare_exchange_strong(expected, true))
		return true;

	std::thread([crash_dir, target, report_url]() {
		try {
			CrashScanOptions options;
			options.crash_dir = crash_dir;
			options.now_unix = static_cast<int64_t>(std::time(nullptr));

			CrashScanContext context;
			context.app_id = target.app_id;
			context.app_secret = target.app_secret;
			context.client_version = obs_get_version_string();
			context.device_id = [&target]() { return ResolveDeviceId(target); };
			context.post = [&report_url](const std::string &body) {
				return ClassifyCrashPost(PostJsonBlocking(report_url, body, kCrashConnectTimeoutSec,
									  kCrashTotalTimeoutSec));
			};

			const size_t sent = ProcessCrashLogs(options, context);
			if (sent > 0)
				rep_log(LOG_INFO, "reported %zu crash log(s) to ppcenter", sent);
		} catch (const std::exception &e) {
			rep_log(LOG_WARNING, "crash report scan failed: %s", e.what());
		} catch (...) {
		}
	}).detach();
	return true;
}
