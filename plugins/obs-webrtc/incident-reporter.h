#pragma once

#include "incident-report.h"

#include <obs.h>

#include <string>

// obs-facing half of incident-report.h: resolves the ppcenter target from a
// service/output, owns the device id and the detached worker threads, and
// exposes everything to other modules (obs-ffmpeg, the frontend) through
// libobs' global proc handler so they need no link-time dependency on this
// plugin:
//
//   void ppobs_report_reconnect(ptr output, string reason, string detail,
//                               string protocol, string codec, float loss_pct,
//                               int bitrate_kbps, int latency_ms)
//   void ppobs_report_crashes(string crash_dir, ptr service)   [out: bool started]
//   void ppobs_record_publish(string crash_dir, ptr service, bool active)
//
// All three are silent no-ops when ppcenter is not configured on the service.
// See plugins/common/ppobs-incident-proc.h for the C-callable wrappers.
void RegisterIncidentProcs();

// Fills `target` from the service's ppcenter_* settings (publish URL, app id,
// secret, stream name, protocol). False when ppcenter is not configured.
// `protocol_hint` ("srt"|"whip"|"ertmp") wins over the service's own protocol.
bool ResolveIncidentTargetFromService(obs_service_t *service, const std::string &protocol_hint,
				      IncidentTarget &target);

// Fire-and-forget: dedups, then POSTs /v1/ppobs/reconnects from a detached
// worker thread with a short timeout. Never blocks, throws or affects the
// stream; failures are only logged.
void ReportReconnect(const IncidentTarget &target, const ReconnectEvent &event);

// Same, resolving the target from the output's service and filling the
// encoder snapshot (codec, summed video bitrate) when the event left them
// unset. `protocol_hint` may be empty.
void ReportReconnectForOutput(obs_output_t *output, const std::string &protocol_hint, ReconnectEvent event);

// Lets a caller that already resolved this device's id (whip-output's Setup)
// share it, so every report uses the same value as the publish request.
void SeedIncidentDeviceId(const std::string &device_id);

// Starts the once-per-process background scan that reports unreported crash
// logs in `crash_dir`. Returns false (and does nothing) while ppcenter config
// is incomplete so the caller can retry later; true once the scan is running
// or already ran.
bool ReportPendingCrashesAsync(const std::string &crash_dir, obs_service_t *service);
