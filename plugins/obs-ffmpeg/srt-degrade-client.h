#pragma once

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

#ifdef __cplusplus
}
#endif
