#include "encoder-report.h"
#include "whip-utils.h"

#include <obs-module.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <ctime>
#include <string>
#include <thread>

#define enc_log(level, format, ...) blog(level, "[obs-webrtc] [encoder-report] " format, ##__VA_ARGS__)

namespace {
constexpr long ENCODER_REPORT_TIMEOUT_SECONDS = 8;
constexpr size_t MAX_RESPONSE_SIZE = 8 * 1024;

struct ResponseBuf {
	std::string data;
	bool exceeded = false;
};

size_t enc_write_response(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *buf = static_cast<ResponseBuf *>(userdata);
	const size_t len = size * nmemb;
	if (buf->data.size() + len > MAX_RESPONSE_SIZE) {
		buf->exceeded = true;
		return 0;
	}
	buf->data.append(ptr, len);
	return len;
}
} // namespace

std::string DeriveEncoderReportUrl(const std::string &publish_url)
{
	const std::string marker = "/v1/";
	auto pos = publish_url.find(marker);
	if (pos == std::string::npos)
		return "";
	return publish_url.substr(0, pos + marker.size()) + "encoders/report";
}

std::string EncoderReportNowUtc()
{
	const std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	gmtime_s(&tm, &t);
#else
	gmtime_r(&t, &tm);
#endif
	char buf[32] = {};
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
	return buf;
}

std::string BuildEncoderReportJson(const std::string &app_id, const std::string &app_secret,
				   const std::string &stream_name, const std::string &device_id,
				   const std::string &reported_at, const ObsEncoderConfig &config)
{
	try {
		nlohmann::json encoder;
		if (!config.video_codec.empty())
			encoder["videoCodec"] = config.video_codec;
		if (!config.video_codecs.empty())
			encoder["videoCodecs"] = config.video_codecs;
		if (!config.encoder_id.empty())
			encoder["encoderId"] = config.encoder_id;
		if (config.hardware)
			encoder["hardware"] = true;
		if (config.width > 0)
			encoder["width"] = config.width;
		if (config.height > 0)
			encoder["height"] = config.height;
		if (config.fps > 0)
			encoder["fps"] = config.fps;
		if (config.video_bitrate_kbps > 0)
			encoder["videoBitrateKbps"] = config.video_bitrate_kbps;
		if (config.keyframe_interval_sec > 0)
			encoder["keyframeIntervalSec"] = config.keyframe_interval_sec;
		if (!config.preset.empty())
			encoder["preset"] = config.preset;
		if (!config.profile.empty())
			encoder["profile"] = config.profile;
		if (!config.rate_control.empty())
			encoder["rateControl"] = config.rate_control;
		if (config.multitrack)
			encoder["multitrack"] = true;

		if (!config.simulcast_layers.empty()) {
			nlohmann::json layers = nlohmann::json::array();
			for (const auto &layer : config.simulcast_layers) {
				nlohmann::json jl;
				if (!layer.rid.empty())
					jl["rid"] = layer.rid;
				if (layer.width > 0)
					jl["width"] = layer.width;
				if (layer.height > 0)
					jl["height"] = layer.height;
				if (layer.fps > 0)
					jl["fps"] = layer.fps;
				if (layer.bitrate_kbps > 0)
					jl["bitrateKbps"] = layer.bitrate_kbps;
				layers.push_back(std::move(jl));
			}
			encoder["simulcastLayers"] = std::move(layers);
		}

		if (!config.audio_codec.empty())
			encoder["audioCodec"] = config.audio_codec;
		if (config.audio_bitrate_kbps > 0)
			encoder["audioBitrateKbps"] = config.audio_bitrate_kbps;
		if (config.audio_sample_rate > 0)
			encoder["audioSampleRate"] = config.audio_sample_rate;
		if (config.audio_channels > 0)
			encoder["audioChannels"] = config.audio_channels;

		nlohmann::json body;
		body["appId"] = app_id;
		body["appSecret"] = app_secret;
		body["streamName"] = stream_name;
		if (!device_id.empty())
			body["deviceId"] = device_id;
		if (!config.protocol.empty())
			body["protocol"] = config.protocol;
		if (!reported_at.empty())
			body["reportedAt"] = reported_at;
		body["encoder"] = std::move(encoder);
		return body.dump();
	} catch (const std::exception &e) {
		enc_log(LOG_WARNING, "failed to build encoder report JSON: %s", e.what());
		return "";
	}
}

void ReportEncoderConfigAsync(const std::string &report_url, const std::string &json_body)
{
	if (report_url.empty() || json_body.empty())
		return;

	// Copy by value into the detached thread: the caller may return and
	// destroy its strings immediately after this call.
	std::thread([report_url, json_body]() {
		CURL *curl = curl_easy_init();
		if (!curl) {
			enc_log(LOG_WARNING, "encoder report skipped: failed to init HTTP client");
			return;
		}

		struct curl_slist *headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		const std::string user_agent = generate_user_agent();
		headers = curl_slist_append(headers, user_agent.c_str());

		ResponseBuf buffer;
		char curl_error[CURL_ERROR_SIZE] = {};
		curl_easy_setopt(curl, CURLOPT_URL, report_url.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, enc_write_response);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, ENCODER_REPORT_TIMEOUT_SECONDS);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

		const CURLcode rc = curl_easy_perform(curl);
		long status = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);

		if (rc != CURLE_OK) {
			enc_log(LOG_WARNING, "encoder report HTTP request failed: %s",
				buffer.exceeded ? "response too large"
						: (curl_error[0] ? curl_error : curl_easy_strerror(rc)));
		} else if (status != 200) {
			enc_log(LOG_WARNING, "encoder report rejected by ppcenter (HTTP %ld)", status);
		} else {
			enc_log(LOG_DEBUG, "encoder config reported (HTTP 200)");
		}
	}).detach();
}
