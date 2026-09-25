#pragma once

#include <obs-module.h>

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include <cstdint>

// One codec's target state, pushed by mmx as TARGET_STATE. Under HEVC/H264
// multitrack each codec is a separate publish path (app/stream/h264,
// app/stream/hevc) with its own FSM, so a target state is per-codec.
struct TargetState {
	std::string path;
	// "h264"/"hevc" - mmx sends it (derived from the path suffix) so the
	// executor can validate and apply to the right encoder group.
	std::string codec;
	int layers = 3;
	int bitrate_percent = 100;
	// Server-side receiver latency the mmx node is applying to this path.
	// Informational only - the client does not act on it, mmx owns it.
	int latency_ms = 0;
};

// OBS-side executor for the mmx degrade protocol (see
// docs/design/publish-degrade-protocol.zh-CN.md). Holds one control channel
// per codec that is actually published (WHIP multitrack publishes two; SRT
// and non-multitrack WHIP publish one), and applies each channel's target to
// that codec's encoders only:
//   * a bitrate-only change is applied live via obs_encoder_update();
//   * a layer-count change restarts the output, and WHIPOutput::Setup()
//     reads TargetLayers() to trim that codec's simulcast ladder down to
//     the lowest-resolution N layers.
// Layers are never removed by detaching encoders from the output, so the two
// codecs can't disturb each other's slots (the bug a single flat encoder
// list caused under multitrack).
class WsDegradeClient {
public:
	static WsDegradeClient &Instance();

	// Registers the output together with the per-codec WHIP endpoint and its
	// ppcenter publish bearer token. The control channel URL is derived from
	// the WHIP URL, and it is authenticated with the same token (mmx verifies
	// it with WHIP_AUTH_KEY) - there is no separate degrade secret. Either
	// pair may be empty (that codec is not published). Safe to call again
	// after every output (re)start - an unchanged URL+token keeps its
	// connection.
	void RegisterOutput(obs_output_t *output, const std::string &h264_whip_url,
			    const std::string &h264_token, const std::string &hevc_whip_url,
			    const std::string &hevc_token);

	void UnregisterOutput();

	// Number of layers the given codec should publish, or 0 when no target
	// has been received for it (no trimming). Read from WHIPOutput::Setup().
	int TargetLayers(const std::string &codec) const;

private:
	struct Channel;

	WsDegradeClient();
	~WsDegradeClient();

	WsDegradeClient(const WsDegradeClient &) = delete;
	WsDegradeClient &operator=(const WsDegradeClient &) = delete;

	bool ParseTargetState(const std::string &json, TargetState &out);
	void ApplyIfNeeded(Channel &ch, const TargetState &target);
	void ApplyBitrateLocked(Channel &ch, obs_output_t *out, int bitrate_percent, bool live);

	std::vector<obs_encoder_t *> codecEncoders(const std::string &codec) const;

	Channel *FindChannel(const std::string &codec);
	const Channel *FindChannel(const std::string &codec) const;

	void ConfigureChannelLocked(Channel &ch, const std::string &whip_url, const std::string &token);
	void ConnectLocked(Channel &ch);
	bool ShouldReconnectLocked(const Channel &ch) const;

	obs_output_t *output;

	std::vector<std::unique_ptr<Channel>> channels;

	mutable std::mutex mtx;
	std::atomic<bool> running;
};
