#include "ppcenter-signal-message.h"
#include "ppcenter-signal-outgoing.h"

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
		auto parsed = ParseP2PSignalMessage(BuildP2PAnswerJSON("s1", "v=0\r\na=\"quoted\""));
		Require(parsed.valid, "answer JSON should parse");
		Require(parsed.type == "answer", "answer type mismatch");
		Require(parsed.sessionId == "s1", "answer session mismatch");
		Require(parsed.sdp == "v=0\r\na=\"quoted\"", "answer SDP escaping mismatch");
	}
	{
		auto parsed = ParseP2PSignalMessage(BuildP2PIceJSON("s2", "candidate:1 \"quoted\"", "0", 0));
		Require(parsed.valid, "ice JSON should parse");
		Require(parsed.type == "ice", "ice type mismatch");
		Require(parsed.sessionId == "s2", "ice session mismatch");
		Require(parsed.candidate == "candidate:1 \"quoted\"", "ice candidate escaping mismatch");
		Require(parsed.sdpMid == "0", "ice sdpMid mismatch");
	}
	{
		auto parsed = ParseP2PSignalMessage(BuildP2PConnectedJSON("s3"));
		Require(parsed.valid, "connected JSON should parse");
		Require(parsed.type == "connected", "connected type mismatch");
		Require(parsed.sessionId == "s3", "connected session mismatch");
	}
	{
		auto parsed = ParseP2PSignalMessage(BuildP2PCloseJSON("s4", "reason with \"quotes\" and newline\n"));
		Require(parsed.valid, "close JSON should parse");
		Require(parsed.type == "close", "close type mismatch");
		Require(parsed.reason == "reason with \"quotes\" and newline\n", "close reason escaping mismatch");
	}
	return 0;
}
