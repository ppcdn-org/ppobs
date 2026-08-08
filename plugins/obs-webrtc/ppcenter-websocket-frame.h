#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

std::vector<char> BuildClientWebSocketFrame(uint8_t opcode, const std::string &payload,
					    const std::array<unsigned char, 4> &maskKey);
