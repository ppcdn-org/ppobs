#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ── Protobuf wire-format helpers (avoid protobuf library dependency) ──

// Encodes a 32-bit signed integer as a zigzag+varint field.
std::vector<uint8_t> ProtoFieldSInt32(int fieldNumber, int32_t value);
// Encodes a uint32 as a varint field.
std::vector<uint8_t> ProtoFieldUInt32(int fieldNumber, uint32_t value);
// Encodes a string/bytes field.
std::vector<uint8_t> ProtoFieldString(int fieldNumber, const std::string &value);

// ── WorkerIndication encoder (publisher → ppcenter heartbeat) ──

struct WorkerIndication {
	std::string msgType;  // "COMMAND_TYPE_INDICATION"
	std::string workerType; // e.g. "NODE_ROLE_PUBLISHER"
	int32_t workerId = 0;
	std::string version;
	std::string region;
	int32_t capacity = 0;
	std::vector<std::string> pipelines;

	// Returns protobuf-encoded binary message (field numbers match mmx.proto).
	std::vector<uint8_t> Encode() const;
};

// ── NodeMsgReq parser (ppcenter → node command) ──

struct NodeMsgReq {
	std::string msgType;
	std::string workerType;
	int32_t workerId = 0;
	std::string streamPath;
	std::string signature;
	std::string msgId;
	std::string publishUrl;
	bool valid = false;
};

NodeMsgReq ParseNodeMsgReq(const std::vector<uint8_t> &data);

// ── NodeMsgRsp builder (node → ppcenter response) ──

struct NodeMsgRspParams {
	std::string workerType; // e.g. "NODE_ROLE_PUBLISHER"
	int32_t workerId = 0;
	int32_t code = 0;
	std::string reason;
	std::string msgId;
};

// Builds protobuf-encoded NodeMsgRsp (field numbers match proto.proto).
std::vector<uint8_t> BuildNodeMsgRsp(const NodeMsgRspParams &params);