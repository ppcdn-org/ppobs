#pragma once

#include <string>
#include <vector>

struct P2PLayerInfo {
	int id = 0;
	int width = 0;
	int height = 0;
	int bitrate = 0;
	std::string label;
};

std::string BuildP2PAnswerJSON(const std::string &sessionId, const std::string &sdp);
std::string BuildP2PIceJSON(const std::string &sessionId, const std::string &candidate,
			    const std::string &sdpMid, int sdpMLineIndex);
std::string BuildP2PConnectedJSON(const std::string &sessionId);
std::string BuildP2PCloseJSON(const std::string &sessionId, const std::string &reason);
std::string BuildP2PLayersJSON(const std::string &sessionId, const std::vector<P2PLayerInfo> &layers);
std::string BuildP2PLayerSwitchedJSON(const std::string &sessionId, int layer);
