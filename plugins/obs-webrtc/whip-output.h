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
class PpcenterTimeSync;

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
	// p2pOnly runs ONLY the P2P publisher path (NAT probe + signaling +
	// WebRTC-to-player feed off the shared encoders) and never opens a WHIP
	// media session - used by the ppcenter_p2p_output so an SRT (or any
	// non-WHIP) publish can still join the P2P mesh. See
	// docs/design/ppobs-p2p-srt-publish-support.zh-CN.md.
	WHIPOutput(obs_data_t *settings, obs_output_t *output, bool p2pOnly = false);
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
	// Keeps ppcenter's publisher-side NAT observation from going stale over
	// a long stream - see CheckNatProbeRefresh()'s own comment.
	void CheckNatProbeRefresh();
	// Returns a currently-valid publisher signal token, minting a replacement
	// from ppcenter when the cached one is close to expiring. Called by
	// P2PSignalClient before each connection attempt - see its own comment.
	// force=true (a previous signaling connect attempt already failed) lets a
	// stale token be refreshed on the very next 5s reconnect instead of
	// waiting out P2P_TOKEN_REFRESH_MIN_INTERVAL_MS.
	std::string AcquireP2PToken(bool force);

	obs_output_t *output;

	// When true, Setup() configures the P2P signaling but returns before any
	// WHIP session is created, and StartThread() starts only the P2P path -
	// see the ctor comment and register_whip_output()'s ppcenter_p2p_output.
	bool p2p_only = false;

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
	// Aligns ppobs's clock to ppcenter (same /ws/play TIME_SYNC probe pplayer
	// uses) so both share one time base for the SEI delay measurement. Shared
	// across outputs publishing to the same ppcenter (see
	// AcquirePpcenterTimeSync); acquired in Setup(), released in StopThread().
	std::shared_ptr<PpcenterTimeSync> timeSync;
	// p2pToken is read by the signaling thread (AcquireP2PToken, via the token
	// provider) and written by both Setup() and AcquireP2PToken(); guard it
	// with p2pTokenMutex. lastP2PTokenRefreshMs throttles unforced refresh
	// attempts so a persistently failing /v1/publish/p2p-token cannot turn the
	// signaling reconnect loop into a request storm (a forced refresh, after a
	// signaling connect failure, intentionally bypasses it).
	std::mutex p2pTokenMutex;
	std::string p2pToken;
	int64_t lastP2PTokenRefreshMs = 0;
	std::string p2pSignalUrl;
	std::string p2pStreamPath;
	std::vector<std::string> p2pStunServers;
	// Platform-set cap on simultaneous P2P peers (from the publish response);
	// forwarded to P2PSignalClient. Defaults to the historical 3.
	int p2pMaxSessions = 3;
	// The single video encoder whose packets feed the P2P peers. P2P
	// announces one H264 m-line per peer (P2PSignalClient::CreatePeerVideo),
	// so Data() must forward exactly one layer: layers[0], i.e. rid "0" /
	// highest quality, the same layer the WHIP path labels rid "0". Null
	// when no H264 layer is attached, in which case P2P carries no video at
	// all rather than an undecodable mix of layers.
	obs_encoder_t *p2pVideoEncoder = nullptr;

	// TEMPORARY P2P feed diagnostics (2026-09-30): the P2P leg reaches
	// ICE+DTLS+SRTP connected on both ends but the browser decodes no frame,
	// and nothing in the log says whether Data() is actually feeding the
	// peers. Rolled up once every 2s while P2P is enabled, so a repro shows
	// whether p2pVideoEncoder still matches, whether the feed is stuck
	// waiting for a keyframe, or whether send() is throwing (the catch that
	// used to swallow it now counts + logs the first few). Logged at Debug
	// only, so it is off at the default log level. Remove once the feed
	// regression is understood.
	struct P2PFeedCounters {
		uint64_t videoPackets = 0;   // every OBS video packet seen by Data()
		uint64_t encoderMatched = 0; // packet->encoder == p2pVideoEncoder
		uint64_t sent = 0;           // videoTrack->send() calls that returned
		uint64_t skippedNoTrack = 0; // peer.videoTrack null or !isOpen()
		uint64_t skippedNoKey = 0;   // !videoStarted && !packet->keyframe
		uint64_t sendErrors = 0;     // videoTrack->send() threw
		// Last RTP state actually pushed for a sent frame - reveals a stuck
		// timestamp (one never-ending AU the browser can't decode) or a
		// sequence-number jump.
		uint32_t lastRtpTimestamp = 0;
		uint16_t lastSequenceNumber = 0;
		int64_t lastDtsUsec = 0;
	};
	P2PFeedCounters p2pFeedCounters;
	int64_t lastP2PFeedLogMs = 0;

	// The resolved per-codec WHIP endpoints Setup() got from ppcenter,
	// reused by StartThread() to derive each codec's degrade control
	// channel ws:// URL. Both are needed when HEVC/H264 multitrack is on
	// (two independent publish paths, each with its own FSM). The H264 one
	// must also be set in p2p_only mode: an SRT publish runs the P2P-only
	// companion, and mmx serves /{path}/ws/whip on the same WebRTC server,
	// so the SRT session's degrade channel is derived from the resolved H264
	// track URL. Empty when that codec isn't published.
	std::string degrade_url_h264;
	std::string degrade_url_hevc;
	// The ppcenter-issued publish bearer token for each codec's WHIP track.
	// The degrade WS reuses this same credential (verified by mmx with
	// WHIP_AUTH_KEY), so it must travel with the URL above - there is no
	// separate degrade secret any more.
	std::string degrade_token_h264;
	std::string degrade_token_hevc;
	std::unique_ptr<UplinkQosPolicy> qosPolicy;
	int64_t lastQosCheckMs = 0;

	// Cached from Setup()'s PPCenterPublishRequest so CheckNatProbeRefresh()
	// (called repeatedly from Data(), long after Setup() has returned) can
	// resubmit the same probe without re-reading service settings.
	std::string natProbeUrl;
	std::string natProbeAppId;
	std::string natProbeAppSecret;
	std::string natProbeStreamName;
	// The deviceId Setup() resolved and sent to /v1/publish/requests - must
	// be reused verbatim as every NAT probe's clientId (initial and
	// refreshed alike), or ppcenter's identity check rejects the pair as
	// identity_mismatch. See ProbePublisherNAT's doc comment.
	std::string natProbeClientId;
	// The probe_id the last successful probe registered. Re-sent when
	// AcquireP2PToken() re-runs /v1/publish/requests, so the fresh publisher
	// token carries the same natProbeId ppcenter already has on file.
	std::string natProbeId;
	int64_t lastNatProbeRefreshMs = 0;

	// Encoder-parameter report (see encoder-report.h): the actual encoder
	// config is captured in Setup() and sent once the output is running in
	// StartThread(), so a start that never goes live reports nothing.
	std::string encoder_report_url;
	std::string encoder_report_body;
	bool encoder_report_pending = false;

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
