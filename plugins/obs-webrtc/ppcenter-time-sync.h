#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rtc/rtc.hpp>

// Aligns ppobs's clock to ppcenter through the same /ws/play TIME_SYNC probe
// pplayer uses, so the SEI timestamp ppobs stamps and the render time pplayer
// measures share one time base. The measured offset is pushed into libobs's
// ntp-clock (ntp_clock_set_ppcenter_offset), which prefers it over the
// public-NTP offset while it is fresh; the existing public-NTP thread keeps
// running as a fallback.
//
// One instance is owned by a running WHIPOutput and stopped when it tears down.
class PpcenterTimeSync {
public:
	explicit PpcenterTimeSync(const std::string &ppcenterPublishUrl);
	~PpcenterTimeSync();
	PpcenterTimeSync(const PpcenterTimeSync &) = delete;
	PpcenterTimeSync &operator=(const PpcenterTimeSync &) = delete;

	// False when the configured ppcenter URL had no usable origin, in which
	// case Start() is a no-op.
	bool Valid() const { return !wsUrl.empty(); }

	void Start();
	void Stop();

private:
	void Run();
	bool ConnectOnce();
	void Disconnect();
	// Takes one TIME_SYNC measurement and appends {offsetMs, rttMs}.
	bool ProbeOnce(std::vector<std::pair<int64_t, int64_t>> &samples);
	void OnMessage(const std::string &text);
	void WaitFor(std::chrono::milliseconds delay);

	std::string wsUrl;

	std::thread thread;
	std::atomic<bool> running{false};

	std::mutex mutex;
	std::condition_variable cv;
	std::shared_ptr<rtc::WebSocket> ws;
	bool wsConnected = false;

	// One probe in flight at a time; OnMessage fills the reply fields. Guarded
	// by `mutex`.
	int64_t probeT1 = 0;
	int64_t probeT2 = 0;
	int64_t probeT3 = 0;
	int64_t probeT4 = 0;
	bool probeComplete = false;
};

// Returns a process-wide PpcenterTimeSync for the given ppcenter address,
// creating and starting it on first use. Every output publishing to the same
// ppcenter origin shares one instance (one /ws/play connection, one probe
// loop), so e.g. an SRT publish's P2P companion output does not open a second
// socket. The instance stops when the last holder drops its shared_ptr.
// Returns nullptr when the address has no usable origin.
std::shared_ptr<PpcenterTimeSync> AcquirePpcenterTimeSync(const std::string &ppcenterPublishUrl);
