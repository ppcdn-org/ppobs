#pragma once

#include <string>

std::string BuildP2PAnswerJSON(const std::string &sessionId, const std::string &sdp);
std::string BuildP2PIceJSON(const std::string &sessionId, const std::string &candidate,
			    const std::string &sdpMid, int sdpMLineIndex);
std::string BuildP2PConnectedJSON(const std::string &sessionId);
std::string BuildP2PCloseJSON(const std::string &sessionId, const std::string &reason);
