#pragma once

#include <string>

struct P2PSignalMessage {
	std::string type;
	std::string sessionId;
	std::string sdp;
	std::string candidate;
	std::string sdpMid;
	std::string reason;
	bool valid = false;
};

P2PSignalMessage ParseP2PSignalMessage(const std::string &json);
