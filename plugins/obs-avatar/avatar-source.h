/* The avatar livestream source.
 *
 * Why a source and not a filter is argued in §3 of
 * docs/avatar-lipsync-design.md: the avatar picture does not come from a
 * parent source, it is generated from audio; "bottom right, top layer" is
 * already what a scene item gives you; and an audio filter cannot output video
 * at all.
 *
 * Stage A's scope: render a still or looping avatar asset and prove the audio
 * tap works.  The lip-sync engines (mode A MuseTalk sidecar, mode B viseme
 * sprites) arrive in stages B and C, at which point avatar_source_render is
 * the only place that has to change - to draw the engine's texture instead.
 */

#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct obs_source_info avatar_source_info;

#ifdef __cplusplus
}
#endif
