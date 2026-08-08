#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <vector>

// ── Protobuf message structures (from ppcenter-node-proto.h) ──

struct NodeMsgReq;
struct NodeMsgRspParams;

// ── Configuration for the node channel client ──

struct NodeChannelConfig {
	std::string url;       // ws:// or wss:// ppcenter /ws/mmx
	std::string bearerToken; // node Bearer token for Authorization header
	std::string workerRole;  // "NODE_ROLE_PUBLISHER"
	int32_t workerId = 1;
	std::string nodeVersion;
	std::string nodeRegion;
	int32_t nodeCapacity = 1;
	int32_t heartbeatIntervalSec = 10;
	int32_t wsFailThreshold = 5;  // consecutive WS failures before switching to HTTP fallback
	int32_t httpPollIntervalSec = 5; // HTTP commands polling interval
};

// ── Forward decl ──

class WSTransport;

// ── NodeChannelClient ──
//
// Establishes a WebSocket connection to ppcenter at /ws/mmx,
// authenticating with HTTP Authorization: Bearer header during upgrade.
//
// On connect, sends a WorkerIndication (protobuf binary frame) immediately,
// then periodic heartbeats every heartbeatIntervalSec.
//
// Receives binary WebSocket frames, parses them as NodeMsgReq commands,
// and dispatches to a callback. The callback result is sent back as a
// NodeMsgRsp (protobuf binary frame).
//
// Handles server pings by replying with a masked pong frame.
// Detect server push close frames and reconnect.
// When the WS is disconnected, the run loop reconnects with exponential backoff.

class NodeChannelClient {
public:
	NodeChannelClient(const NodeChannelConfig &config);
	~NodeChannelClient();

	// Start the connect/read/write loop in a background thread.
	bool Start();
	// Stop the loop and close the WebSocket.
	void Stop();

	// Callback invoked for each incoming command from ppcenter.
	// The callback returns a NodeMsgRspParams that gets sent back.
	using CommandHandler = std::function<NodeMsgRspParams(const NodeMsgReq &)>;
	void SetCommandHandler(CommandHandler handler);

	bool IsConnected() const;

private:
	void Run();
	bool ConnectWS();

	// WS frame read helpers
	bool ReadExact(char *buffer, size_t length);
	bool ReadExtendedLength(size_t &payloadLen);
	bool ReadPayload(size_t payloadLen, std::vector<char> &payload);
	bool ReadBinaryPayload(size_t payloadLen, std::vector<uint8_t> &payload);

	void SendPong(const std::string &payload);
	bool SendBinaryFrame(const std::vector<uint8_t> &data);
	std::vector<uint8_t> BuildIndication() const;

	// HTTP fallback methods
	bool HTTPSendHeartbeat();
	bool HTTPPollCommands();
	void HTTPSendCommandResponse(const std::vector<uint8_t> &data);
	std::string BuildHTTPBase() const;

	NodeChannelConfig config;
	CommandHandler handler;
	std::unique_ptr<WSTransport> transport;
	std::vector<char> pendingRead;
	std::mutex sendMutex;
	std::atomic<bool> running{false};
	std::atomic<bool> connected{false};
	std::thread workerThread;
};