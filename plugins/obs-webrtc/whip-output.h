#pragma once

#include "quality-score.h"
#include "whip-codec-session.h"

#include <obs-module.h>
#include <util/curl/curl-helper.h>
#include <util/platform.h>
#include <util/base.h>
#include <util/dstr.h>

#include <string>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class P2PSignalClient;
class UplinkQosPolicy;

// The libobs output object ("whip_output"). Wraps one or two independent
// WHIPCodecSession instances (H264 always; HEVC too when the HEVC/H264
// multitrack feature is enabled - see
// docs/design/whip-hevc-h264-multitrack-simulcast-design.zh-CN.md).
//
// Encoder-packet dispatch (Data()) routes each packet to whichever
// session(s) own it: video packets go to exactly the one session whose
// SetVideoLayers() encoder set includes packet->encoder; audio packets go
// to every active session (both codec sessions share the same Opus
// encoder output - see the design doc's §4.1 "音频不复制编码结果").
//
// P2P (direct ppobs<->ppplayer WebRTC, unrelated to any WHIP session) is
// keyed off the H264 session only, matching the pre-multitrack single-
// session behavior - see StartP2PSignal().
class WHIPOutput {
public:
	WHIPOutput(obs_data_t *settings, obs_output_t *output);
	~WHIPOutput();

	bool Start();
	void Stop(bool signal = true);
	void Data(struct encoder_packet *packet);

	inline size_t GetTotalBytes()
	{
		size_t total = h264Session ? h264Session->GetTotalBytes() : 0;
		if (hevcSession)
			total += hevcSession->GetTotalBytes();
		return total;
	}

	inline int GetConnectTime() { return connect_time_ms; }

private:
	bool Init();
	// Resolves ppcenter, builds every session's Config, and runs each
	// session's first Connect() concurrently (one thread per codec).
	// Returns false (and leaves every session torn down) unless *all*
	// requested sessions' first connect succeeded - see the design
	// doc's §4.1 "失败策略" strong-consistency start.
	bool Setup(uint64_t generation);
	void StartThread(uint64_t generation);
	void StopThread(bool signal, uint64_t generation);
	bool IsActiveGeneration(uint64_t generation) const;
	// Fired from a WHIPCodecSession's own worker thread once it gives
	// up for good; tears every sibling session down and stops the OBS
	// output. See WHIPCodecSession::Callbacks::onPermanentFailure.
	void OnSessionPermanentFailure(const std::string &codecLabel, const std::string &reason);
	bool StartP2PSignal();
	void CheckUplinkQos();

	obs_output_t *output;

	std::atomic<uint64_t> active_generation{0};
	std::atomic<bool> running{false};

	std::mutex start_stop_mutex;
	std::thread start_stop_thread;

	// Always present once Start() has assigned encoders; hevcSession is
	// only constructed when the service's "whip_hevc_h264_multitrack"
	// setting is on and a HEVC layer 0 encoder is actually attached
	// (see Setup()/UIValidation - the frontend blocks starting with the
	// checkbox on but no HEVC encoder configured).
	std::unique_ptr<WHIPCodecSession> h264Session;
	std::unique_ptr<WHIPCodecSession> hevcSession;
	std::atomic<bool> multitrack_permanent_failure{false};

	std::atomic<int> connect_time_ms{0};

	std::unique_ptr<P2PSignalClient> p2pSignal;
	std::string p2pToken;
	std::string p2pSignalUrl;
	std::string p2pStreamPath;
	std::vector<std::string> p2pStunServers;
	std::unique_ptr<UplinkQosPolicy> qosPolicy;
	int64_t lastQosCheckMs = 0;

	// Full-reference quality score (program feed = 100) for the top
	// layer; started from Start() based on the service's "quality_score"
	// setting, fed from Data(). Always scored against the H264 top
	// layer, matching pre-multitrack behavior.
	QualityScorer quality_scorer;

	// Reconnect timing, configurable from Settings > Output > Advanced;
	// forwarded into every WHIPCodecSession::Config.
	int disconnect_grace_sec = 5;
	int reconnect_backoff_sec = 3;
	int watchdog_interval_sec = 10;
	int watchdog_stall_sec = 30;
	bool enable_pacing = false;
	// 0 = retry forever. Non-zero only in multitrack mode - see the
	// design doc's "双轨重连超过配置上限时，停止整组输出".
	int multitrack_max_reconnect_attempts = 0;
};

void register_whip_output();
