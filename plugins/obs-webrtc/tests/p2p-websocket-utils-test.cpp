#include "ppcenter-websocket-utils.h"

#include <cstdlib>
#include <iostream>

namespace {
void Require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << "\n";
		std::exit(1);
	}
}
}

int main()
{
	{
		auto parsed = ParseWebSocketSignalURL("ws://center.example/v1/p2p/signal");
		Require(parsed.valid, "ws URL should be valid");
		Require(!parsed.tls, "ws URL should not be TLS");
		Require(parsed.host == "center.example", "ws host mismatch");
		Require(parsed.path == "/v1/p2p/signal", "ws path mismatch");
	}
	{
		auto parsed = ParseWebSocketSignalURL("wss://center.example:9443");
		Require(parsed.valid, "wss URL should be valid");
		Require(parsed.tls, "wss URL should be TLS");
		Require(parsed.host == "center.example:9443", "wss host mismatch");
		Require(parsed.path == "/", "default path mismatch");
	}
	Require(!ParseWebSocketSignalURL("http://center.example/v1/p2p/signal").valid, "http URL should be rejected");
	Require(!ParseWebSocketSignalURL("wss:///v1/p2p/signal").valid, "missing host should be rejected");

	const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
	Require(BuildWebSocketAccept(key) == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC accept mismatch");
	const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
				     "Upgrade: websocket\r\n"
				     "Connection: Upgrade\r\n"
				     "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
				     "Sec-WebSocket-Protocol: ppcdn-p2p-v1\r\n\r\n";
	Require(WebSocketHandshakeAccepted(response), "101 response should be accepted");
	Require(HasP2PSubprotocol(response), "P2P subprotocol should be detected");
	Require(HasExpectedAccept(response, key), "accept header should match");
	Require(!WebSocketHandshakeAccepted("HTTP/1.1 200 OK\r\n\r\n"), "non-101 response should be rejected");
	Require(!HasP2PSubprotocol("HTTP/1.1 101 Switching Protocols\r\n\r\n"), "missing subprotocol should be rejected");
	Require(!HasExpectedAccept(response, "wrong-key"), "wrong accept key should be rejected");
	return 0;
}
