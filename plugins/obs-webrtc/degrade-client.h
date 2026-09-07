#pragma once

#include <obs-module.h>

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cstdint>

#define ASIO_STANDALONE 1
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

struct TargetState {
	std::string path;
	int layers = 3;
	int bitrate_percent = 100;
};

class WsDegradeClient {
	using client_t = websocketpp::client<websocketpp::config::asio_tls_client>;
	using handle_t = websocketpp::connection_hdl;
	using conn_ptr = client_t::connection_ptr;
	using context_ptr = websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>;

public:
	static WsDegradeClient &Instance();

	void RegisterOutput(obs_output_t *output, const std::string &whip_url);
	void UnregisterOutput();

	TargetState currentTarget() const { return target_; }

private:
	WsDegradeClient();
	~WsDegradeClient();

	WsDegradeClient(const WsDegradeClient &) = delete;
	WsDegradeClient &operator=(const WsDegradeClient &) = delete;

	bool ParseTargetState(const std::string &json, TargetState &out);
	void ApplyIfNeeded(const TargetState &state);

	// Scales the bitrate of encoder slots [0, count) by bitrate_percent.
	// When live is true each encoder is reconfigured in place via
	// obs_encoder_update() (safe on an active encoder - the settings are
	// picked up on the encoder thread rather than dropped); otherwise the
	// settings object is just updated for the encoder to read when the
	// output next starts. Assumes mtx is held.
	void ApplyBitrateLocked(obs_output_t *out, int count, int bitrate_percent, bool live);

	// Opens a connection to ws_url. Assumes mtx is held, ws_url and
	// ws_secret are non-empty, and any existing conn has been reset by the
	// caller. Shared by RegisterOutput() (first connect / URL change) and
	// the worker loop's reconnect check (same URL, connection dropped -
	// see the close/fail handlers and ShouldReconnectLocked).
	void ConnectLocked();

	// True if we should attempt (re)connecting: we have a URL and secret,
	// aren't already connected/connecting, and the backoff delay set by
	// the close/fail handlers has elapsed. Assumes mtx is held.
	bool ShouldReconnectLocked() const;

	client_t client;
	conn_ptr conn;

	obs_output_t *output;
	std::string whip_url;
	std::string ws_url;

	// Bearer secret for the control channel, cached from the service
	// settings at RegisterOutput() time so a reconnect can re-authenticate
	// without needing the service to still be reachable.
	std::string ws_secret;

	TargetState target_;
	mutable std::mutex mtx;

	std::atomic<bool> running;
	std::thread worker;

	int last_layers;
	int last_pct;

	// Reconnect backoff state (protected by mtx). The close/fail handlers
	// reset conn to null and schedule the next retry via these; the worker
	// loop's idle tick checks ShouldReconnectLocked() and calls
	// ConnectLocked() once the deadline passes. Doubles on each
	// consecutive failure, capped, and resets once open_handler fires.
	uint64_t next_reconnect_attempt_ns = 0;
	int reconnect_backoff_ms = 2000;
	static constexpr int kReconnectBackoffMinMs = 2000;
	static constexpr int kReconnectBackoffMaxMs = 30000;

	// Cached encoder pointers (all slots), saved at RegisterOutput time.
	// Index 0 is the highest-resolution (full-res) encoder, index
	// max_layers-1 is the lowest-resolution simulcast layer. We use
	// obs_output_set_video_encoder2(out, nullptr, idx) to temporarily
	// remove the highest-resolution layers when degrading (see
	// docs/obs-mmx-degrade-protocol.md).
	std::vector<obs_encoder_t *> all_encoders;
	int max_layers;
};
