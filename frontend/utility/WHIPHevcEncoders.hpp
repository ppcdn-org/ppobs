#pragma once

#include <utility/WHIPSimulcastEncoders.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// HEVC/H264 multitrack simulcast feature - see
// docs/design/whip-hevc-h264-multitrack-simulcast-design.zh-CN.md §4.1.
//
// Builds an independent HEVC encoder ladder (main layer + up to 2 scaled-
// down layers, i.e. at most 3 layers total - the design's confirmed HEVC
// Simulcast cap) that mirrors WHIPSimulcastEncoders' H264 ladder: same
// resolution step-down and proportional-bitrate formulas, just capped
// lower and always starting from its own dedicated HEVC encoder rather
// than reusing the main Stream encoder. The HEVC ladder's layer count
// follows Stream1.WHIPSimulcastTotalLayers the same as H264 does, clamped
// to WHIPHevcEncoders::kMaxLayers - so unchecking "Follow Main" per layer,
// resolution steps, and bitrate proportionality all behave identically to
// the H264 side; only the encoder identity and codec differ.

// Picks a HEVC encoder id to pair with the given H264 encoder id: prefers
// the same hardware vendor's HEVC encoder when the user's H264 pick is
// hardware-accelerated (matching encoder families tend to share the same
// underlying session limits and behavior), and falls back to the first
// HEVC encoder OBS has registered at all. Returns an empty string if no
// HEVC encoder is available - the caller must treat that as "cannot start
// multitrack", not fall back to H264 (see the design doc's §4.1 "对无法创
// 建 HEVC encoder 的情况，启动失败并明确提示").
static inline std::string ResolveWHIPHevcEncoderId(const std::string &h264EncoderId)
{
	struct VendorHint {
		const char *h264Substr;
		const char *hevcId;
	};
	// Ordered by how the underlying vendor plugins name their H264/HEVC
	// pairs (obs-qsv11, obs-nvenc, obs-amf on Windows; VideoToolbox on
	// macOS) - see plugins/obs-qsv11/obs-qsv11.c, plugins/obs-nvenc/nvenc.c,
	// plugins/obs-amf's registration, and translate_macvth264_encoder()
	// in AdvancedOutput.cpp for the matching VideoToolbox ids.
	static const VendorHint hints[] = {
		{"qsv", "obs_qsv11_hevc"},
		{"nvenc", "obs_nvenc_hevc_tex"},
		{"amf", "h265_texture_amf"},
		{"videotoolbox", "com.apple.videotoolbox.videoencoder.ave.hevc"},
	};

	auto encoderRegistered = [](const char *id) {
		size_t i = 0;
		const char *val;
		while (obs_enum_encoder_types(i++, &val))
			if (strcmp(val, id) == 0)
				return true;
		return false;
	};

	for (const auto &hint : hints) {
		if (h264EncoderId.find(hint.h264Substr) != std::string::npos && encoderRegistered(hint.hevcId))
			return hint.hevcId;
	}

	// No vendor match (e.g. software x264, or a hardware family with no
	// HEVC counterpart registered) - fall back to whatever HEVC encoder
	// OBS has, if any.
	size_t i = 0;
	const char *val;
	while (obs_enum_encoder_types(i++, &val)) {
		const char *codec = obs_get_encoder_codec(val);
		if (codec && strcmp(codec, "hevc") == 0 && obs_get_encoder_type(val) == OBS_ENCODER_VIDEO)
			return val;
	}
	return std::string();
}

struct WHIPHevcEncoders {
public:
	// HEVC Simulcast is capped at 3 layers total (design doc §11: "严格
	// 小于 4") - stricter than H264's cap in WHIPSimulcastEncoders.
	static constexpr int kMaxLayers = 3;

	// hevcEncoderId must be non-empty and already validated as an
	// available HEVC encoder (see ResolveWHIPHevcEncoderId) - this
	// struct does not fall back to H264 on failure, matching the
	// strong-consistency start requirement.
	void Create(const std::string &hevcEncoderId, obs_data_t *videoSettings, int rescaleFilter,
		    int requestedTotalLayers, uint32_t outputWidth, uint32_t outputHeight, config_t *config = nullptr)
	{
		if (rescaleFilter == OBS_SCALE_DISABLE)
			rescaleFilter = OBS_SCALE_BICUBIC;

		int totalLayers = std::min(requestedTotalLayers, kMaxLayers);
		if (totalLayers < 1)
			totalLayers = 1;

		OBSDataAutoRelease mainSettings =
			videoSettings ? obs_data_create_from_json(obs_data_get_json(videoSettings)) : nullptr;
		mainEncoder = obs_video_encoder_create(hevcEncoderId.c_str(), "whip_hevc_main", mainSettings, nullptr);
		if (!mainEncoder) {
			blog(LOG_ERROR, "Failed to create main HEVC encoder '%s' for WHIP HEVC/H264 multitrack",
			     hevcEncoderId.c_str());
			return;
		}
		obs_encoder_set_video(mainEncoder, obs_get_video());
		obs_encoder_release(mainEncoder);

		if (totalLayers <= 1)
			return;

		usingCustomLayer.assign(totalLayers, false);

		auto widthStep = outputWidth / totalLayers;
		auto heightStep = outputHeight / totalLayers;
		int videoBitrate = videoSettings ? static_cast<int>(obs_data_get_int(videoSettings, "bitrate")) : 0;
		std::string encoder_name = "whip_hevc_layer_0";

		// Same rid convention as WHIPSimulcastEncoders::Create(): slot
		// numbering (1, 2, ...) matches the H264 side's per-layer
		// config keys ("WHIPSimulcastLayer<slot>...") so a HEVC layer
		// and its equivalently-numbered H264 layer follow the same
		// user-configured resolution/bitrate ladder shape - see the
		// Settings UI, which has one set of per-layer rows shared by
		// both codecs.
		for (int i = totalLayers - 1; i > 0; i--) {
			encoder_name.back() = std::to_string(i).at(0);

			int slot = totalLayers - i;
			WHIPSimulcastLayer custom;
			bool haveCustom = GetWHIPSimulcastLayerConfig(config, slot, &custom);

			uint32_t width, height;
			if (haveCustom && !custom.followMain) {
				usingCustomLayer[i] = true;
				width = custom.width;
				height = custom.height;
			} else {
				width = widthStep * i;
				width -= width % 2;
				height = heightStep * i;
				height -= height % 2;
			}

			OBSDataAutoRelease layerSettings;
			if (videoSettings) {
				layerSettings = obs_data_create_from_json(obs_data_get_json(videoSettings));
				if (haveCustom && !custom.followMain) {
					obs_data_set_int(layerSettings, "bitrate", custom.bitrateKbps);
				} else {
					obs_data_set_int(layerSettings, "bitrate",
							  WHIPSimulcastProportionalBitrate(videoBitrate, outputWidth,
											    outputHeight, width, height));
				}

				const int64_t layerBitrate = obs_data_get_int(layerSettings, "bitrate");
				const int64_t mainMaxBitrate = obs_data_get_int(layerSettings, "max_bitrate");
				if (mainMaxBitrate > 0 && videoBitrate > 0) {
					int64_t layerMax = mainMaxBitrate * layerBitrate / videoBitrate;
					if (layerMax < layerBitrate)
						layerMax = layerBitrate;
					obs_data_set_int(layerSettings, "max_bitrate", layerMax);
				}
			}

			auto layerEncoder =
				obs_video_encoder_create(hevcEncoderId.c_str(), encoder_name.c_str(), layerSettings, nullptr);
			if (layerEncoder) {
				obs_encoder_set_video(layerEncoder, obs_get_video());
				obs_encoder_set_scaled_size(layerEncoder, width, height);
				obs_encoder_set_gpu_scale_type(layerEncoder, (obs_scale_type)rescaleFilter);
				hevcLayerEncoders.push_back(layerEncoder);
				obs_encoder_release(layerEncoder);
			} else {
				blog(LOG_WARNING, "Failed to create HEVC Simulcast layer encoder (WHIPHevcEncoders)");
			}
		}
	}

	void Update(obs_data_t *videoSettings, int videoBitrate, config_t *config = nullptr)
	{
		if (!mainEncoder)
			return;
		obs_encoder_update(mainEncoder, videoSettings);

		uint32_t outputWidth = video_output_get_width(obs_get_video());
		uint32_t outputHeight = video_output_get_height(obs_get_video());

		for (size_t idx = 0; idx < hevcLayerEncoders.size(); idx++) {
			OBSDataAutoRelease layerSettings = obs_data_create_from_json(obs_data_get_json(videoSettings));

			WHIPSimulcastLayer custom;
			int slot = static_cast<int>(idx) + 1;
			if (GetWHIPSimulcastLayerConfig(config, slot, &custom) && !custom.followMain) {
				obs_data_set_int(layerSettings, "bitrate", custom.bitrateKbps);
			} else {
				uint32_t layerWidth = obs_encoder_get_width(hevcLayerEncoders[idx]);
				uint32_t layerHeight = obs_encoder_get_height(hevcLayerEncoders[idx]);
				obs_data_set_int(layerSettings, "bitrate",
						  WHIPSimulcastProportionalBitrate(videoBitrate, outputWidth,
										    outputHeight, layerWidth, layerHeight));
			}

			obs_encoder_update(hevcLayerEncoders[idx], layerSettings);
		}
	}

	void SetVideoFormat(enum video_format format)
	{
		if (mainEncoder)
			obs_encoder_set_preferred_video_format(mainEncoder, format);
		for (auto enc : hevcLayerEncoders)
			obs_encoder_set_preferred_video_format(enc, format);
	}

	// See WHIPSimulcastEncoders::SetVideo()'s identical comment - rebinds
	// every encoder to the current video mix after a video reset.
	void SetVideo()
	{
		if (mainEncoder)
			obs_encoder_set_video(mainEncoder, obs_get_video());
		for (auto enc : hevcLayerEncoders)
			obs_encoder_set_video(enc, obs_get_video());
	}

	// Assigns this ladder to encoder slots starting at firstSlot -
	// distinct from H264's slots (0..N) so WHIPOutput::collectVideoLayers
	// can tell the two codecs' layers apart purely by
	// obs_encoder_get_codec() (see whip-output.cpp), without needing to
	// know slot ranges itself.
	void SetStreamOutput(obs_output_t *streamOutput, uint32_t firstSlot)
	{
		if (!mainEncoder)
			return;
		obs_output_set_video_encoder2(streamOutput, mainEncoder, firstSlot);
		for (size_t i = 0; i < hevcLayerEncoders.size(); i++)
			obs_output_set_video_encoder2(streamOutput, hevcLayerEncoders[i],
						      firstSlot + 1 + static_cast<uint32_t>(i));
	}

	bool HasMainEncoder() const { return mainEncoder != nullptr; }
	int LayerCount() const { return mainEncoder ? static_cast<int>(hevcLayerEncoders.size()) + 1 : 0; }

private:
	OBSEncoder mainEncoder;
	std::vector<OBSEncoder> hevcLayerEncoders;
	std::vector<bool> usingCustomLayer;
};
