#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OBS-side control channel for RTMP/ERTMP publishes.
 *
 * RTMP carries no in-band keyframe request (no RTCP PLI/FIR), so mmx relays a
 * new viewer's request over the same out-of-band WebSocket used by the
 * degrade protocol. This client only listens for REQUEST_KEYFRAME and asks
 * the output's video encoders for an IDR; it deliberately does not implement
 * the degrade FSM (that remains on the WHIP/SRT paths).
 *
 * ws_url/token are the per-codec degrade credentials written into the
 * service settings by ResolvePPCDNPublishEndpoint. Empty values clear the
 * registration (no control channel for this publish).
 */
void rtmp_keyframe_register(obs_output_t *output, const char *ws_url, const char *token);

/* Must be called when the output stops. Idempotent. */
void rtmp_keyframe_unregister(obs_output_t *output);

#ifdef __cplusplus
}
#endif
