#include "encoder-report.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << "\n";
		std::exit(1);
	}
}
} // namespace

int main()
{
	Require(DeriveEncoderReportUrl("https://center.example/v1/publish/requests") ==
			"https://center.example/v1/encoders/report",
		"publish URL should map to the encoder report URL");
	Require(DeriveEncoderReportUrl("https://center.example/other") == "",
		"a URL without /v1/ should not derive a report URL");

	ObsEncoderConfig config;
	config.protocol = "whip";
	config.video_codec = "hevc";
	config.video_codecs = {"h264", "hevc"};
	config.encoder_id = "obs_nvenc_hevc_tex";
	config.hardware = true;
	config.width = 1920;
	config.height = 1080;
	config.fps = 30;
	config.video_bitrate_kbps = 6000;
	config.keyframe_interval_sec = 2;
	config.preset = "p5";
	config.profile = "main";
	config.rate_control = "cbr";
	config.multitrack = true;
	config.simulcast_layers.push_back(ObsEncoderLayer{"f", 1920, 1080, 30, 6000});
	config.simulcast_layers.push_back(ObsEncoderLayer{"h", 1280, 720, 30, 3000});
	config.audio_codec = "opus";
	config.audio_bitrate_kbps = 128;
	config.audio_sample_rate = 48000;
	config.audio_channels = 2;

	auto body = nlohmann::json::parse(
		BuildEncoderReportJson("app", "secret", "live", "dev-1", "2026-09-16T08:12:03Z", config));
	Require(body.value("appId", "") == "app", "appId should be included");
	Require(body.value("appSecret", "") == "secret", "appSecret should be included");
	Require(body.value("streamName", "") == "live", "streamName should be included");
	Require(body.value("protocol", "") == "whip", "protocol should be included");
	Require(body.value("deviceId", "") == "dev-1", "deviceId should be included");
	Require(body.value("reportedAt", "") == "2026-09-16T08:12:03Z", "reportedAt should be included");

	const auto &encoder = body["encoder"];
	Require(encoder.value("videoCodec", "") == "hevc", "primary codec should be included");
	Require(encoder["videoCodecs"] == nlohmann::json::array({"h264", "hevc"}), "codec set should be included");
	Require(encoder.value("encoderId", "") == "obs_nvenc_hevc_tex", "encoderId should be included");
	Require(encoder.value("hardware", false), "hardware flag should be included");
	Require(encoder.value("width", 0) == 1920, "width should be included");
	Require(encoder.value("height", 0) == 1080, "height should be included");
	Require(encoder.value("videoBitrateKbps", 0) == 6000, "video bitrate should be included");
	Require(encoder.value("keyframeIntervalSec", 0) == 2, "keyframe interval should be included");
	Require(encoder.value("multitrack", false), "multitrack flag should be included");
	Require(encoder["simulcastLayers"].size() == 2, "both simulcast layers should be included");
	Require(encoder["simulcastLayers"][0].value("rid", "") == "f", "layer rid should be included");
	Require(encoder["simulcastLayers"][1].value("bitrateKbps", 0) == 3000, "layer bitrate should be included");
	Require(encoder.value("audioCodec", "") == "opus", "audio codec should be included");
	Require(encoder.value("audioSampleRate", 0) == 48000, "audio sample rate should be included");

	// A minimal config must omit empty/zero fields rather than sending them.
	ObsEncoderConfig minimal;
	minimal.protocol = "srt";
	minimal.video_codec = "h264";
	auto minimalBody = nlohmann::json::parse(BuildEncoderReportJson("a", "s", "l", "", "", minimal));
	Require(!minimalBody.contains("deviceId"), "empty deviceId should be omitted");
	Require(!minimalBody.contains("reportedAt"), "empty reportedAt should be omitted");
	const auto &minimalEncoder = minimalBody["encoder"];
	Require(!minimalEncoder.contains("width"), "zero width should be omitted");
	Require(!minimalEncoder.contains("simulcastLayers"), "empty ladder should be omitted");
	Require(!minimalEncoder.contains("audioCodec"), "empty audio codec should be omitted");
	return 0;
}
