#pragma once

#include <string>

struct ParsedWebSocketSignalURL {
	bool valid = false;
	bool tls = false;
	std::string host;
	std::string path;
};

ParsedWebSocketSignalURL ParseWebSocketSignalURL(const std::string &url);
// Production signaling must be encrypted because the P2P token is sent in
// Sec-WebSocket-Protocol. Plain ws:// is allowed only for loopback hosts so
// local development remains possible without weakening remote endpoints.
bool IsAllowedWebSocketSignalURL(const std::string &url);
std::string BuildWebSocketAccept(const std::string &websocketKey);
std::string GetWebSocketHeaderValue(const std::string &response, const std::string &headerName);
bool WebSocketHandshakeAccepted(const std::string &response);
bool HasP2PSubprotocol(const std::string &response);
bool HasExpectedAccept(const std::string &response, const std::string &websocketKey);
