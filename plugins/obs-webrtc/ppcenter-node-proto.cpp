#include "ppcenter-node-proto.h"

#include <cstring>
#include <algorithm>

namespace {

// ── Varint encoding ──

void AppendVarint(std::vector<uint8_t> &out, uint64_t value)
{
	while (value >= 0x80) {
		out.push_back(static_cast<uint8_t>(value | 0x80));
		value >>= 7;
	}
	out.push_back(static_cast<uint8_t>(value));
}

void AppendTag(std::vector<uint8_t> &out, int fieldNumber, int wireType)
{
	uint64_t tag = (static_cast<uint64_t>(fieldNumber) << 3) | static_cast<uint64_t>(wireType);
	AppendVarint(out, tag);
}

void AppendBytes(std::vector<uint8_t> &out, const std::string &value)
{
	AppendVarint(out, static_cast<uint64_t>(value.size()));
	out.insert(out.end(), value.begin(), value.end());
}

// ── uint64 parsing ──

bool ReadVarint(const uint8_t *&ptr, const uint8_t *end, uint64_t &value)
{
	value = 0;
	int shift = 0;
	while (ptr < end) {
		uint8_t byte = *ptr++;
		value |= static_cast<uint64_t>(byte & 0x7F) << shift;
		if (!(byte & 0x80))
			return true;
		shift += 7;
		if (shift >= 64)
			return false;
	}
	return false;
}

bool ReadString(const uint8_t *&ptr, const uint8_t *end, std::string &value)
{
	uint64_t length = 0;
	if (!ReadVarint(ptr, end, length))
		return false;
	if (length > static_cast<uint64_t>(end - ptr))
		return false;
	value.assign(reinterpret_cast<const char *>(ptr), static_cast<size_t>(length));
	ptr += static_cast<size_t>(length);
	return true;
}

bool ReadSInt32(const uint8_t *&ptr, const uint8_t *end, int32_t &value)
{
	uint64_t zigzag = 0;
	if (!ReadVarint(ptr, end, zigzag))
		return false;
	value = static_cast<int32_t>(static_cast<uint32_t>(zigzag) >> 1);
	if (zigzag & 1)
		value = ~value;
	return true;
}

bool ReadUInt32(const uint8_t *&ptr, const uint8_t *end, uint32_t &value)
{
	uint64_t v = 0;
	if (!ReadVarint(ptr, end, v))
		return false;
	value = static_cast<uint32_t>(v);
	return true;
}

// ── Protobuf wire type constants ──
constexpr int WIRE_VARINT = 0;
constexpr int WIRE_BYTES  = 2;

} // namespace

// ── Public field encoders ──

std::vector<uint8_t> ProtoFieldSInt32(int fieldNumber, int32_t value)
{
	// Zigzag encode: (value << 1) ^ (value >> 31)
	uint64_t zigzag = (static_cast<uint64_t>(static_cast<uint32_t>(value) << 1)) ^
			  (static_cast<uint64_t>(static_cast<int64_t>(value) >> 31));
	std::vector<uint8_t> out;
	AppendTag(out, fieldNumber, WIRE_VARINT);
	AppendVarint(out, zigzag);
	return out;
}

std::vector<uint8_t> ProtoFieldUInt32(int fieldNumber, uint32_t value)
{
	std::vector<uint8_t> out;
	AppendTag(out, fieldNumber, WIRE_VARINT);
	AppendVarint(out, static_cast<uint64_t>(value));
	return out;
}

std::vector<uint8_t> ProtoFieldString(int fieldNumber, const std::string &value)
{
	std::vector<uint8_t> out;
	AppendTag(out, fieldNumber, WIRE_BYTES);
	AppendBytes(out, value);
	return out;
}

// ── WorkerIndication encoder ──

std::vector<uint8_t> WorkerIndication::Encode() const
{
	std::vector<uint8_t> out;

	// field 1: msgType (string)
	{
		auto f = ProtoFieldString(1, msgType);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 2: workerType (string)
	{
		auto f = ProtoFieldString(2, workerType);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 3: workerId (sint32, zigzag)
	{
		auto f = ProtoFieldSInt32(3, workerId);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 4: version (string)
	{
		auto f = ProtoFieldString(4, version);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 5: region (string)
	{
		auto f = ProtoFieldString(5, region);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 6: capacity (int32/u32)
	{
		auto f = ProtoFieldUInt32(6, static_cast<uint32_t>(capacity));
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 7: pipelines (repeated string)
	for (const auto &p : pipelines) {
		auto f = ProtoFieldString(7, p);
		out.insert(out.end(), f.begin(), f.end());
	}

	return out;
}

// ── NodeMsgReq parser ──

NodeMsgReq ParseNodeMsgReq(const std::vector<uint8_t> &data)
{
	NodeMsgReq req;
	const uint8_t *ptr = data.data();
	const uint8_t *end = ptr + data.size();

	while (ptr < end) {
		uint64_t tag = 0;
		if (!ReadVarint(ptr, end, tag))
			return req; // invalid

		int fieldNumber = static_cast<int>(tag >> 3);
		int wireType = static_cast<int>(tag & 0x07);

		switch (fieldNumber) {
		case 1: // msgType (string)
			if (wireType == WIRE_BYTES && !ReadString(ptr, end, req.msgType))
				return req;
			break;
		case 2: // workerType (string)
			if (wireType == WIRE_BYTES && !ReadString(ptr, end, req.workerType))
				return req;
			break;
		case 3: // workerId (sint32)
			if (wireType == WIRE_VARINT && !ReadSInt32(ptr, end, req.workerId))
				return req;
			break;
		case 4: { // streamPath (string)
			std::string sp;
			if (wireType == WIRE_BYTES && !ReadString(ptr, end, sp))
				return req;
			req.streamPath = sp;
			break;
		}
		case 5: { // signature (string)
			std::string sig;
			if (wireType == WIRE_BYTES && !ReadString(ptr, end, sig))
				return req;
			req.signature = sig;
			break;
		}
		case 6: { // msgId (string)
			std::string mid;
			if (wireType == WIRE_BYTES && !ReadString(ptr, end, mid))
				return req;
			req.msgId = mid;
			break;
		}
		case 7: { // publishUrl (string)
			std::string pu;
			if (wireType == WIRE_BYTES && !ReadString(ptr, end, pu))
				return req;
			req.publishUrl = pu;
			break;
		}
		default:
			// Unknown field: skip based on wire type
			if (wireType == WIRE_VARINT) {
				uint64_t dummy = 0;
				if (!ReadVarint(ptr, end, dummy))
					return req;
			} else if (wireType == WIRE_BYTES) {
				std::string dummy;
				if (!ReadString(ptr, end, dummy))
					return req;
			} else {
				// Wire type 5 (32-bit fixed) skip 4 bytes
				if (end - ptr < 4)
					return req;
				ptr += 4;
			}
			break;
		}
	}

	req.valid = true;
	return req;
}

// ── NodeMsgRsp builder ──

std::vector<uint8_t> BuildNodeMsgRsp(const NodeMsgRspParams &params)
{
	std::vector<uint8_t> out;

	// field 1: msgType = "COMMAND_TYPE_RESPONSE"
	{
		auto f = ProtoFieldString(1, "COMMAND_TYPE_RESPONSE");
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 2: workerType
	{
		auto f = ProtoFieldString(2, params.workerType);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 3: workerId (sint32)
	{
		auto f = ProtoFieldSInt32(3, params.workerId);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 4: code (sint32)
	{
		auto f = ProtoFieldSInt32(4, params.code);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 5: reason (string)
	if (!params.reason.empty()) {
		auto f = ProtoFieldString(5, params.reason);
		out.insert(out.end(), f.begin(), f.end());
	}
	// field 6: msgId (string)
	if (!params.msgId.empty()) {
		auto f = ProtoFieldString(6, params.msgId);
		out.insert(out.end(), f.begin(), f.end());
	}

	return out;
}