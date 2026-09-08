#include "ppcenter-websocket-utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <vector>

namespace {
std::string Base64Encode(const unsigned char *data, size_t len)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		const uint32_t b0 = data[i];
		const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
		const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
		const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
		out.push_back(table[(triple >> 18) & 0x3F]);
		out.push_back(table[(triple >> 12) & 0x3F]);
		out.push_back((i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=');
		out.push_back((i + 2 < len) ? table[triple & 0x3F] : '=');
	}
	return out;
}

std::string ToLower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

uint32_t RotateLeft(uint32_t value, int bits)
{
	return (value << bits) | (value >> (32 - bits));
}

std::array<unsigned char, 20> Sha1(const std::string &input)
{
	std::vector<unsigned char> data(input.begin(), input.end());
	const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8;
	data.push_back(0x80);
	while ((data.size() % 64) != 56)
		data.push_back(0);
	for (int i = 7; i >= 0; --i)
		data.push_back(static_cast<unsigned char>((bitLen >> (i * 8)) & 0xFF));

	uint32_t h0 = 0x67452301;
	uint32_t h1 = 0xEFCDAB89;
	uint32_t h2 = 0x98BADCFE;
	uint32_t h3 = 0x10325476;
	uint32_t h4 = 0xC3D2E1F0;

	for (size_t offset = 0; offset < data.size(); offset += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; ++i) {
			w[i] = (static_cast<uint32_t>(data[offset + i * 4]) << 24) |
			       (static_cast<uint32_t>(data[offset + i * 4 + 1]) << 16) |
			       (static_cast<uint32_t>(data[offset + i * 4 + 2]) << 8) |
			       static_cast<uint32_t>(data[offset + i * 4 + 3]);
		}
		for (int i = 16; i < 80; ++i)
			w[i] = RotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

		uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
		for (int i = 0; i < 80; ++i) {
			uint32_t f, k;
			if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			const uint32_t temp = RotateLeft(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = RotateLeft(b, 30);
			b = a;
			a = temp;
		}
		h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
	}

	std::array<unsigned char, 20> digest{};
	const uint32_t words[] = {h0, h1, h2, h3, h4};
	for (int i = 0; i < 5; ++i) {
		digest[i * 4] = static_cast<unsigned char>((words[i] >> 24) & 0xFF);
		digest[i * 4 + 1] = static_cast<unsigned char>((words[i] >> 16) & 0xFF);
		digest[i * 4 + 2] = static_cast<unsigned char>((words[i] >> 8) & 0xFF);
		digest[i * 4 + 3] = static_cast<unsigned char>(words[i] & 0xFF);
	}
	return digest;
}

std::string Trim(std::string value)
{
	auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c) { return !isSpace(c); }));
	value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c) { return !isSpace(c); }).base(), value.end());
	return value;
}
} // namespace

ParsedWebSocketSignalURL ParseWebSocketSignalURL(const std::string &url)
{
	ParsedWebSocketSignalURL parsed;
	auto schemeEnd = url.find("://");
	if (schemeEnd == std::string::npos)
		return parsed;

	const auto scheme = url.substr(0, schemeEnd);
	if (scheme == "wss")
		parsed.tls = true;
	else if (scheme != "ws")
		return parsed;

	auto hostStart = schemeEnd + 3;
	auto pathStart = url.find('/', hostStart);
	if (pathStart != std::string::npos) {
		parsed.host = url.substr(hostStart, pathStart - hostStart);
		parsed.path = url.substr(pathStart);
	} else {
		parsed.host = url.substr(hostStart);
		parsed.path = "/";
	}
	parsed.valid = !parsed.host.empty();
	return parsed;
}

bool IsAllowedWebSocketSignalURL(const std::string &url)
{
	auto parsed = ParseWebSocketSignalURL(url);
	if (!parsed.valid)
		return false;
	if (parsed.tls)
		return true;

	std::string hostname = parsed.host;
	if (!hostname.empty() && hostname.front() == '[') {
		auto close = hostname.find(']');
		if (close == std::string::npos)
			return false;
		hostname = hostname.substr(1, close - 1);
	} else {
		auto colon = hostname.find(':');
		if (colon != std::string::npos)
			hostname.resize(colon);
	}

	hostname = ToLower(hostname);
	if (hostname == "localhost" || hostname == "::1")
		return true;
	if (hostname.compare(0, 4, "127.") != 0)
		return false;

	// Parse the rest of 127/8 strictly enough to avoid accepting names such
	// as 127.example.com as a local debugging exemption.
	int octets = 0;
	size_t start = 0;
	while (start < hostname.size()) {
		auto end = hostname.find('.', start);
		auto part = hostname.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (part.empty() || part.size() > 3 ||
		    !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
			return false;
		if (std::stoi(part) > 255)
			return false;
		++octets;
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return octets == 4;
}

std::string BuildWebSocketAccept(const std::string &websocketKey)
{
	static const std::string guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	const auto digest = Sha1(websocketKey + guid);
	return Base64Encode(digest.data(), digest.size());
}

std::string GetWebSocketHeaderValue(const std::string &response, const std::string &headerName)
{
	const auto lower = ToLower(response);
	const auto header = "\r\n" + ToLower(headerName) + ":";
	auto pos = lower.find(header);
	if (pos == std::string::npos)
		return "";
	pos += header.size();
	auto end = response.find("\r\n", pos);
	return Trim(response.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
}

bool WebSocketHandshakeAccepted(const std::string &response)
{
	auto lineEnd = response.find("\r\n");
	const auto statusLine = lineEnd == std::string::npos ? response : response.substr(0, lineEnd);
	return statusLine.find(" 101 ") != std::string::npos ||
	       (statusLine.size() >= 12 && statusLine.compare(statusLine.size() - 4, 4, " 101") == 0);
}

bool HasP2PSubprotocol(const std::string &response)
{
	const auto value = ToLower(GetWebSocketHeaderValue(response, "Sec-WebSocket-Protocol"));
	return value.find("ppcdn-p2p-v1") != std::string::npos;
}

bool HasExpectedAccept(const std::string &response, const std::string &websocketKey)
{
	return GetWebSocketHeaderValue(response, "Sec-WebSocket-Accept") == BuildWebSocketAccept(websocketKey);
}
