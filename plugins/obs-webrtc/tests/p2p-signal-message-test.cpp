#include "ppcenter-signal-message.h"

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
		auto msg = ParseP2PSignalMessage(R"({"v":1,"type":"offer","sessionId":"s1","sdp":"v=0\r\na=sendrecv"})");
		Require(msg.valid, "offer should be valid");
		Require(msg.type == "offer", "offer type mismatch");
		Require(msg.sessionId == "s1", "offer session mismatch");
		Require(msg.sdp == "v=0\r\na=sendrecv", "offer SDP mismatch");
	}
	{
		auto msg = ParseP2PSignalMessage(R"({"v":1,"type":"ice","sessionId":"s2","candidate":{"candidate":"candidate:1 1 udp \"quoted\"","sdpMid":"0","sdpMLineIndex":0}})");
		Require(msg.valid, "ice should be valid");
		Require(msg.type == "ice", "ice type mismatch");
		Require(msg.candidate == "candidate:1 1 udp \"quoted\"", "ice candidate mismatch");
		Require(msg.sdpMid == "0", "ice sdpMid mismatch");
	}
	{
		auto msg = ParseP2PSignalMessage(R"({"v":1,"type":"close","sessionId":"s3","reason":"viewer closed"})");
		Require(msg.valid, "close should be valid");
		Require(msg.type == "close", "close type mismatch");
		Require(msg.reason == "viewer closed", "close reason mismatch");
	}
	Require(!ParseP2PSignalMessage("{").valid, "invalid JSON should be rejected");
	Require(!ParseP2PSignalMessage(R"({"v":1,"type":"offer"})").valid, "missing session should be rejected");
	return 0;
}
