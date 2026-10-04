#include "ppcenter-time-sync.h"
#include "ppcenter-time-sync-utils.h"

#include <util/base.h>
#include <util/ntp-clock.h>

#include <algorithm>
#include <exception>
#include <future>
#include <map>

namespace {
// Mirror pplayer's time-sync.mjs: take several probes spaced out, keep the
// lowest-RTT sample (least queuing/jitter skew), and resync periodically so a
// drifting local clock is corrected. The probe uses the same wire shape as
// pplayer - {"type":"TIME_SYNC","t1":<ms>} -> {"type":"TIME_SYNC_ACK",
// "t1":..,"t2":..,"t3":..}.
constexpr int PROBE_SAMPLE_COUNT = 6;
constexpr int PROBE_SPACING_MS = 150;
constexpr int PROBE_TIMEOUT_MS = 5000;
constexpr int RESYNC_INTERVAL_MS = 45000;
constexpr int RECONNECT_DELAY_MS = 3000;
constexpr int CONNECT_TIMEOUT_MS = 10000;

// One PpcenterTimeSync per ws://.../ws/play endpoint, shared by every output
// publishing to that ppcenter. The map holds weak_ptr so an entry does not
// keep an otherwise-idle sync alive; the last shared_ptr holder destroys it.
std::mutex g_registryMutex;
std::map<std::string, std::weak_ptr<PpcenterTimeSync>> g_registry;

int64_t NowWallMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}
} // namespace

PpcenterTimeSync::PpcenterTimeSync(const std::string &ppcenterPublishUrl)
	: wsUrl(DerivePpcenterTimeSyncUrl(ppcenterPublishUrl))
{
}

PpcenterTimeSync::~PpcenterTimeSync()
{
	Stop();
}

void PpcenterTimeSync::Start()
{
	if (!Valid() || running.exchange(true))
		return;
	blog(LOG_INFO, "[ppobs TimeSync] aligning clock to ppcenter via %s", wsUrl.c_str());
	thread = std::thread(&PpcenterTimeSync::Run, this);
}

void PpcenterTimeSync::Stop()
{
	if (!running.exchange(false))
		return;
	cv.notify_all();
	Disconnect();
	if (thread.joinable())
		thread.join();
}

void PpcenterTimeSync::Run()
{
	// The next probe burst is due on its own schedule, not "shortly after
	// whatever connect just succeeded". ppcenter (or a proxy in front of it)
	// closes the idle /ws/play socket well before RESYNC_INTERVAL_MS, and
	// treating each such close as a reason to re-probe turned the intended
	// 45s cadence into a resync every ~10s. A drop may reconnect early, but
	// it must not move the next probe forward.
	auto nextProbeAt = std::chrono::steady_clock::now();
	while (running.load()) {
		if (!ConnectOnce()) {
			WaitFor(std::chrono::milliseconds(RECONNECT_DELAY_MS));
			continue;
		}

		if (std::chrono::steady_clock::now() >= nextProbeAt) {
			std::vector<std::pair<int64_t, int64_t>> samples;
			for (int i = 0; i < PROBE_SAMPLE_COUNT && running.load(); ++i) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					if (!wsConnected)
						break;
				}
				ProbeOnce(samples);
				WaitFor(std::chrono::milliseconds(PROBE_SPACING_MS));
			}

			if (!samples.empty()) {
				auto best = std::min_element(
					samples.begin(), samples.end(),
					[](const std::pair<int64_t, int64_t> &a, const std::pair<int64_t, int64_t> &b) {
						return a.second < b.second;
					});
				ntp_clock_set_ppcenter_offset(best->first);
				blog(LOG_INFO, "[ppobs TimeSync] offset=%lldms rtt=%lldms (%zu/%d samples)",
				     (long long)best->first, (long long)best->second, samples.size(),
				     PROBE_SAMPLE_COUNT);
			}
			nextProbeAt = std::chrono::steady_clock::now() +
				      std::chrono::milliseconds(RESYNC_INTERVAL_MS);
		}

		// Hold the current offset until the next scheduled resync, waking
		// early only to reconnect a dropped socket.
		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait_until(lock, nextProbeAt,
				      [this] { return !running.load() || !wsConnected; });
		}
		Disconnect();
	}
}

bool PpcenterTimeSync::ConnectOnce()
{
	auto sock = std::make_shared<rtc::WebSocket>();
	auto opened = std::make_shared<std::promise<bool>>();
	auto openedFuture = opened->get_future();

	sock->onOpen([this, opened]() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			wsConnected = true;
		}
		try {
			opened->set_value(true);
		} catch (...) {
		}
		cv.notify_all();
	});
	sock->onError([this, opened](rtc::string err) {
		blog(LOG_WARNING, "[ppobs TimeSync] websocket error: %s", err.c_str());
		try {
			opened->set_value(false);
		} catch (...) {
		}
	});
	sock->onClosed([this, opened]() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			wsConnected = false;
		}
		// Resolve a still-pending handshake so a close without an error does
		// not block for the full connect timeout.
		try {
			opened->set_value(false);
		} catch (...) {
		}
		cv.notify_all();
	});
	sock->onMessage([](rtc::binary) {}, [this](rtc::string text) { OnMessage(text); });

	{
		std::lock_guard<std::mutex> lock(mutex);
		ws = sock;
	}
	try {
		sock->open(wsUrl);
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[ppobs TimeSync] open failed: %s", e.what());
		std::lock_guard<std::mutex> lock(mutex);
		if (ws == sock)
			ws.reset();
		wsConnected = false;
		return false;
	}

	if (!running.load()) {
		Disconnect();
		return false;
	}
	if (openedFuture.wait_for(std::chrono::milliseconds(CONNECT_TIMEOUT_MS)) != std::future_status::ready) {
		Disconnect();
		return false;
	}
	if (!openedFuture.get()) {
		Disconnect();
		return false;
	}
	return true;
}

void PpcenterTimeSync::Disconnect()
{
	std::shared_ptr<rtc::WebSocket> sock;
	{
		std::lock_guard<std::mutex> lock(mutex);
		sock = ws;
		ws.reset();
		wsConnected = false;
		probeComplete = false;
	}
	if (sock) {
		try {
			sock->close();
		} catch (...) {
		}
	}
}

bool PpcenterTimeSync::ProbeOnce(std::vector<std::pair<int64_t, int64_t>> &samples)
{
	const int64_t t1 = NowWallMs();

	std::shared_ptr<rtc::WebSocket> sock;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!ws || !wsConnected)
			return false;
		sock = ws;
		probeT1 = t1;
		probeComplete = false;
	}

	const std::string msg = "{\"type\":\"TIME_SYNC\",\"t1\":" + std::to_string(t1) + "}";
	if (!sock->isOpen() || !sock->send(msg))
		return false;

	std::unique_lock<std::mutex> lock(mutex);
	if (!cv.wait_for(lock, std::chrono::milliseconds(PROBE_TIMEOUT_MS),
			 [this] { return probeComplete || !running.load() || !wsConnected; }))
		return false;
	if (!probeComplete || probeT1 != t1)
		return false;

	const int64_t offset = ComputePpcenterClockOffsetMs(t1, probeT2, probeT3, probeT4);
	const int64_t rtt = (probeT4 - t1) - (probeT3 - probeT2);
	samples.emplace_back(offset, rtt < 0 ? 0 : rtt);
	return true;
}

void PpcenterTimeSync::OnMessage(const std::string &text)
{
	int64_t ackT1 = 0, t2 = 0, t3 = 0;
	if (!ExtractJsonIntField(text, "t1", &ackT1) || !ExtractJsonIntField(text, "t2", &t2) ||
	    !ExtractJsonIntField(text, "t3", &t3))
		return;

	const int64_t t4 = NowWallMs();
	std::lock_guard<std::mutex> lock(mutex);
	if (ackT1 != probeT1)
		return; // stale or unrelated reply
	probeT2 = t2;
	probeT3 = t3;
	probeT4 = t4;
	probeComplete = true;
	cv.notify_all();
}

void PpcenterTimeSync::WaitFor(std::chrono::milliseconds delay)
{
	std::unique_lock<std::mutex> lock(mutex);
	cv.wait_for(lock, delay, [this] { return !running.load(); });
}

std::shared_ptr<PpcenterTimeSync> AcquirePpcenterTimeSync(const std::string &ppcenterPublishUrl)
{
	const std::string wsUrl = DerivePpcenterTimeSyncUrl(ppcenterPublishUrl);
	if (wsUrl.empty())
		return nullptr;

	std::lock_guard<std::mutex> lock(g_registryMutex);
	auto it = g_registry.find(wsUrl);
	if (it != g_registry.end()) {
		if (auto existing = it->second.lock())
			return existing;
	}
	auto sync = std::make_shared<PpcenterTimeSync>(ppcenterPublishUrl);
	sync->Start();
	g_registry[wsUrl] = sync;
	return sync;
}
