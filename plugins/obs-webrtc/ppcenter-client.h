#pragma once

#include <string>

struct PPCenterPublishRequest {
	std::string url;
	std::string app_id;
	std::string app_secret;
	std::string stream_name;
	std::string region;
};

struct PPCenterPublishResponse {
	std::string whip_url;
	std::string bearer_token;
	std::string signal_token;
	std::string signal_url;
};

bool ppcenter_resolve_publish(const PPCenterPublishRequest &request, PPCenterPublishResponse &response,
			      std::string &error);
