#pragma once

#include <obs.h>

#include <algorithm>
#include <cstdint>
#include <string>

// Hard publish limits shared by every ppobs transport (WHIP, SRT, ERTMP):
//   - 3840x2160 landscape / 2160x3840 portrait
//   - 60 FPS at 1080p and below, 30 FPS at 2K (2560x1440) and 4K
//   - 10000 Kbps video bitrate per encoder/layer
//
// WHIP enforces these inside WHIPOutput::Setup(); the frontend calls the
// same helpers before starting SRT/ERTMP (and any other output) so the
// limits cannot diverge between transports.
namespace ppobs_push_limits {

constexpr uint32_t kMaxLandscapeWidth = 3840;
constexpr uint32_t kMaxLandscapeHeight = 2160;
constexpr uint32_t kMaxPortraitWidth = 2160;
constexpr uint32_t kMaxPortraitHeight = 3840;
constexpr double kMaxFps = 60.0;
constexpr double kMaxFpsAt2KAnd4K = 30.0;
constexpr int64_t kMaxVideoBitrateKbps = 10000;

inline bool CheckEncoder(obs_encoder_t *encoder, std::string &error)
{
	if (!encoder)
		return true;

	const uint32_t width = obs_encoder_get_width(encoder);
	const uint32_t height = obs_encoder_get_height(encoder);
	const bool landscape = width >= height;
	const uint32_t maxWidth = landscape ? kMaxLandscapeWidth : kMaxPortraitWidth;
	const uint32_t maxHeight = landscape ? kMaxLandscapeHeight : kMaxPortraitHeight;
	if (width > maxWidth || height > maxHeight) {
		error = "resolution " + std::to_string(width) + "x" + std::to_string(height) +
			" exceeds the " + std::to_string(maxWidth) + "x" + std::to_string(maxHeight) +
			" limit for " + (landscape ? "landscape" : "portrait") + " streams";
		return false;
	}

	video_t *video = obs_encoder_video(encoder);
	if (!video)
		video = obs_get_video();
	const double fps = video ? video_output_get_frame_rate(video) : 0.0;
	const double maxFps = std::min(width, height) > 1080 ? kMaxFpsAt2KAnd4K : kMaxFps;
	if (fps > maxFps + 0.01) {
		error = "frame rate " + std::to_string(fps) + " exceeds the " +
			std::to_string((int)maxFps) + " FPS limit";
		return false;
	}

	obs_data_t *settings = obs_encoder_get_settings(encoder);
	const int64_t bitrate = settings ? obs_data_get_int(settings, "bitrate") : 0;
	if (settings)
		obs_data_release(settings);
	if (bitrate > kMaxVideoBitrateKbps) {
		error = "video bitrate " + std::to_string(bitrate) + " Kbps exceeds the " +
			std::to_string(kMaxVideoBitrateKbps) + " Kbps limit";
		return false;
	}

	return true;
}

// Checks every video encoder assigned to an output (the main encoder plus all
// Simulcast/HEVC layers).
inline bool CheckOutput(obs_output_t *output, std::string &error)
{
	if (!output)
		return true;

	for (size_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		obs_encoder_t *encoder = obs_output_get_video_encoder2(output, idx);
		if (!encoder)
			break;
		if (!CheckEncoder(encoder, error))
			return false;
	}
	return true;
}

} // namespace ppobs_push_limits
