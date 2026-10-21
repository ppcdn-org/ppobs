#pragma once

#include <obs.h>
#include <util/base.h>

#include <cstring>
#include <string>

// Forces an encoder to emit no B-frames.
//
// OBS's encoders do not agree on the settings key:
//   * "bf"      - NVENC (obs-nvenc and ffmpeg-nvenc), x264, VAAPI, AMF
//   * "bframes" - QSV, and VideoToolbox (as a bool)
// Set both unconditionally; encoders ignore keys they do not read. Applied
// with obs_encoder_update(), which only mutates settings when the encoder has
// not been initialized yet, and defers/reconfigures otherwise.
//
// obs-nvenc's UHQ tuning hard-codes at least four B-frames regardless of bf
// (see plugins/obs-nvenc/venc.c), so UHQ has to be moved back to HQ for bf=0
// to actually take effect.
static inline void ForceEncoderNoBFrames(obs_encoder_t *encoder)
{
	if (!encoder)
		return;

	const char *id = obs_encoder_get_id(encoder);
	const std::string encoderId = id ? id : "";
	const bool videoToolbox = encoderId.find("videotoolbox") != std::string::npos;

	obs_data_t *current = obs_encoder_get_settings(encoder);
	const long long oldBf = current ? obs_data_get_int(current, "bf") : 0;
	const long long oldBFrames = current && !videoToolbox ? obs_data_get_int(current, "bframes") : 0;
	const bool oldVTBFrames = current && videoToolbox ? obs_data_get_bool(current, "bframes") : false;
	if (current)
		obs_data_release(current);

	obs_data_t *force = obs_data_create();
	obs_data_set_int(force, "bf", 0);
	if (videoToolbox)
		obs_data_set_bool(force, "bframes", false);
	else
		obs_data_set_int(force, "bframes", 0);

	if (encoderId.find("nvenc") != std::string::npos) {
		obs_data_t *settings = obs_encoder_get_settings(encoder);
		if (settings) {
			const char *tune = obs_data_get_string(settings, "tune");
			if (tune && strcmp(tune, "uhq") == 0) {
				obs_data_set_string(force, "tune", "hq");
				blog(LOG_INFO,
				     "[PPCDN] encoder '%s' (%s) uses NVENC UHQ (forces B-frames); switching tune to hq",
				     obs_encoder_get_name(encoder), encoderId.c_str());
			}
			obs_data_release(settings);
		}
	}

	if (oldBf != 0 || oldBFrames != 0 || oldVTBFrames) {
		blog(LOG_INFO, "[PPCDN] forcing B-frames=0 on encoder '%s' (%s): bf=%lld bframes=%lld",
		     obs_encoder_get_name(encoder), encoderId.c_str(), oldBf,
		     videoToolbox ? (oldVTBFrames ? 1LL : 0LL) : oldBFrames);
	}

	obs_encoder_update(encoder, force);
	obs_data_release(force);
}
