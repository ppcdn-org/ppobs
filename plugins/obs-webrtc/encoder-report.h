#pragma once

#include <string>
#include <vector>

// Actual encoder configuration reported to ppcenter's
// POST /v1/encoders/report (the AI health report's "encoder parameters"
// dimension). Values here mirror what OBS itself has configured, not what the
// network ends up carrying - the two together are what makes an uplink
// problem explainable.

struct ObsEncoderLayer {
	std::string rid;
	int width = 0;
	int height = 0;
	int fps = 0;
	int bitrate_kbps = 0;
};

struct ObsEncoderConfig {
	std::string protocol;                  // "whip"
	std::string video_codec;               // primary codec
	std::vector<std::string> video_codecs; // e.g. {"h264","hevc"}
	std::string encoder_id;                // e.g. "obs_nvenc_hevc_tex"
	bool hardware = false;
	int width = 0;
	int height = 0;
	int fps = 0;
	int video_bitrate_kbps = 0;
	int keyframe_interval_sec = 0;
	std::string preset;
	std::string profile;
	std::string rate_control;
	bool multitrack = false;
	std::vector<ObsEncoderLayer> simulcast_layers;
	std::string audio_codec;
	int audio_bitrate_kbps = 0;
	int audio_sample_rate = 0;
	int audio_channels = 0;
};

// "https://host/v1/publish/requests" -> "https://host/v1/encoders/report".
// Returns "" when the publish URL has no "/v1/" segment.
std::string DeriveEncoderReportUrl(const std::string &publish_url);

// Pure: builds the report request body. Never throws.
std::string BuildEncoderReportJson(const std::string &app_id, const std::string &app_secret,
				   const std::string &stream_name, const std::string &device_id,
				   const std::string &reported_at, const ObsEncoderConfig &config);

// Fire-and-forget POST on a detached thread, so the media/start path is never
// blocked by ppcenter (including a slow or unreachable one). Failures are
// logged and otherwise ignored: encoder reporting is best-effort evidence.
void ReportEncoderConfigAsync(const std::string &report_url, const std::string &json_body);

// Current UTC time as RFC3339 ("2026-09-16T08:12:03Z").
std::string EncoderReportNowUtc();
