#pragma once

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <rtc/rtc.hpp>

// One codec's independent WHIP publish session (H264 or HEVC) - see
// docs/design/whip-hevc-h264-multitrack-simulcast-design.zh-CN.md §4.1.
//
// Two-phase API, because the HEVC/H264 multitrack feature requires a
// strong-consistency start (§4.1 "失败策略": both codec sessions' *first*
// WHIP POST must succeed before WHIPOutput declares itself started; a
// first-attempt failure on either aborts the whole output, but a failure
// *after* that initial success only tears down and reconnects its own
// session):
//
//   1. Connect() - exactly one synchronous WHIP POST/SDP exchange, run on
//      whatever thread the caller calls it from (WHIPOutput runs both
//      sessions' Connect() concurrently on two threads it owns, so neither
//      codec's connect latency serializes behind the other's). Returns
//      false on any failure, including a permanently-rejected offer -
//      the caller decides what "first attempt failed" means for the
//      whole multi-codec output and must call Abort() to clean up state
//      left behind by the failed attempt.
//   2. Supervise() - only after a successful Connect(): spawns this
//      session's own worker thread, which watches the live connection
//      (PeerConnection state, stall watchdog) and, on any subsequent
//      disconnect, reconnects independently with its own linear backoff -
//      exactly like the pre-multitrack single-session WHIPOutput did,
//      just scoped to one codec. Only a reconnect-budget exhaustion or a
//      newly-rejected offer after that point reaches
//      Callbacks::onPermanentFailure.
//
// A codec session never calls obs_output_* start/stop/signal functions -
// only the owning WHIPOutput does that, once it has decided (via
// Callbacks::onPermanentFailure, or an Abort()ed first Connect()) that
// every sibling session should also be torn down.
class WHIPCodecSession {
public:
	WHIPCodecSession(obs_output_t *output, std::string label);
	~WHIPCodecSession();

	struct Config {
		std::string endpoint_url;
		std::string bearer_token;
		bool has_audio = true;
		bool enable_pacing = false;
		int disconnect_grace_sec = 5;
		int reconnect_backoff_sec = 3;
		int watchdog_interval_sec = 10;
		int watchdog_stall_sec = 30;
		// 0 = retry forever (matches the pre-multitrack single-session
		// behavior). WHIPOutput sets a finite budget in multitrack
		// mode so one permanently-dead codec endpoint can't retry
		// forever while its sibling session is healthy - see the
		// design doc's "双轨重连超过配置上限时，停止整组输出".
		int max_reconnect_attempts = 0;
	};

	struct Callbacks {
		// A session Supervise() was watching is now permanently gone:
		// either its own reconnect budget ran out, or the server
		// rejected a reconnect offer outright (401/403/404/406, not
		// retryable). The session has already torn itself down by
		// the time this fires. Runs on this session's own worker
		// thread - must not block, and must not call this session's
		// own Stop() (that would self-join); calling Stop() on
		// sibling sessions is fine.
		std::function<void(const std::string &reason)> onPermanentFailure;
	};

	// Registers the video layer encoders this session owns, in rid order
	// (rid "0" = layers[0], highest quality first). Must be called
	// before Connect().
	void SetVideoLayers(const std::vector<obs_encoder_t *> &layers);

	bool OwnsVideoEncoder(obs_encoder_t *encoder) const;

	// One synchronous WHIP POST/SDP exchange. Blocks the calling thread
	// for the duration of the HTTP round-trip (curl timeout: 8s). On
	// success, the session is live (SendVideo()/SendAudio() work) but
	// unsupervised - the caller must follow up with either Supervise()
	// or Abort().
	bool Connect(const Config &config);

	// Arms the reconnect/watchdog worker thread after a successful
	// Connect(). Must be called at most once per Connect() success.
	void Supervise(Callbacks callbacks);

	// Tears down a session whose Connect() just failed (or that was
	// never connected at all) without going through Supervise() -
	// releases any partially-established PeerConnection. Safe even if
	// Connect() was never called.
	void Abort();

	// Signals the worker thread (if Supervise() was called) to tear
	// down (DELETEing the WHIP resource, best-effort) and stop for
	// good, then blocks until it has exited. If Supervise() was never
	// called after a successful Connect(), also tears down that
	// unsupervised connection. Safe to call from any thread other than
	// this session's own worker thread. Idempotent.
	void Stop();

	void SendVideo(struct encoder_packet *packet);
	void SendAudio(struct encoder_packet *packet);

	size_t GetTotalBytes() const { return total_bytes_sent.load(); }
	int GetConnectTimeMs() const { return connect_time_ms.load(); }
	const std::string &Label() const { return label; }

private:
	struct videoLayerState {
		uint16_t sequenceNumber = 0;
		uint32_t rtpTimestamp = 0;
		int64_t lastVideoTimestamp = 0;
		uint32_t ssrc = 0;
		std::string rid;
	};

	enum class PCEvent { None, Connected, Disconnected, Failed, Closed };

	void WorkerLoop(uint64_t generation);
	// Blocks (waking early on any PCEvent, watchdog progress check tick,
	// or Stop()) until the live connection should be torn down; returns
	// the reason.
	const char *RunUntilDisconnect(uint64_t generation);
	// Reconnect loop body: waits out this attempt's backoff, then
	// Connect()s a fresh generation. Returns false (worker thread should
	// exit) on Stop(), reconnect-budget exhaustion, or a permanently
	// rejected offer - in the latter two cases also fires
	// onPermanentFailure.
	bool ReconnectOnce(const char *disconnectReason);

	void ConfigureAudioTrack(const std::string &media_stream_id, const std::string &cname);
	void ConfigureVideoTrack(const std::string &media_stream_id, const std::string &cname);
	bool DoConnect(uint64_t generation, std::string &resourceURL);
	void TeardownGeneration(uint64_t generation, const std::string &resourceURL, const char *reason, bool wasConnected);
	void SendDeleteResource(const std::string &resourceURL, uint64_t generation, const char *reason);
	void ParseLinkHeader(std::string linkHeader, std::vector<rtc::IceServer> &iceServers);
	void Send(const void *data, uintptr_t size, uint64_t duration, std::shared_ptr<rtc::Track> track,
		  std::shared_ptr<rtc::RtcpSrReporter> rtcp_sr_reporter);
	bool IsActiveGeneration(uint64_t generation) const;
	// Sleeps (interruptibly, waking early on Stop()) for this attempt's
	// linear backoff. Returns false if Stop() interrupted the wait.
	bool WaitBackoff();
	void ClosePeerConnectionWithTimeout(std::shared_ptr<rtc::PeerConnection> pc);

	obs_output_t *output;    // borrowed - never started/stopped/signaled directly
	std::string label;       // "h264" / "hevc", log-only
	std::string video_codec; // read from layer[0]'s encoder at SetVideoLayers() time

	Config cfg;
	Callbacks callbacks;

	std::mutex start_stop_mutex;
	std::thread worker_thread;
	std::atomic<uint64_t> active_generation{0};
	std::atomic<bool> stop_requested{false};
	std::atomic<bool> connected_unsupervised{false}; // true between a successful Connect() and Supervise()/Abort()
	std::string resourceURL_unsupervised;

	// Wakes RunUntilDisconnect's wait and WaitBackoff's sleep early.
	// Every writer (PeerConnection state callback, Stop()) sets the
	// relevant flag(s) below before notifying.
	std::mutex cv_mutex;
	std::condition_variable cv;
	std::atomic<PCEvent> pending_pc_event{PCEvent::None};

	std::string resource_url;
	std::mutex resource_mutex;

	std::atomic<bool> sending_enabled{false};

	std::shared_mutex tracks_mutex;
	uint32_t base_ssrc;
	std::shared_ptr<rtc::PeerConnection> peer_connection;
	std::shared_ptr<rtc::Track> audio_track;
	std::shared_ptr<rtc::Track> video_track;
	std::shared_ptr<rtc::RtcpSrReporter> audio_sr_reporter;
	std::shared_ptr<rtc::RtcpSrReporter> video_sr_reporter;
	std::shared_ptr<rtc::DataChannel> timestamp_channel;

	std::vector<obs_encoder_t *> video_layers; // in rid order
	std::map<obs_encoder_t *, std::shared_ptr<videoLayerState>> videoLayerStates;

	std::atomic<size_t> total_bytes_sent{0};
	std::atomic<int> connect_time_ms{0};
	int64_t start_time_ns = 0;
	int64_t last_audio_timestamp = 0;

	std::atomic<int> reconnect_attempt{0};
	// Set by DoConnect() when the server rejected the offer outright
	// (401/403/404/406) - a condition retrying can never fix.
	std::atomic<bool> last_failure_permanent{false};

	int close_timeout_sec = 5;
};
