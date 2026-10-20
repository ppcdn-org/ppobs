#include "srt-degrade-client.h"
#include "publish-loss-reconnect.h"

#include <obs.hpp>
#include <nlohmann/json.hpp>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define ASIO_STANDALONE 1
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#define do_log(level, fmt, ...) blog(level, "[srt-degrade-client] " fmt, ##__VA_ARGS__)

namespace {

constexpr int kReconnectBackoffMinMs = 2000;
constexpr int kReconnectBackoffMaxMs = 30000;

struct TargetState {
	int layers = 0;
	int bitrate_percent = 100;
};

bool ParseTargetState(const std::string &json, TargetState &out)
{
	try {
		auto j = nlohmann::json::parse(json);
		if (!j.contains("type") || j["type"] != "TARGET_STATE")
			return false;
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

// Identical transform to WsDegradeClient's whip_to_ws and
// PPCDNPublishEndpoint.cpp's DegradeWsURLFromWhipURL - duplicated rather
// than shared because this plugin module can't link against obs-webrtc and
// the frontend doesn't open this connection itself (see those two files'
// comments on the same function). ws_url here is already the final wss://
// control URL by the time it reaches srt_degrade_register - the frontend
// derives it from the WHIP track URL before writing it into service
// settings - so this file never needs to do the transform itself; kept only
// as a reference point for anyone comparing the three copies.

// All of this output's video encoder slots, sorted highest-resolution
// first - same convention as WsDegradeClient's collectCodecEncoders, so
// "keep the lowest N" means the same thing on both transports. An
// ffmpeg_mpegts_muxer output is always single-codec (see
// obs-ffmpeg-mpegts.c's setup_video_settings), so unlike WHIP's version this
// doesn't need to filter by codec - every attached slot belongs to the one
// ladder this output carries.
std::vector<obs_encoder_t *> collectEncoders(obs_output_t *output)
{
	std::vector<obs_encoder_t *> layers;
	if (!output)
		return layers;
	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		auto *encoder = obs_output_get_video_encoder2(output, idx);
		if (encoder)
			layers.push_back(encoder);
	}
	std::stable_sort(layers.begin(), layers.end(), [](obs_encoder_t *a, obs_encoder_t *b) {
		const uint64_t areaA = (uint64_t)obs_encoder_get_width(a) * obs_encoder_get_height(a);
		const uint64_t areaB = (uint64_t)obs_encoder_get_width(b) * obs_encoder_get_height(b);
		return areaA > areaB;
	});
	return layers;
}

struct Channel {
	using client_t = websocketpp::client<websocketpp::config::asio_tls_client>;
	using handle_t = websocketpp::connection_hdl;
	using conn_ptr = client_t::connection_ptr;
	using context_ptr = websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>;

	obs_output_t *output = nullptr;
	std::string codec;
	std::string ws_url;
	// ppcenter WHIP bearer token for this codec's whipTracks entry, sent as
	// "Authorization: Bearer <token>" - same credential a real WHIP publish
	// of this codec would use; mmx verifies it with WHIP_AUTH_KEY.
	std::string token;

	conn_ptr conn;
	TargetState target;
	bool target_set = false;
	int max_layers = 0;
	int last_pct = 100;
	uint64_t next_reconnect_attempt_ns = 0;
	int reconnect_backoff_ms = kReconnectBackoffMinMs;

	// True while this registry is stopping the output to restart it with a
	// new target - srt_degrade_unregister() keeps the target across that
	// stop instead of clearing it, same reasoning as WsDegradeClient's
	// restart_to_apply.
	bool restart_to_apply = false;

	// Fast-reconnect window fed by mmx's own LOSS_SAMPLE pushes (the
	// receive-side, normalized unrecoverable loss rate - see
	// internal/degrade/degrade.go's LossSampleMsg) instead of this output's
	// own sender-side srt_bstats(): same shared policy/thresholds as
	// obs-ffmpeg-mpegts.c's local monitor (plugins/common/
	// publish-loss-reconnect.h), just fed a trustworthy number instead of
	// one that reads systematically worse than WHIP's RTCP-derived one for
	// the same real loss. Armed fresh on every Register() call (mirrors
	// mpegts_srt_loss_arm's per-connect re-arm - a loss window from the
	// previous SRT connection must not count against this one). While this
	// is live (the channel connected and therefore receiving samples),
	// obs-ffmpeg-mpegts.c's own local monitor stands down - see
	// srt_degrade_connected()'s doc comment.
	struct publish_loss_monitor loss_monitor {};
	bool loss_monitor_armed = false;

	std::unique_ptr<client_t> client;
	std::thread worker;
	std::atomic<bool> running{true};
};

// Process-wide registry, keyed by the SRT output it belongs to - unlike
// WHIP's WsDegradeClient (one fixed h264/hevc channel pair shared by every
// output, because WHIP multitrack is two tracks on one output), an SRT
// HEVC/H264 multitrack publish is two *separate* single-codec outputs, so
// each one gets its own independent entry here with no cross-output state
// to reconcile.
class SrtDegradeRegistry {
public:
	static SrtDegradeRegistry &Instance()
	{
		static SrtDegradeRegistry instance;
		return instance;
	}

	void Register(obs_output_t *output, const std::string &codec, const std::string &ws_url,
		      const std::string &token);
	void Unregister(obs_output_t *output);
	int TargetLayers(obs_output_t *output) const;
	bool IsConnected(obs_output_t *output) const;

private:
	Channel *GetOrCreateLocked(obs_output_t *output, const std::string &codec);
	void StartWorker(Channel *ch);
	void ConfigureChannelLocked(Channel &ch, const std::string &ws_url, const std::string &token);
	void ConnectLocked(Channel &ch);
	bool ShouldReconnectLocked(const Channel &ch) const;
	void ApplyIfNeeded(Channel *ch, const TargetState &target);
	void ApplyBitrateLocked(Channel &ch, int bitrate_percent, bool live);
	void HandleLossSample(Channel *ch, uint64_t unrecoverable_delta, uint64_t expected_delta);
	bool RestartOutput(Channel *ch, const char *reason);

	mutable std::mutex mtx;
	std::unordered_map<obs_output_t *, std::unique_ptr<Channel>> channels;
};

Channel *SrtDegradeRegistry::GetOrCreateLocked(obs_output_t *output, const std::string &codec)
{
	auto it = channels.find(output);
	if (it != channels.end())
		return it->second.get();

	auto ch = std::make_unique<Channel>();
	ch->output = output;
	ch->codec = codec;
	Channel *chp = ch.get();

	chp->client = std::make_unique<Channel::client_t>();
	auto &c = *chp->client;
	c.clear_access_channels(websocketpp::log::alevel::all);
	c.clear_error_channels(websocketpp::log::elevel::all);
	c.init_asio();
	c.set_tls_init_handler([](Channel::handle_t) -> Channel::context_ptr {
		auto context = websocketpp::lib::make_shared<websocketpp::lib::asio::ssl::context>(
			websocketpp::lib::asio::ssl::context::tls_client);
		context->set_options(websocketpp::lib::asio::ssl::context::default_workarounds |
				     websocketpp::lib::asio::ssl::context::no_sslv2 |
				     websocketpp::lib::asio::ssl::context::no_sslv3);
		// The bundled OpenSSL build does not use the Windows root
		// certificate store. Keep the channel encrypted, but do not
		// reject the connection when the server certificate cannot be
		// verified locally - same tradeoff degrade-client.cpp makes.
		context->set_verify_mode(websocketpp::lib::asio::ssl::verify_none);
		return context;
	});

	c.set_open_handler([this, chp](Channel::handle_t) {
		do_log(LOG_INFO, "WS connected [%s] to %s", chp->codec.c_str(), chp->ws_url.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		chp->reconnect_backoff_ms = kReconnectBackoffMinMs;
	});

	c.set_close_handler([this, chp](Channel::handle_t) {
		std::string reason;
		int code = 0;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (chp->conn) {
				reason = chp->conn->get_remote_close_reason();
				code = (int)chp->conn->get_remote_close_code();
			}
		}
		do_log(LOG_INFO, "WS closed [%s] (%s) code=%d reason=%s", chp->codec.c_str(), chp->ws_url.c_str(),
		       code, reason.empty() ? "(none)" : reason.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		chp->conn.reset();
		if (!chp->ws_url.empty()) {
			chp->next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)chp->reconnect_backoff_ms * 1000000ULL;
			chp->reconnect_backoff_ms = std::min(chp->reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
		}
	});

	c.set_fail_handler([this, chp](Channel::handle_t) {
		std::string ec_msg;
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (chp->conn)
				ec_msg = chp->conn->get_ec().message();
		}
		do_log(LOG_INFO, "WS fail [%s] (%s) ec=%s", chp->codec.c_str(), chp->ws_url.c_str(),
		       ec_msg.empty() ? "(unknown)" : ec_msg.c_str());
		std::lock_guard<std::mutex> lk(mtx);
		chp->conn.reset();
		if (!chp->ws_url.empty()) {
			chp->next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)chp->reconnect_backoff_ms * 1000000ULL;
			chp->reconnect_backoff_ms = std::min(chp->reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
		}
	});

	c.set_message_handler([this, chp](Channel::handle_t, Channel::client_t::message_ptr msg) {
		const std::string &payload = msg->get_payload();
		do_log(LOG_DEBUG, "WS Rx [%s]: %s", chp->codec.c_str(), payload.c_str());

		TargetState ts;
		if (ParseTargetState(payload, ts)) {
			ApplyIfNeeded(chp, ts);
			return;
		}

		try {
			auto j = nlohmann::json::parse(payload);
			if (!j.contains("type"))
				return;
			if (j["type"] == "LOSS_SAMPLE") {
				HandleLossSample(chp, j.value("unrecoverable_delta", (uint64_t)0),
						 j.value("expected_delta", (uint64_t)0));
			} else if (j["type"] == "ALERT") {
				do_log(LOG_INFO, "ALERT path=%s reason=%s", j.value("path", "").c_str(),
				       j.value("reason", "").c_str());
			}
		} catch (const std::exception &) {
		}
	});

	channels.emplace(output, std::move(ch));
	StartWorker(chp);
	return chp;
}

void SrtDegradeRegistry::StartWorker(Channel *ch)
{
	ch->worker = std::thread([this, ch]() {
		while (ch->running.load()) {
			if (ch->client->stopped())
				ch->client->reset();
			ch->client->run();
			os_sleep_ms(50);

			std::lock_guard<std::mutex> lk(mtx);
			if (ShouldReconnectLocked(*ch))
				ConnectLocked(*ch);
		}
	});
}

bool SrtDegradeRegistry::ShouldReconnectLocked(const Channel &ch) const
{
	return !ch.conn && !ch.ws_url.empty() && !ch.token.empty() && ch.next_reconnect_attempt_ns != 0 &&
	       os_gettime_ns() >= ch.next_reconnect_attempt_ns;
}

void SrtDegradeRegistry::ConnectLocked(Channel &ch)
{
	ch.next_reconnect_attempt_ns = 0;

	do_log(LOG_INFO, "Connecting [%s] to %s", ch.codec.c_str(), ch.ws_url.c_str());

	websocketpp::lib::error_code ec;
	ch.conn = ch.client->get_connection(ch.ws_url, ec);
	if (ec) {
		do_log(LOG_ERROR, "Failed to create connection [%s] %s: %s", ch.codec.c_str(), ch.ws_url.c_str(),
		       ec.message().c_str());
		ch.conn.reset();
		ch.next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)ch.reconnect_backoff_ms * 1000000ULL;
		ch.reconnect_backoff_ms = std::min(ch.reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
		return;
	}

	ch.conn->append_header("Authorization", "Bearer " + ch.token);
	ch.client->connect(ch.conn);
}

void SrtDegradeRegistry::ConfigureChannelLocked(Channel &ch, const std::string &ws_url, const std::string &token)
{
	if (ws_url.empty() || token.empty()) {
		if (ch.conn) {
			websocketpp::lib::error_code ec;
			ch.conn->close(websocketpp::close::status::going_away, "unused", ec);
			ch.conn.reset();
		}
		ch.ws_url.clear();
		ch.token.clear();
		ch.next_reconnect_attempt_ns = 0;
		return;
	}

	// Same endpoint AND same credential: keep the live connection. A changed
	// token (ppcenter reissues one per publish session) must force a
	// reconnect, otherwise the channel would keep presenting the stale token.
	if (ws_url == ch.ws_url && token == ch.token)
		return;

	ch.ws_url = ws_url;
	ch.token = token;
	if (ch.conn) {
		websocketpp::lib::error_code ec;
		ch.conn->close(websocketpp::close::status::going_away, "url-or-token-change", ec);
		ch.conn.reset();
	}
	ch.reconnect_backoff_ms = kReconnectBackoffMinMs;
	ConnectLocked(ch);
}

void SrtDegradeRegistry::Register(obs_output_t *output, const std::string &codec, const std::string &ws_url,
				  const std::string &token)
{
	if (!output)
		return;

	std::lock_guard<std::mutex> lk(mtx);
	Channel *ch = GetOrCreateLocked(output, codec);

	// Track the layer ceiling as the largest ladder ever seen on this
	// output, not just the current count: a degrade-triggered restart
	// re-registers with a deliberately-trimmed encoder set, and that must
	// not lower the ceiling TARGET_STATE's layers gets clamped against (see
	// WsDegradeClient::RegisterOutput's identical reasoning).
	const int counted = (int)collectEncoders(output).size();
	if (counted > ch->max_layers)
		ch->max_layers = counted;

	// Fresh window every (re)connect - see loss_monitor's doc comment. Called
	// on every Register(), which obs-ffmpeg-mpegts.c's connect_mpegts_url()
	// does on every successful libsrt_open(), same point it arms its own
	// local monitor.
	struct publish_loss_config cfg;
	publish_loss_config_load(&cfg);
	publish_loss_monitor_init(&ch->loss_monitor, &cfg);
	ch->loss_monitor_armed = true;

	ConfigureChannelLocked(*ch, ws_url, token);
}

void SrtDegradeRegistry::Unregister(obs_output_t *output)
{
	std::unique_ptr<Channel> to_destroy;
	{
		std::lock_guard<std::mutex> lk(mtx);
		auto it = channels.find(output);
		if (it == channels.end())
			return;
		Channel *ch = it->second.get();

		if (ch->conn) {
			websocketpp::lib::error_code ec;
			ch->conn->close(websocketpp::close::status::going_away, "output-unregistered", ec);
			ch->conn.reset();
		}
		ch->ws_url.clear();
		ch->token.clear();
		ch->next_reconnect_attempt_ns = 0;
		ch->reconnect_backoff_ms = kReconnectBackoffMinMs;

		if (ch->restart_to_apply) {
			// Our own restart: Register() runs again within the next
			// connect attempt, keep the channel (and its target/
			// max_layers) alive rather than tearing down its worker
			// thread just to recreate one immediately.
			ch->restart_to_apply = false;
			return;
		}

		// A real stop (user-initiated, or a permanent failure): nothing
		// will re-register this output soon, if ever, so the worker
		// thread must not outlive it.
		ch->running.store(false);
		to_destroy = std::move(it->second);
		channels.erase(it);
	}
	if (to_destroy) {
		do_log(LOG_INFO, "Output unregistered [%s]", to_destroy->codec.c_str());
		to_destroy->client->stop();
		if (to_destroy->worker.joinable())
			to_destroy->worker.join();
	}
}

int SrtDegradeRegistry::TargetLayers(obs_output_t *output) const
{
	std::lock_guard<std::mutex> lk(mtx);
	auto it = channels.find(output);
	if (it == channels.end() || !it->second->target_set)
		return 0;
	return it->second->target.layers;
}

bool SrtDegradeRegistry::IsConnected(obs_output_t *output) const
{
	std::lock_guard<std::mutex> lk(mtx);
	auto it = channels.find(output);
	return it != channels.end() && (bool)it->second->conn;
}

// A bitrate-only change is applied in place via obs_encoder_update(), which
// per obs-encoder.c is safe on an already-active encoder. A layer-count
// change restarts the output; setup_video_settings() then reads
// srt_degrade_target_layers() and trims the ladder before the muxer streams
// are (re)created.
void SrtDegradeRegistry::ApplyIfNeeded(Channel *ch, const TargetState &target)
{
	bool layer_changed = false;
	TargetState prev;
	bool prev_set = false;
	int prev_pct = 100;
	{
		std::lock_guard<std::mutex> lk(mtx);

		int new_layers = target.layers;
		if (new_layers < 1)
			new_layers = 1;
		int actual_max = ch->max_layers > 0 ? ch->max_layers : (int)collectEncoders(ch->output).size();
		if (actual_max > 0 && new_layers > actual_max)
			new_layers = actual_max;

		int current = ch->target_set ? ch->target.layers : actual_max;
		if (!ch->target_set)
			layer_changed = (actual_max > 0 && new_layers != actual_max);
		else
			layer_changed = (new_layers != ch->target.layers);

		do_log(LOG_INFO, "TARGET_STATE codec=%s layers=%d (was %d) bitrate=%d%%", ch->codec.c_str(),
		       new_layers, current, target.bitrate_percent);

		if (!layer_changed && ch->target_set && ch->last_pct == target.bitrate_percent) {
			ch->target = target;
			ch->target.layers = new_layers;
			return;
		}

		prev = ch->target;
		prev_set = ch->target_set;
		prev_pct = ch->last_pct;

		ch->target = target;
		ch->target.layers = new_layers;
		ch->target_set = true;
		ch->last_pct = target.bitrate_percent;

		ApplyBitrateLocked(*ch, target.bitrate_percent, !layer_changed);

		if (!layer_changed) {
			do_log(LOG_INFO, "Applied codec=%s bitrate=%d%% (live, no restart)", ch->codec.c_str(),
			       target.bitrate_percent);
			return;
		}

	}

	char reason[96];
	snprintf(reason, sizeof(reason), "apply codec=%s layers=%d", ch->codec.c_str(), ch->target.layers);
	if (!RestartOutput(ch, reason)) {
		// Didn't go inactive in time: roll back the optimistic state update
		// so the idempotency check at the top doesn't swallow mmx's retry
		// (it resends TARGET_STATE on its own schedule).
		std::lock_guard<std::mutex> lk(mtx);
		ch->target = prev;
		ch->target_set = prev_set;
		ch->last_pct = prev_pct;
	}
}

// Stops then restarts ch->output, waiting (bounded) for it to go fully
// inactive before starting it again. Sets restart_to_apply before stopping
// so the symmetric srt_degrade_unregister() (obs-ffmpeg-mpegts.c's stop(),
// which obs_output_stop() below re-enters synchronously or asynchronously
// depending on the output type) knows this is a self-triggered restart and
// keeps this channel's target/max_layers/loss_monitor alive instead of
// clearing them. Shared by ApplyIfNeeded (a layer-count change) and
// HandleLossSample (the fast-reconnect window tripping) - the only
// difference between the two call sites is what they roll back if this
// returns false.
bool SrtDegradeRegistry::RestartOutput(Channel *ch, const char *reason)
{
	obs_output_t *out_copy;
	{
		std::lock_guard<std::mutex> lk(mtx);
		out_copy = ch->output;
		ch->restart_to_apply = true;
	}

	do_log(LOG_INFO, "Stopping output [%s]: %s", ch->codec.c_str(), reason);
	obs_output_stop(out_copy);

	const int max_wait_ms = 5000;
	int waited_ms = 0;
	while (obs_output_active(out_copy) && waited_ms < max_wait_ms) {
		os_sleep_ms(20);
		waited_ms += 20;
	}
	if (obs_output_active(out_copy)) {
		do_log(LOG_WARNING, "output [%s] did not go inactive within %dms, giving up on this restart",
		       ch->codec.c_str(), max_wait_ms);
		std::lock_guard<std::mutex> lk(mtx);
		ch->restart_to_apply = false;
		return false;
	}

	do_log(LOG_INFO, "Starting output [%s]", ch->codec.c_str());
	obs_output_start(out_copy);
	return true;
}

// Fast-reconnect window fed by mmx's receive-side LOSS_SAMPLE pushes - see
// Channel::loss_monitor's doc comment on why this exists alongside
// obs-ffmpeg-mpegts.c's own sender-side one. Reuses the exact same shared
// policy (plugins/common/publish-loss-reconnect.h) obs-ffmpeg-mpegts.c and
// WHIP's whip-codec-session.cpp already use, just fed a different (more
// trustworthy) number - unrecoverable_delta is already net of
// retransmission (see LossSampleMsg's doc comment in degrade.go), so this
// passes retrans=0, same convention WHIP's own local monitor already uses
// for the same reason.
void SrtDegradeRegistry::HandleLossSample(Channel *ch, uint64_t unrecoverable_delta, uint64_t expected_delta)
{
	if (!ch->loss_monitor_armed)
		return;

	bool have_sample = false;
	double sample_pct = 0.0;
	bool reconnect = publish_loss_monitor_record(&ch->loss_monitor, unrecoverable_delta, 0, expected_delta,
						     (int64_t)(os_gettime_ns() / 1000000), &have_sample, &sample_pct);

	if (have_sample && sample_pct > PUBLISH_LOSS_LOG_THRESHOLD_PCT)
		do_log(LOG_INFO, "mmx-reported loss %.2f%% this interval (codec=%s, unrecoverable=%llu/%llu)",
		       sample_pct, ch->codec.c_str(), (unsigned long long)unrecoverable_delta,
		       (unsigned long long)expected_delta);

	if (!reconnect)
		return;

	do_log(LOG_WARNING, "mmx-reported windowed unrecoverable loss above threshold (codec=%s), forcing reconnect",
	       ch->codec.c_str());
	RestartOutput(ch, "mmx-reported unrecoverable loss above local window threshold");
}

void SrtDegradeRegistry::ApplyBitrateLocked(Channel &ch, int bitrate_percent, bool live)
{
	auto encoders = collectEncoders(ch.output);
	int keep = (int)encoders.size();
	if (ch.target_set && ch.target.layers > 0 && ch.target.layers < keep)
		keep = ch.target.layers;

	for (int i = (int)encoders.size() - keep; i < (int)encoders.size(); i++) {
		auto *enc = encoders[i];
		if (!enc)
			continue;

		OBSDataAutoRelease s = obs_encoder_get_settings(enc);
		int b = (int)obs_data_get_int(s, "bitrate");
		if (b < 1)
			b = 20000;

		// Cache base bitrate (per encoder) the first time we scale it, so
		// repeated degrade/recover messages always scale off the original
		// rather than compounding off a previously-scaled value.
		int base = (int)obs_data_get_int(s, "base_bitrate");
		if (base == 0) {
			obs_data_set_int(s, "base_bitrate", b);
			base = b;
		}

		long long scaled = (long long)base * bitrate_percent / 100LL;
		if (scaled < 1)
			scaled = 1;
		obs_data_set_int(s, "bitrate", (int)scaled);

		int max_base = (int)obs_data_get_int(s, "base_max_bitrate");
		if (max_base == 0)
			max_base = (int)obs_data_get_int(s, "max_bitrate");

		if (max_base > 0) {
			obs_data_set_int(s, "base_max_bitrate", max_base);
			long long scaled_max = (long long)max_base * bitrate_percent / 100LL;
			if (scaled_max < scaled)
				scaled_max = scaled;
			obs_data_set_int(s, "max_bitrate", (int)scaled_max);
		}

		if (live)
			obs_encoder_update(enc, s);
	}
}

} // namespace

void srt_degrade_register(obs_output_t *output, const char *codec, const char *ws_url, const char *token)
{
	if (!output)
		return;
	SrtDegradeRegistry::Instance().Register(output, codec ? codec : "", ws_url ? ws_url : "",
						 token ? token : "");
}

void srt_degrade_unregister(obs_output_t *output)
{
	if (!output)
		return;
	SrtDegradeRegistry::Instance().Unregister(output);
}

int srt_degrade_target_layers(obs_output_t *output)
{
	if (!output)
		return 0;
	return SrtDegradeRegistry::Instance().TargetLayers(output);
}

bool srt_degrade_connected(obs_output_t *output)
{
	if (!output)
		return false;
	return SrtDegradeRegistry::Instance().IsConnected(output);
}
