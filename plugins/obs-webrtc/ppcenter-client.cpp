#include "ppcenter-client.h"
#include "whip-utils.h"

#include <obs.h>
#include <curl/curl.h>
#include <cstring>

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

	obs_data_t *body = obs_data_create();
	obs_data_set_string(body, "appId", request.app_id.c_str());
	obs_data_set_string(body, "appSecret", request.app_secret.c_str());
	obs_data_set_string(body, "streamName", request.stream_name.c_str());
	obs_data_set_string(body, "requestRegion", request.region.c_str());
	const char *json = obs_data_get_json(body);

	CURL *curl = curl_easy_init();
	if (!curl) {
		obs_data_release(body);
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
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(strlen(json)));
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
	obs_data_release(body);

	if (result != CURLE_OK) {
		error = buffer.exceeded ? "ppcenter response is too large" :
			(curl_error[0] ? curl_error : curl_easy_strerror(result));
		return false;
	}
	if (status != 200) {
		error = "ppcenter rejected the publish request (HTTP " + std::to_string(status) + ")";
		return false;
	}

	obs_data_t *decoded = obs_data_create_from_json(buffer.data.c_str());
	if (!decoded) {
		error = "ppcenter returned invalid JSON";
		return false;
	}
	response.whip_url = obs_data_get_string(decoded, "whipUrl");
	response.bearer_token = obs_data_get_string(decoded, "bearerToken");
	obs_data_t *signal = obs_data_get_obj(decoded, "signal");
	if (signal) {
		response.signal_token = obs_data_get_string(signal, "token");
		response.signal_url = obs_data_get_string(signal, "signalUrl");
		obs_data_release(signal);
	}
	obs_data_release(decoded);
	if (response.whip_url.empty() || response.bearer_token.empty()) {
		error = "ppcenter response is missing WHIP credentials";
		return false;
	}
	return true;
}
