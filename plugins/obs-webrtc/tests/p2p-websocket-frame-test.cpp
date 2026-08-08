#include "ppcenter-websocket-frame.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << "\n";
		std::exit(1);
	}
}

std::string UnmaskPayload(const std::vector<char> &frame, size_t payloadOffset)
{
	unsigned char mask[] = {static_cast<unsigned char>(frame[payloadOffset - 4]),
			      static_cast<unsigned char>(frame[payloadOffset - 3]),
			      static_cast<unsigned char>(frame[payloadOffset - 2]),
			      static_cast<unsigned char>(frame[payloadOffset - 1])};
	std::string out;
	for (size_t i = payloadOffset; i < frame.size(); ++i)
		out.push_back(frame[i] ^ mask[(i - payloadOffset) % 4]);
	return out;
}
}

int main()
{
	const std::array<unsigned char, 4> mask = {0x01, 0x02, 0x03, 0x04};
	{
		auto frame = BuildClientWebSocketFrame(0x01, "hello", mask);
		Require(static_cast<unsigned char>(frame[0]) == 0x81, "text opcode mismatch");
		Require(static_cast<unsigned char>(frame[1]) == (0x80 | 5), "short masked length mismatch");
		Require(static_cast<unsigned char>(frame[2]) == 0x01 && static_cast<unsigned char>(frame[5]) == 0x04, "mask mismatch");
		Require(UnmaskPayload(frame, 6) == "hello", "short payload mismatch");
	}
	{
		auto frame = BuildClientWebSocketFrame(0x0A, std::string(126, 'a'), mask);
		Require(static_cast<unsigned char>(frame[0]) == 0x8A, "pong opcode mismatch");
		Require(static_cast<unsigned char>(frame[1]) == (0x80 | 126), "126 marker mismatch");
		Require(static_cast<unsigned char>(frame[2]) == 0 && static_cast<unsigned char>(frame[3]) == 126, "126 length mismatch");
		Require(UnmaskPayload(frame, 8) == std::string(126, 'a'), "126 payload mismatch");
	}
	{
		auto frame = BuildClientWebSocketFrame(0x01, std::string(66000, 'b'), mask);
		Require(static_cast<unsigned char>(frame[1]) == (0x80 | 127), "127 marker mismatch");
		Require(static_cast<unsigned char>(frame[8]) == 0x01 && static_cast<unsigned char>(frame[9]) == 0xD0, "127 length high bytes mismatch");
		Require(static_cast<unsigned char>(frame[10]) == 0x01 && static_cast<unsigned char>(frame[13]) == 0x04, "127 mask mismatch");
		Require(UnmaskPayload(frame, 14).size() == 66000, "127 payload size mismatch");
		Require(UnmaskPayload(frame, 14)[0] == 'b' && UnmaskPayload(frame, 14)[65999] == 'b', "127 payload mismatch");
	}
	return 0;
}
