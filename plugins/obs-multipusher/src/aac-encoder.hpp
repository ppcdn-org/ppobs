#pragma once

/// Registers a minimal AAC audio encoder ("mp_aac") built on the FFmpeg
/// libraries this plugin already links. It is registered as an internal
/// encoder (hidden from OBS' UI lists) and is used as a fallback when the host
/// has no AAC encoder registered at all, e.g. when the obs-ffmpeg plugin could
/// not be loaded on this machine.
void RegisterMultipusherAacEncoder();
