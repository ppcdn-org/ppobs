#include "ppcenter-websocket-frame.h"

#include <cstring>

std::vector<char> BuildClientWebSocketFrame(uint8_t opcode, const std::string &payload,
					    const std::array<unsigned char, 4> &maskKey)
{
	unsigned char header[14];
	size_t len = payload.size();
	int headerSize;
	header[0] = 0x80 | opcode;
	if (len <= 125) {
		header[1] = 0x80 | static_cast<unsigned char>(len);
		headerSize = 2;
	} else if (len <= 65535) {
		header[1] = 0x80 | 126;
		header[2] = static_cast<unsigned char>((len >> 8) & 0xFF);
		header[3] = static_cast<unsigned char>(len & 0xFF);
		headerSize = 4;
	} else {
		header[1] = 0x80 | 127;
		for (int i = 0; i < 8; i++)
			header[2 + i] = static_cast<unsigned char>((len >> (56 - i * 8)) & 0xFF);
		headerSize = 10;
	}
	memcpy(header + headerSize, maskKey.data(), maskKey.size());
	headerSize += 4;
	std::vector<char> frame(headerSize + len);
	memcpy(frame.data(), header, headerSize);
	for (size_t i = 0; i < len; i++)
		frame[headerSize + i] = payload[i] ^ maskKey[i % 4];
	return frame;
}
