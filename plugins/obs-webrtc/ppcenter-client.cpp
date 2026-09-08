#include "ppcenter-client.h"
#include "whip-utils.h"

#include <obs.h>
#include <curl/curl.h>
#include <cstring>
#include <nlohmann/json.hpp>

namespace {
constexpr long PPCENTER_TIMEOUT_SECONDS = 8;
constexpr size_t MAX_RESPONSE_SIZE = 64 * 1024;

struct ResponseBuffer {
	std::string data;
	bool exceeded = false;
};

size_t write_response(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *buffer = static_cast<ResponseBuffer *>(userdata);
	const size_t length = size * nmemb;
	if (buffer->data.size() + length > MAX_RESPONSE_SIZE) {
		buffer->exceeded = true;
		return 0;
	}
	buffer->data.append(ptr, length);
	return length;
}
}

bool ppcenter_resolve_publish(const PPCenterPublishRequest &request, PPCenterPublishResponse &response,
			      std::string &error)
{
	if (request.url.empty() || request.app_id.empty() || request.app_secret.empty() || request.stream_name.empty()) {
		error = "ppcenter URL, app ID, app secret and stream name are required";
		return false;
	}

	// Built directly as JSON (rather than through obs_data_t) so
	// "capabilities" can be a plain JSON string array, matching
	// ppcenter's publishRequestV1.Capabilities []string (see
	// ppcenter/internal/apis/publish_v1.go) - obs_data_array_t's element
	// type is fixed to an object, with no "push a bare string" call.
	nlohmann::json body = {
		{"appId", request.app_id},
		{"appSecret", request.app_secret},
		{"streamName", request.stream_name},
		{"requestRegion", request.region},
	};
	if (request.request_hevc_h264_multitrack) {
		// "whip-hevc-h264" is the only capability value the dual-codec
		// publish path needs - see the design doc's §3.2.
		body["capabilities"] = nlohmann::json::array({"whip-hevc-h264"});
	}
	const std::string json_body = body.dump();

	CURL *curl = curl_easy_init();
	if (!curl) {
		error = "failed to initialize HTTP client";
		return false;
	}
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	const std::string user_agent_header = generate_user_agent();
	headers = curl_slist_append(headers, user_agent_header.c_str());
	ResponseBuffer buffer;
	char curl_error[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, PPCENTER_TIMEOUT_SECONDS);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);

	const CURLcode result = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (result != CURLE_OK) {
		error = buffer.exceeded ? "ppcenter response is too large" :
			(curl_error[0] ? curl_error : curl_easy_strerror(result));
		return false;
	}
	if (status != 200) {
		error = "ppcenter rejected the publish request (HTTP " + std::to_string(status) + ")";
		return false;
	}

	nlohmann::json decoded;
	try {
		decoded = nlohmann::json::parse(buffer.data);
	} catch (const std::exception &) {
		error = "ppcenter returned invalid JSON";
		return false;
	}

	response.whip_url = decoded.value("whipUrl", "");
	response.bearer_token = decoded.value("bearerToken", "");
	if (decoded.contains("signal") && decoded["signal"].is_object()) {
		const auto &signal = decoded["signal"];
		response.signal_token = signal.value("token", "");
		response.signal_url = signal.value("signalUrl", "");
	}
	response.whip_tracks.clear();
	if (decoded.contains("whipTracks") && decoded["whipTracks"].is_object()) {
		// Keyed "h264"/"hevc" - see the design doc's §3.2 and
		// ppcenter's publishDecisionV1.WHIPTracks
		// (map[string]publishTrackV1).
		for (auto &[codec, track] : decoded["whipTracks"].items()) {
			if (!track.is_object())
				continue;
			PPCenterWhipTrack t;
			t.url = track.value("url", "");
			t.bearer_token = track.value("bearerToken", "");
			response.whip_tracks[codec] = t;
		}
	}
	if (response.whip_url.empty() || response.bearer_token.empty()) {
		error = "ppcenter response is missing WHIP credentials";
		return false;
	}
	return true;
}
