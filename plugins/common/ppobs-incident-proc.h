#pragma once

#include <stdbool.h>
#include <string.h>

#include <obs.h>
#include <callback/calldata.h>
#include <callback/proc.h>

/*
 * Call-site helper for ppobs' abnormal-reconnect reporting (obs-webrtc's
 * incident-reporter). The reporter lives in the obs-webrtc plugin; modules
 * that cannot link against it (obs-ffmpeg's SRT output) reach it through
 * libobs' global proc handler instead. Nothing here blocks or can fail the
 * caller: when the reporter is not loaded (or ppcenter is not configured on
 * the output's service) the call is a silent no-op, and when it is, the
 * actual HTTP POST happens on a detached worker thread.
 *
 * The reason is always chosen explicitly by the call site, never inferred.
 * Closed set accepted by ppcenter:
 */
#define PPOBS_RECONNECT_REASON_PPMMX_COMMAND "ppmmx_reconnect_command"
#define PPOBS_RECONNECT_REASON_HIGH_LOSS "high_packet_loss"
#define PPOBS_RECONNECT_REASON_SRT_LOST "srt_connection_lost"
#define PPOBS_RECONNECT_REASON_WHIP_LOST "whip_connection_lost"
#define PPOBS_RECONNECT_REASON_ERTMP_LOST "ertmp_connection_lost"
#define PPOBS_RECONNECT_REASON_TOKEN_REFRESH "ppcenter_token_refresh_failed"
#define PPOBS_RECONNECT_REASON_NETWORK_CHANGED "network_changed" /* no detection exists yet */
#define PPOBS_RECONNECT_REASON_UNKNOWN "unknown"

/* "srt" | "whip" | "ertmp"; NULL/"" lets the reporter derive it from the service */

/*
 * detail:       free text (<=512 chars on the wire), may be NULL
 * codec:        "h264"|"hevc"|"av1" or NULL to derive from the output's encoders
 * loss_pct:     percent, <0 when unavailable
 * bitrate_kbps: <0 to derive from the output's video encoders
 * latency_ms:   <0 when unavailable
 */
static inline void ppobs_report_reconnect(obs_output_t *output, const char *reason, const char *detail,
					  const char *protocol, const char *codec, double loss_pct, int bitrate_kbps,
					  int latency_ms)
{
	if (!output)
		return;
	proc_handler_t *ph = obs_get_proc_handler();
	if (!ph)
		return;

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "output", output);
	calldata_set_string(&cd, "reason", reason ? reason : "");
	calldata_set_string(&cd, "detail", detail ? detail : "");
	calldata_set_string(&cd, "protocol", protocol ? protocol : "");
	calldata_set_string(&cd, "codec", codec ? codec : "");
	calldata_set_float(&cd, "loss_pct", loss_pct);
	calldata_set_int(&cd, "bitrate_kbps", bitrate_kbps);
	calldata_set_int(&cd, "latency_ms", latency_ms);
	proc_handler_call(ph, "ppobs_report_reconnect", &cd);
	calldata_free(&cd);
}
