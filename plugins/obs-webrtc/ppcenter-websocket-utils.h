#pragma once

#include <string>

struct ParsedWebSocketSignalURL {
	bool valid = false;
	bool tls = false;
	std::string host;
	std::string path;
};

ParsedWebSocketSignalURL ParseWebSocketSignalURL(const std::string &url);
std::string BuildWebSocketAccept(const std::string &websocketKey);
std::string GetWebSocketHeaderValue(const std::string &response, const std::string &headerName);
bool WebSocketHandshakeAccepted(const std::string &response);
bool HasP2PSubprotocol(const std::string &response);
bool HasExpectedAccept(const std::string &response, const std::string &websocketKey);
