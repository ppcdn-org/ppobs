#pragma once

#include <stdbool.h>

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OBS-side executor for the SRT half of mmx's unified publish-degrade
 * protocol (see docs/design/publish-degrade-protocol.zh-CN.md), mirroring
 * plugins/obs-webrtc/degrade-client.cpp's WsDegradeClient for WHIP - but
 * scoped to exactly one SRT output per registration (an ffmpeg_mpegts_muxer
 * output is always single-codec, see obs-ffmpeg-mpegts.c's
 * setup_video_settings), so there is no multi-channel/shared-singleton
 * bookkeeping to do: every registered obs_output_t* gets its own
 * independent connection, keyed by that pointer. An HEVC/H264 multitrack
 * SRT publish is two such outputs (one per codec), each registering
 * separately.
 *
 * A bitrate-only TARGET_STATE is applied live via obs_encoder_update() across
 * every one of the output's video encoder slots. A layer-count change stops
 * and restarts the output; setup_video_settings() then calls
 * srt_degrade_target_layers() to trim the ladder down to the lowest N
 * layers before the muxer streams are (re)created.
 */

/* Registers/updates the degrade WS connection for this SRT output. Safe to
 * call on every (re)connect - an unchanged ws_url+token keeps the existing
 * connection; an empty ws_url or token clears it (no degrade channel for
 * this output, e.g. ppcenter didn't return a whipTracks entry for this
 * codec). codec is "h264"/"hevc", used only for logging. */
void srt_degrade_register(obs_output_t *output, const char *codec, const char *ws_url, const char *token);

/* Must be called once the output has fully stopped (see
 * obs-ffmpeg-mpegts.c's stop()). A no-op if this output was never
 * registered. */
void srt_degrade_unregister(obs_output_t *output);

/* Target layer count for this output's next (re)connect, or 0 when no
 * target has been received yet (keep every layer the frontend configured).
 * Read from setup_video_settings() before the muxer streams are built. */
int srt_degrade_target_layers(obs_output_t *output);

/* True while this output's degrade WS is registered AND currently connected
 * (not just reconnecting/backed-off). mmx's own receive-side ULR is already
 * the authority driving TARGET_STATE and - once the ladder bottoms out -
 * its own forced reconnect (raising SRT's TSBPD latency then closing the
 * connection, see docs/design/publish-degrade-protocol.zh-CN.md and
 * internal/degrade/degrade.go's terminal case). obs-ffmpeg-mpegts.c's local
 * loss-reconnect safety net (plugins/common/publish-loss-reconnect.h, fed
 * from this output's own sender-side srt_bstats()) is a *different*,
 * typically more pessimistic measurement of the same thing - see the
 * deployment notes on why SRT's sender-side accounting reads worse than
 * WHIP's receiver-side one for comparable real loss. Call this before
 * acting on that local monitor's verdict, and stand down whenever this
 * returns true: a connected degrade channel means mmx's own ULR is already
 * deciding when to reconnect, so there's no reason for OBS to also decide
 * from a different, noisier number. Falls back to the local monitor's own
 * judgment (returns false) when the channel was never registered, or is
 * currently down/reconnecting - e.g. an old mmx that never offers
 * whipTracks for this origin, or a transient WS outage - so a stream never
 * loses its only loss-driven safety net just because the control channel
 * happens to be the thing that's unavailable. */
bool srt_degrade_connected(obs_output_t *output);

#ifdef __cplusplus
}
#endif
