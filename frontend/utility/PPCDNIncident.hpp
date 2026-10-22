#pragma once

#include <string>

#include <obs.hpp>

// Frontend glue for ppobs' diagnostics reporting to PPCenter (crash logs and
// abnormal reconnects). The reporter itself lives in the obs-webrtc plugin
// (plugins/obs-webrtc/incident-reporter.cpp) and is reached through libobs'
// global proc handler, so the frontend needs no link-time dependency on it.
// Every helper here is a silent no-op when the plugin is not loaded or the
// service has no PPCenter configuration, and none of them blocks: the HTTP
// work runs on the reporter's own detached threads.

char *GetAppConfigPathPtr(const char *name);

namespace PPCDNIncident {

// Where OBS writes "Crash <date> <time>.txt" (honors portable mode because it
// goes through GetAppConfigPath).
inline std::string CrashDir()
{
	BPtr<char> path(GetAppConfigPathPtr("obs-studio/crashes"));
	return path ? std::string(path.Get()) : std::string();
}

// Starts the once-per-process background scan that reports unreported crash
// logs of previous sessions. Cheap and idempotent: call it whenever PPCenter
// config may have just become available (startup, before a publish); it does
// nothing until the service carries a complete PPCenter configuration.
inline void ReportPendingCrashes(obs_service_t *service)
{
	static bool started = false;
	if (started || !service)
		return;
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return;

	const std::string crashDir = CrashDir();
	if (crashDir.empty())
		return;

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "crash_dir", crashDir.c_str());
	calldata_set_ptr(&cd, "service", service);
	if (proc_handler_call(ph, "ppobs_report_crashes", &cd))
		started = calldata_bool(&cd, "started");
	calldata_free(&cd);
}

// Records which publish is live (active=true when one starts, false when it
// stops) so a later crash report can say what the publisher was doing.
inline void RecordPublish(obs_service_t *service, bool active)
{
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph || (active && !service))
		return;
	const std::string crashDir = CrashDir();
	if (crashDir.empty())
		return;

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "crash_dir", crashDir.c_str());
	calldata_set_ptr(&cd, "service", service);
	calldata_set_bool(&cd, "active", active);
	proc_handler_call(ph, "ppobs_record_publish", &cd);
	calldata_free(&cd);
}

// Generic "the publish connection dropped and libobs is reconnecting" report,
// for transports whose own code has no ppcenter hook (RTMP/eRTMP) and as a
// fallback for the others. The reporter picks srt/whip/ertmp_connection_lost
// from the output's service, and drops this report when the transport already
// reported a more specific reason for the same reconnect.
inline void ReportReconnect(obs_output_t *output)
{
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph || !output)
		return;

	const char *lastError = obs_output_get_last_error(output);

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "output", output);
	calldata_set_string(&cd, "reason", ""); // protocol-specific *_connection_lost
	calldata_set_string(&cd, "detail", lastError ? lastError : "");
	calldata_set_string(&cd, "protocol", "");
	calldata_set_string(&cd, "codec", "");
	calldata_set_float(&cd, "loss_pct", -1.0);
	calldata_set_int(&cd, "bitrate_kbps", -1);
	calldata_set_int(&cd, "latency_ms", -1);
	proc_handler_call(ph, "ppobs_report_reconnect", &cd);
	calldata_free(&cd);
}

} // namespace PPCDNIncident
