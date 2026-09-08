#pragma once

#include <map>
#include <string>

struct PPCenterPublishRequest {
	std::string url;
	std::string app_id;
	std::string app_secret;
	std::string stream_name;
	std::string region;
	// Set true to ask ppcenter for the HEVC/H264 multitrack "whipTracks"
	// map (capability "whip-hevc-h264") in addition to the legacy
	// top-level whipUrl/bearerToken - see
	// docs/design/whip-hevc-h264-multitrack-simulcast-design.zh-CN.md
	// §3.2. Ignored by an old ppcenter, which simply won't include
	// whipTracks in its response.
	bool request_hevc_h264_multitrack = false;
};

// One codec's WHIP endpoint inside PPCenterPublishResponse::whip_tracks -
// mirrors ppcenter's publishTrackV1 (see ppcenter/internal/apis/publish_v1.go).
struct PPCenterWhipTrack {
	std::string url;
	std::string bearer_token;
};

struct PPCenterPublishResponse {
	std::string whip_url;
	std::string bearer_token;
	std::string signal_token;
	std::string signal_url;
	// Keyed "h264"/"hevc" - populated only when the request asked for
	// whip-hevc-h264 and ppcenter supports it. Empty (both keys absent)
	// otherwise; callers must check both PPCenterWhipTrack.url are
	// non-empty before trusting this map for a dual-codec publish (see
	// the design doc's §3.2 "不完整时拒绝启动双轨").
	std::map<std::string, PPCenterWhipTrack> whip_tracks;
};

bool ppcenter_resolve_publish(const PPCenterPublishRequest &request, PPCenterPublishResponse &response,
			      std::string &error);
