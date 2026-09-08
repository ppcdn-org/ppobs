#pragma once

#include <map>
#include <string>
#include <vector>

struct PPCenterPublishRequest {
	std::string url;
	std::string app_id;
	std::string app_secret;
	std::string stream_name;
	std::string region;
	std::string device_id;
	std::string nat_probe_id;
};

// One codec's WHIP endpoint inside PPCenterPublishResponse::whip_tracks -
// mirrors ppcenter's publishTrackV1 (see ppcenter/internal/apis/publish_v1.go).
struct PPCenterWhipTrack {
	std::string url;
	std::string bearer_token;
};

struct PPCenterPublishResponse {
	std::string signal_token;
	std::string signal_url;
	// ICE servers supplied by ppcenter for direct P2P media. Older
	// ppcenter versions omit this field; callers provide a temporary
	// public-STUN fallback so NAT traversal still works during rollout.
	std::vector<std::string> stun_servers;
	// Keyed "h264"/"hevc" - "h264" is always required, "hevc" only when
	// HEVC/H264 multitrack is enabled - see the design doc's §3.1/§3.2.
	std::map<std::string, PPCenterWhipTrack> whip_tracks;
};

std::string ppcenter_build_publish_json(const PPCenterPublishRequest &request);
bool ppcenter_resolve_publish(const PPCenterPublishRequest &request, PPCenterPublishResponse &response,
			      std::string &error);
