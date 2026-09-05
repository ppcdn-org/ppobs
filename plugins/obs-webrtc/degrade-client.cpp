#include "degrade-client.h"

#include <obs.hpp>
#include <nlohmann/json.hpp>
#include <util/platform.h>

#define do_log(level, fmt, ...) blog(level, "[degrade-client] " fmt, ##__VA_ARGS__)

// -------------------------------------------------------------------
// URL helper
// -------------------------------------------------------------------
static std::string whip_to_ws(const std::string &whip_url)
{
	std::string url = whip_url;
	while (!url.empty() && url.back() == '/')
		url.pop_back();

	if (url.compare(0, 8, "https://") == 0)
		url.replace(0, 8, "wss://");
	else if (url.compare(0, 7, "http://") == 0)
		url.replace(0, 7, "ws://");

	auto p = url.find("/whip");
	if (p != std::string::npos) {
		url = url.substr(0, p);
		url += "/ws/whip";
	}
	return url;
}

// -------------------------------------------------------------------
//  Singleton
// -------------------------------------------------------------------
WsDegradeClient &WsDegradeClient::Instance()
{
	static WsDegradeClient instance;
	return instance;
}

WsDegradeClient::WsDegradeClient()
	: client(),
	  conn(),
	  output(nullptr),
	  whip_url(),
	  ws_url(),
	  ws_secret(),
	  target_(),
	  mtx(),
	  running(true),
	  worker(),
	  last_layers(3),
	  last_pct(100)
{
	client.clear_access_channels(websocketpp::log::alevel::all);
	client.clear_error_channels(websocketpp::log::elevel::all);

	client.init_asio();

	client.set_open_handler([this](handle_t) {
		do_log(LOG_INFO, "WS connected to %s", ws_url.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		reconnect_backoff_ms = kReconnectBackoffMinMs;
	});

	client.set_close_handler([this](handle_t) {
		std::string reason;
		websocketpp::close::status::value code = websocketpp::close::status::abnormal_close;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (conn) {
				reason = conn->get_remote_close_reason();
				code = conn->get_remote_close_code();
			}
		}
		do_log(LOG_INFO, "WS closed (%s) code=%d reason=%s",
		       ws_url.c_str(),
		       (int)code,
		       reason.empty() ? "(none)" : reason.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		conn.reset();
		// Schedule a reconnect attempt; the worker loop's idle tick
		// picks this up (see ShouldReconnectLocked/ConnectLocked).
		// Without this, a dropped connection (e.g. code=1006 abnormal
		// close) just sits idle forever, silently losing the ability
		// to receive further degrade/recover commands for the rest of
		// the stream - RegisterOutput() can't recover it either, since
		// it returns early when the URL hasn't changed.
		next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
		reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
	});

	client.set_fail_handler([this](handle_t) {
		std::string ec_msg;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (conn) {
				ec_msg = conn->get_ec().message();
			}
		}
		do_log(LOG_INFO, "WS fail (%s) ec=%s",
		       ws_url.c_str(),
		       ec_msg.empty() ? "(unknown)" : ec_msg.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		conn.reset();
		next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
		reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
	});

	client.set_message_handler([this](handle_t h, client_t::message_ptr msg) {
		const std::string &payload = msg->get_payload();
		do_log(LOG_DEBUG, "WS Rx: %s", payload.c_str());

		TargetState ts;
		if (ParseTargetState(payload, ts)) {
			ApplyIfNeeded(ts);
			return;
		}

		// Protocol §2: terminate-state ALERT (no action, log only)
		try {
			auto j = nlohmann::json::parse(payload);
			if (j.contains("type") &&
			    j["type"] == "ALERT") {
				std::string path = j.value("path", "");
				std::string reason = j.value("reason", "");
				do_log(LOG_INFO,
				       "ALERT path=%s reason=%s",
				       path.c_str(),
				       reason.c_str());
			}
		} catch (const std::exception &) {
			// not valid JSON for our purpose, ignore
		}
	});

	worker = std::thread([this]() {
		while (running.load()) {
			// run() leaves the io_service in the "stopped" state
			// whenever it returns for lack of work, and a stopped
			// io_service services nothing and returns from run()
			// immediately. Restarting it here - while this thread
			// is provably not inside run() - is what lets a
			// connection queued after that point (by the reconnect
			// check below, or by RegisterOutput on another thread)
			// actually get processed.
			if (client.stopped())
				client.reset();

			client.run();
			// brief sleep to avoid busy-loop when no io work
			os_sleep_ms(50);

			std::lock_guard<std::mutex> lk(mtx);
			if (ShouldReconnectLocked())
				ConnectLocked();
		}
	});
}

WsDegradeClient::~WsDegradeClient()
{
	running.store(false);

	{
		std::lock_guard<std::mutex> lk(mtx);
		if (conn) {
			websocketpp::lib::error_code ec;
			conn->close(websocketpp::close::status::going_away, "shutdown", ec);
			conn.reset();
		}
	}

	client.stop();
	if (worker.joinable())
		worker.join();
}

// -------------------------------------------------------------------
//  Connection (re)establishment
// -------------------------------------------------------------------
bool WsDegradeClient::ShouldReconnectLocked() const
{
	return !conn && !ws_url.empty() && !ws_secret.empty() && next_reconnect_attempt_ns != 0 &&
	       os_gettime_ns() >= next_reconnect_attempt_ns;
}

void WsDegradeClient::ConnectLocked()
{
	next_reconnect_attempt_ns = 0;

	do_log(LOG_INFO, "Connecting to %s", ws_url.c_str());

	websocketpp::lib::error_code ec;
	conn = client.get_connection(ws_url, ec);
	if (ec) {
		do_log(LOG_ERROR, "Failed to create connection %s: %s", ws_url.c_str(), ec.message().c_str());
		conn.reset();
		// Retry later rather than giving up permanently - a transient
		// local resource error shouldn't need a full output restart
		// (RegisterOutput call) to recover from.
		next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)reconnect_backoff_ms * 1000000ULL;
		reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
		return;
	}

	conn->append_header("Authorization", "Bearer " + ws_secret);

	client.connect(conn);
}

// -------------------------------------------------------------------
//  Output registration
// -------------------------------------------------------------------
void WsDegradeClient::RegisterOutput(obs_output_t *out)
{
	std::lock_guard<std::mutex> lk(mtx);
	output = out;

	// Cache all video encoder pointers ONCE, the first time we ever see
	// this output (all_encoders starts empty and is never cleared
	// afterwards - see ApplyIfNeeded). RegisterOutput is called again
	// after every degrade/recover-triggered restart, and by then some
	// slots have deliberately been nulled out (fewer layers); rescanning
	// obs_output_get_video_encoder2 at that point would stop at the
	// first null slot and silently shrink max_layers / drop the cached
	// pointers for the higher layers, making it impossible to ever
	// recover back up to the real ceiling.
	if (all_encoders.empty()) {
		max_layers = 0;
		for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			auto *enc = obs_output_get_video_encoder2(out, i);
			if (!enc) break;
			all_encoders.push_back(enc);
			max_layers++;
		}
		do_log(LOG_INFO, "output '%s' registered (%d encoders)",
		       obs_output_get_name(out), max_layers);
	} else {
		do_log(LOG_DEBUG, "output '%s' re-registered after restart (max_layers=%d, unchanged)",
		       obs_output_get_name(out), max_layers);
	}

	obs_service_t *svc = obs_output_get_service(out);
	if (!svc)
		return;

	// Only connect if an explicit secret is configured; there is no
	// hard-coded fallback (unlike WHIP publish auth) since this
	// channel can remote-control encoder settings. Reuses the same
	// ppcenter App Secret the user already enters for ppcenter
	// publish/signal auth (see whip-output.cpp's Setup()).
	OBSDataAutoRelease service_settings = obs_service_get_settings(svc);
	std::string secret = obs_data_get_string(service_settings, "ppcenter_secret");
	if (secret.empty()) {
		do_log(LOG_DEBUG, "ppcenter_secret not set, degrade channel disabled");
		return;
	}
	ws_secret = secret;

	const char *url_c = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (!url_c || !url_c[0])
		return;

	std::string new_whip(url_c);
	std::string new_ws = whip_to_ws(new_whip);

	// Same endpoint: nothing to do here. This early return is why the
	// worker loop owns reconnection - a connection that dropped mid-stream
	// would otherwise never be reopened, since every subsequent
	// RegisterOutput() call (one per output restart) lands right here.
	if (new_ws == ws_url)
		return;

	whip_url = new_whip;
	ws_url = new_ws;

	// Close old connection
	if (conn) {
		websocketpp::lib::error_code ec;
		conn->close(websocketpp::close::status::going_away, "url-change", ec);
		conn.reset();
	}

	// Fresh endpoint: reset the backoff so the first attempt against it
	// isn't delayed by whatever the previous endpoint's failures ran up.
	reconnect_backoff_ms = kReconnectBackoffMinMs;
	ConnectLocked();
}

void WsDegradeClient::UnregisterOutput()
{
	std::lock_guard<std::mutex> lk(mtx);
	output = nullptr;

	// Stop reconnecting once nothing is streaming. Without this, only
	// nulling `output` left `conn`/`ws_url` untouched, so the worker
	// thread's idle tick (ShouldReconnectLocked/ConnectLocked) kept
	// retrying ws_url on its backoff schedule - up to
	// kReconnectBackoffMaxMs apart - forever, since the singleton's
	// destructor is otherwise the only thing that ever closes conn or
	// stops the worker. That both wasted network/CPU for as long as OBS
	// stayed open after "Stop Streaming", and made app exit wait on
	// whatever connect attempt the worker happened to be mid-retry on.
	// Clearing ws_url also matters for correctness, not just cleanup:
	// RegisterOutput()'s `new_ws == ws_url` check treats an unchanged
	// URL as "already connected, nothing to do", so leaving the old
	// value in place would make the next start to the same path silently
	// skip reconnecting at all.
	if (conn) {
		websocketpp::lib::error_code ec;
		conn->close(websocketpp::close::status::going_away, "output-unregistered", ec);
		conn.reset();
	}
	ws_url.clear();
	next_reconnect_attempt_ns = 0;

	do_log(LOG_INFO, "Output unregistered");
}

// ---------------------------------------------------------------------
//  JSON parsing
// ---------------------------------------------------------------------
bool WsDegradeClient::ParseTargetState(const std::string &json, TargetState &out)
{
	try {
		auto j = nlohmann::json::parse(json);
		if (!j.contains("type") || j["type"] != "TARGET_STATE")
			return false;
		if (j.contains("path"))
			out.path = j["path"];
		if (j.contains("layers"))
			out.layers = j["layers"];
		if (j.contains("bitrate_percent"))
			out.bitrate_percent = j["bitrate_percent"];
	} catch (const std::exception &e) {
		do_log(LOG_DEBUG, "JSON parse error: %s", e.what());
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------
//  Apply TARGET_STATE
// ---------------------------------------------------------------------
//
// A bitrate-only change is applied in place via obs_encoder_update(),
// which per obs-encoder.c is safe on an already-active encoder (the new
// settings are picked up on the encoder thread instead of being dropped).
// That keeps the common degrade/recover case - which the server can send
// at any time as the link fluctuates - completely invisible to viewers.
//
// A change in layer count still needs a restart:
// obs_output_set_video_encoder2() refuses to do anything (just logs a
// WARNING) while the output is active - see obs-output.c,
// obs_output_set_video_encoder2(): "tried to set video encoder on output
// ... while the output is still active!". So the encoder-slot
// manipulation MUST happen after the output has actually stopped, not
// before obs_output_stop() is even called. obs_output_stop() is
// asynchronous (WHIPOutput::Stop() spins up StopThread), so we poll
// obs_output_active() rather than guessing a fixed sleep duration.
void WsDegradeClient::ApplyIfNeeded(const TargetState &target)
{
	obs_output_t *out_copy = nullptr;
	int new_layers = 0;
	int prev_layers = 0;
	int prev_pct = 0;
	{
		std::lock_guard<std::mutex> lk(mtx);

		if (!output) {
			do_log(LOG_DEBUG, "no output registered, skip");
			return;
		}

		// Read current layer count
		int cur_layers = 0;
		for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			auto *enc = obs_output_get_video_encoder2(output, i);
			if (!enc)
				break;
			cur_layers++;
		}
		if (cur_layers == 0)
			cur_layers = 1;

		new_layers = target.layers;
		if (new_layers > max_layers)
			new_layers = max_layers;
		if (new_layers < 1)
			new_layers = 1;

		do_log(LOG_INFO,
		       "TARGET_STATE layers=%d (clamped %d) bitrate=%d%% | current layers=%d bitrate=%d%%",
		       target.layers,
		       new_layers,
		       target.bitrate_percent,
		       cur_layers,
		       last_pct);

		// Idempotency check, against the *clamped* layer count: a
		// server asking for more layers than this machine has encoders
		// for would otherwise never compare equal, forcing a pointless
		// restart on every message.
		if (cur_layers == new_layers && last_pct == target.bitrate_percent) {
			do_log(LOG_DEBUG, "already in target state, nothing to do");
			return;
		}

		prev_layers = last_layers;
		prev_pct = last_pct;
		last_layers = target.layers;
		last_pct = target.bitrate_percent;

		// Layer count already matches - this is a pure bitrate change,
		// so apply it live and skip the stop/restart entirely.
		if (cur_layers == new_layers) {
			ApplyBitrateLocked(output, cur_layers, target.bitrate_percent, true);
			do_log(LOG_INFO, "Applied bitrate=%d%% (live update, no restart)", target.bitrate_percent);
			return;
		}

		out_copy = output;
	}

	// --- Stop first (outside the mutex: Stop() re-enters this class via
	//     UnregisterOutput(), which also takes mtx) ---
	do_log(LOG_INFO, "Stopping output to apply layers=%d pct=%d%%",
	       new_layers, target.bitrate_percent);
	obs_output_stop(out_copy);

	// Wait for the output to actually go inactive - encoder-slot changes
	// are silently dropped by libobs otherwise. Capped so a stuck
	// output can't wedge this thread forever.
	const int max_wait_ms = 5000;
	int waited_ms = 0;
	while (obs_output_active(out_copy) && waited_ms < max_wait_ms) {
		os_sleep_ms(20);
		waited_ms += 20;
	}
	if (obs_output_active(out_copy)) {
		do_log(LOG_WARNING,
		       "output did not go inactive within %dms, skipping this TARGET_STATE (will retry on next message)",
		       max_wait_ms);
		// Roll back the recorded state: it was updated optimistically
		// above, and leaving it claiming we reached the target would
		// make the idempotency check swallow the identical retry this
		// message is waiting for.
		std::lock_guard<std::mutex> lk(mtx);
		last_layers = prev_layers;
		last_pct = prev_pct;
		return;
	}

	{
		std::lock_guard<std::mutex> lk(mtx);

		// --- Manipulate encoder slots so that WHIPOutput::Start()
		//     only sees |new_layers| encoders ---------------------
		//
		// all_encoders[0] is the full-resolution encoder (highest),
		// all_encoders[max_layers-1] is the lowest-resolution
		// simulcast layer (see WHIPSimulcastEncoders::Create /
		// SetStreamOutput). Reducing layers must drop the
		// highest-resolution layers first and keep the
		// lowest-resolution ones - i.e. keep the tail of
		// all_encoders, not the head.
		//
		// Remove superfluous encoders (null the slot)
		for (int i = new_layers; i < MAX_OUTPUT_VIDEO_ENCODERS; i++) {
			obs_output_set_video_encoder2(out_copy, nullptr, i);
		}

		// Restore the lowest-resolution |new_layers| cached encoders
		int first_kept = max_layers - new_layers;
		for (int i = 0; i < new_layers && (first_kept + i) < (int)all_encoders.size(); i++) {
			obs_output_set_video_encoder2(out_copy, all_encoders[first_kept + i], i);
		}

		// --- Update bitrate on every active encoder ----------
		// Not a live update: the output is stopped at this point, so
		// the encoders read these settings when it starts again below.
		ApplyBitrateLocked(out_copy, new_layers, target.bitrate_percent, false);
	}

	do_log(LOG_INFO, "Starting output (layers=%d pct=%d%%)",
	       new_layers, target.bitrate_percent);
	obs_output_start(out_copy);
}

void WsDegradeClient::ApplyBitrateLocked(obs_output_t *out, int count, int bitrate_percent, bool live)
{
	for (int i = 0; i < count; i++) {
		auto *enc = obs_output_get_video_encoder2(out, i);
		if (!enc) continue;

		OBSDataAutoRelease s = obs_encoder_get_settings(enc);
		int b = (int)obs_data_get_int(s, "bitrate");
		if (b < 1) b = 20000;

		// Cache base bitrate (per encoder) the first time we scale it,
		// so repeated degrade/recover messages always scale off the
		// original bitrate rather than compounding off a
		// previously-scaled value.
		int base = (int)obs_data_get_int(s, "base_bitrate");
		if (base == 0) {
			obs_data_set_int(s, "base_bitrate", b);
			base = b;
		}

		long long scaled = (long long)base * bitrate_percent / 100LL;
		if (scaled < 1) scaled = 1;
		obs_data_set_int(s, "bitrate", (int)scaled);

		// Under VBR the ceiling has to come down with the target, or a
		// degrade only moves the floor: the server has just said the
		// link can't carry the current rate, while the encoder stays
		// free to burst to the original peak on complex content -
		// exactly the wrong moment for it. Cached and scaled the same
		// way as the target above, so the peak/target ratio the user
		// configured survives repeated degrade/recover cycles.
		//
		// max_bitrate only means anything in VBR (NVENC and QSV both
		// read it solely on that path), so this is a no-op under CBR;
		// x264 has no such setting at all and reads 0 here, which
		// skips the block.
		int max_base = (int)obs_data_get_int(s, "base_max_bitrate");
		if (max_base == 0)
			max_base = (int)obs_data_get_int(s, "max_bitrate");

		if (max_base > 0) {
			obs_data_set_int(s, "base_max_bitrate", max_base);

			long long scaled_max = (long long)max_base * bitrate_percent / 100LL;
			// A ceiling below the target is not a ceiling.
			if (scaled_max < scaled)
				scaled_max = scaled;
			obs_data_set_int(s, "max_bitrate", (int)scaled_max);
		}

		// Push the new settings into a running encoder. Without this
		// the encoder keeps using whatever it read at start time, and
		// the change only lands on the next output restart.
		if (live)
			obs_encoder_update(enc, s);
	}
}
