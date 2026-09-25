#include "degrade-client.h"

#include <obs.hpp>
#include <nlohmann/json.hpp>
#include <util/platform.h>

#include <algorithm>

#define ASIO_STANDALONE 1
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#define do_log(level, fmt, ...) blog(level, "[degrade-client] " fmt, ##__VA_ARGS__)

namespace {

constexpr int kReconnectBackoffMinMs = 2000;
constexpr int kReconnectBackoffMaxMs = 30000;

// WHIP endpoint -> its derive control-channel endpoint: same host/port,
// http->ws / https->wss, and the trailing ".../<codec>/whip" becomes
// ".../<codec>/ws/whip" (mmx serves the degrade WS on its WebRTC server,
// keyed by the full path including the codec segment).
std::string whip_to_ws(const std::string &whip_url)
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

// All of one codec's video encoders currently attached to the output, sorted
// highest-resolution first. Gaps are skipped (not treated as end-of-list) so
// this stays correct regardless of how the frontend packs slots.
std::vector<obs_encoder_t *> collectCodecEncoders(obs_output_t *output, const std::string &codec)
{
	std::vector<obs_encoder_t *> layers;
	if (!output)
		return layers;
	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		auto *encoder = obs_output_get_video_encoder2(output, idx);
		if (!encoder)
			continue;
		const char *c = obs_encoder_get_codec(encoder);
		if (c && codec == c)
			layers.push_back(encoder);
	}
	std::stable_sort(layers.begin(), layers.end(), [](obs_encoder_t *a, obs_encoder_t *b) {
		const uint64_t areaA = (uint64_t)obs_encoder_get_width(a) * obs_encoder_get_height(a);
		const uint64_t areaB = (uint64_t)obs_encoder_get_width(b) * obs_encoder_get_height(b);
		return areaA > areaB;
	});
	return layers;
}

} // namespace

struct WsDegradeClient::Channel {
	using client_t = websocketpp::client<websocketpp::config::asio_tls_client>;
	using handle_t = websocketpp::connection_hdl;
	using conn_ptr = client_t::connection_ptr;
	using context_ptr = websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>;

	std::string codec;
	std::string ws_url;
	conn_ptr conn;
	TargetState target;
	bool target_set = false;
	int max_layers = 0;
	int last_pct = 100;
	uint64_t next_reconnect_attempt_ns = 0;
	int reconnect_backoff_ms = kReconnectBackoffMinMs;
	std::unique_ptr<client_t> client;
	std::thread worker;
};

WsDegradeClient &WsDegradeClient::Instance()
{
	static WsDegradeClient instance;
	return instance;
}

WsDegradeClient::WsDegradeClient()
	: output(nullptr), ws_secret(), channels(), mtx(), running(true)
{
	for (const char *codec : {"h264", "hevc"}) {
		auto ch = std::make_unique<Channel>();
		ch->codec = codec;
		Channel *chp = ch.get();

		ch->client = std::make_unique<Channel::client_t>();
		auto &c = *ch->client;
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
			// verified locally.
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
			do_log(LOG_INFO, "WS closed [%s] (%s) code=%d reason=%s", chp->codec.c_str(),
			       chp->ws_url.c_str(), code, reason.empty() ? "(none)" : reason.c_str());
			std::lock_guard<std::mutex> lk(mtx);
			chp->conn.reset();
			if (!chp->ws_url.empty()) {
				chp->next_reconnect_attempt_ns =
					os_gettime_ns() + (uint64_t)chp->reconnect_backoff_ms * 1000000ULL;
				chp->reconnect_backoff_ms =
					std::min(chp->reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
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
				chp->next_reconnect_attempt_ns =
					os_gettime_ns() + (uint64_t)chp->reconnect_backoff_ms * 1000000ULL;
				chp->reconnect_backoff_ms =
					std::min(chp->reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
			}
		});

		c.set_message_handler([this, chp](Channel::handle_t, Channel::client_t::message_ptr msg) {
			const std::string &payload = msg->get_payload();
			do_log(LOG_DEBUG, "WS Rx [%s]: %s", chp->codec.c_str(), payload.c_str());

			TargetState ts;
			if (ParseTargetState(payload, ts)) {
				ApplyIfNeeded(*chp, ts);
				return;
			}

			// Protocol: terminate-state ALERT (no action, log only).
			try {
				auto j = nlohmann::json::parse(payload);
				if (j.contains("type") && j["type"] == "ALERT") {
					do_log(LOG_INFO, "ALERT path=%s reason=%s", j.value("path", "").c_str(),
					       j.value("reason", "").c_str());
				}
			} catch (const std::exception &) {
			}
		});

		ch->worker = std::thread([this, chp]() {
			while (running.load()) {
				if (chp->client->stopped())
					chp->client->reset();
				chp->client->run();
				os_sleep_ms(50);

				std::lock_guard<std::mutex> lk(mtx);
				if (ShouldReconnectLocked(*chp))
					ConnectLocked(*chp);
			}
		});

		channels.push_back(std::move(ch));
	}
}

WsDegradeClient::~WsDegradeClient()
{
	running.store(false);

	for (auto &ch : channels) {
		{
			std::lock_guard<std::mutex> lk(mtx);
			if (ch->conn) {
				websocketpp::lib::error_code ec;
				ch->conn->close(websocketpp::close::status::going_away, "shutdown", ec);
				ch->conn.reset();
			}
		}
		ch->client->stop();
	}
	for (auto &ch : channels) {
		if (ch->worker.joinable())
			ch->worker.join();
	}
}

WsDegradeClient::Channel *WsDegradeClient::FindChannel(const std::string &codec)
{
	for (auto &ch : channels) {
		if (ch->codec == codec)
			return ch.get();
	}
	return nullptr;
}

const WsDegradeClient::Channel *WsDegradeClient::FindChannel(const std::string &codec) const
{
	for (auto &ch : channels) {
		if (ch->codec == codec)
			return ch.get();
	}
	return nullptr;
}

int WsDegradeClient::TargetLayers(const std::string &codec) const
{
	std::lock_guard<std::mutex> lk(mtx);
	const Channel *ch = FindChannel(codec);
	if (!ch || !ch->target_set)
		return 0;
	return ch->target.layers;
}

std::vector<obs_encoder_t *> WsDegradeClient::codecEncoders(const std::string &codec) const
{
	return collectCodecEncoders(output, codec);
}

// -------------------------------------------------------------------
//  Connection (re)establishment
// -------------------------------------------------------------------
bool WsDegradeClient::ShouldReconnectLocked(const Channel &ch) const
{
	return !ch.conn && !ch.ws_url.empty() && !ws_secret.empty() && ch.next_reconnect_attempt_ns != 0 &&
	       os_gettime_ns() >= ch.next_reconnect_attempt_ns;
}

void WsDegradeClient::ConnectLocked(Channel &ch)
{
	ch.next_reconnect_attempt_ns = 0;

	do_log(LOG_INFO, "Connecting [%s] to %s", ch.codec.c_str(), ch.ws_url.c_str());

	websocketpp::lib::error_code ec;
	ch.conn = ch.client->get_connection(ch.ws_url, ec);
	if (ec) {
		do_log(LOG_ERROR, "Failed to create connection [%s] %s: %s", ch.codec.c_str(),
		       ch.ws_url.c_str(), ec.message().c_str());
		ch.conn.reset();
		ch.next_reconnect_attempt_ns = os_gettime_ns() + (uint64_t)ch.reconnect_backoff_ms * 1000000ULL;
		ch.reconnect_backoff_ms = std::min(ch.reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
		return;
	}

	ch.conn->append_header("Authorization", "Bearer " + ws_secret);
	ch.client->connect(ch.conn);
}

void WsDegradeClient::ConfigureChannelLocked(Channel &ch, const std::string &whip_url)
{
	if (whip_url.empty()) {
		if (ch.conn) {
			websocketpp::lib::error_code ec;
			ch.conn->close(websocketpp::close::status::going_away, "unused", ec);
			ch.conn.reset();
		}
		ch.ws_url.clear();
		ch.next_reconnect_attempt_ns = 0;
		return;
	}

	std::string new_ws = whip_to_ws(whip_url);
	if (new_ws == ch.ws_url)
		return; // same endpoint: keep the live connection

	ch.ws_url = new_ws;
	if (ch.conn) {
		websocketpp::lib::error_code ec;
		ch.conn->close(websocketpp::close::status::going_away, "url-change", ec);
		ch.conn.reset();
	}
	ch.reconnect_backoff_ms = kReconnectBackoffMinMs;
	ConnectLocked(ch);
}

// -------------------------------------------------------------------
//  Output registration
// -------------------------------------------------------------------
void WsDegradeClient::RegisterOutput(obs_output_t *out, const std::string &h264_whip_url,
				     const std::string &hevc_whip_url)
{
	if (!out)
		return;

	std::lock_guard<std::mutex> lk(mtx);
	output = out;

	obs_service_t *svc = obs_output_get_service(out);
	if (!svc)
		return;

	OBSDataAutoRelease service_settings = obs_service_get_settings(svc);
	std::string secret = obs_data_get_string(service_settings, "ppcenter_secret");
	if (secret.empty()) {
		do_log(LOG_DEBUG, "ppcenter_secret not set, degrade channel disabled");
		return;
	}
	ws_secret = secret;

	// Cache each codec's full layer count once; a degrade-triggered restart
	// re-registers with the same encoders, and re-counting after a trim
	// would make the ceiling shrink.
	for (auto &ch : channels) {
		if (ch->max_layers == 0)
			ch->max_layers = (int)codecEncoders(ch->codec).size();
	}

	if (Channel *h264 = FindChannel("h264"))
		ConfigureChannelLocked(*h264, h264_whip_url);
	if (Channel *hevc = FindChannel("hevc"))
		ConfigureChannelLocked(*hevc, hevc_whip_url);
}

void WsDegradeClient::UnregisterOutput()
{
	std::lock_guard<std::mutex> lk(mtx);
	output = nullptr;

	for (auto &ch : channels) {
		if (ch->conn) {
			websocketpp::lib::error_code ec;
			ch->conn->close(websocketpp::close::status::going_away, "output-unregistered", ec);
			ch->conn.reset();
		}
		ch->ws_url.clear();
		ch->next_reconnect_attempt_ns = 0;
		ch->reconnect_backoff_ms = kReconnectBackoffMinMs;
	}

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
		if (j.contains("codec"))
			out.codec = j["codec"];
		if (j.contains("layers"))
			out.layers = j["layers"];
		if (j.contains("bitrate_percent"))
			out.bitrate_percent = j["bitrate_percent"];
		if (j.contains("latency_ms"))
			out.latency_ms = j["latency_ms"];
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
// A bitrate-only change is applied in place via obs_encoder_update(), which
// per obs-encoder.c is safe on an already-active encoder. A layer-count change
// restarts the output; WHIPOutput::Setup() then reads TargetLayers() and trims
// that codec's simulcast ladder. Layers are never detached from the output, so
// H264 and HEVC can't corrupt each other's slot layout.
void WsDegradeClient::ApplyIfNeeded(Channel &ch, const TargetState &target)
{
	obs_output_t *out_copy = nullptr;
	bool layer_changed = false;
	TargetState prev;
	bool prev_set = false;
	int prev_pct = 100;
	{
		std::lock_guard<std::mutex> lk(mtx);

		if (!output) {
			do_log(LOG_DEBUG, "no output registered, skip");
			return;
		}
		if (!target.codec.empty() && target.codec != ch.codec) {
			do_log(LOG_WARNING, "TARGET_STATE codec=%s on %s channel, ignoring", target.codec.c_str(),
			       ch.codec.c_str());
			return;
		}

		int new_layers = target.layers;
		if (new_layers < 1)
			new_layers = 1;
		int actual_max = ch.max_layers > 0 ? ch.max_layers : (int)codecEncoders(ch.codec).size();
		if (actual_max > 0 && new_layers > actual_max)
			new_layers = actual_max;

		int current = ch.target_set ? ch.target.layers : actual_max;
		if (!ch.target_set)
			layer_changed = (actual_max > 0 && new_layers != actual_max);
		else
			layer_changed = (new_layers != ch.target.layers);

		do_log(LOG_INFO,
		       "TARGET_STATE codec=%s layers=%d (was %d) bitrate=%d%% latency=%dms",
		       ch.codec.c_str(), new_layers, current, target.bitrate_percent, target.latency_ms);

		if (!layer_changed && ch.target_set && ch.last_pct == target.bitrate_percent) {
			ch.target = target;
			ch.target.layers = new_layers;
			return;
		}

		prev = ch.target;
		prev_set = ch.target_set;
		prev_pct = ch.last_pct;

		ch.target = target;
		ch.target.layers = new_layers;
		ch.target_set = true;
		ch.last_pct = target.bitrate_percent;

		// Update encoder settings for this codec's (possibly newly-kept)
		// layers. When a restart follows, the encoders re-read these
		// settings; when it doesn't, the call below makes it live.
		ApplyBitrateLocked(ch, output, target.bitrate_percent, !layer_changed);

		if (!layer_changed) {
			do_log(LOG_INFO, "Applied codec=%s bitrate=%d%% (live, no restart)", ch.codec.c_str(),
			       target.bitrate_percent);
			return;
		}

		out_copy = output;
	}

	// --- Stop first (outside the mutex: Stop() re-enters this class via
	//     UnregisterOutput(), which also takes mtx) ---
	do_log(LOG_INFO, "Stopping output to apply codec=%s layers=%d", ch.codec.c_str(), ch.target.layers);
	obs_output_stop(out_copy);

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
		// Roll back the recorded target: it was updated optimistically, and
		// leaving it claiming we reached the target would make the
		// idempotency check swallow the identical retry this message waits for.
		std::lock_guard<std::mutex> lk(mtx);
		ch.target = prev;
		ch.target_set = prev_set;
		ch.last_pct = prev_pct;
		return;
	}

	do_log(LOG_INFO, "Starting output (codec=%s layers=%d)", ch.codec.c_str(), ch.target.layers);
	obs_output_start(out_copy);
}

void WsDegradeClient::ApplyBitrateLocked(Channel &ch, obs_output_t *out, int bitrate_percent, bool live)
{
	auto encoders = codecEncoders(ch.codec);
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

		// Under VBR the ceiling has to come down with the target, or a
		// degrade only moves the floor: the server has just said the link
		// can't carry the current rate, while the encoder stays free to
		// burst to the original peak on complex content - exactly the wrong
		// moment. Cached and scaled the same way as the target above, so the
		// peak/target ratio the user configured survives repeated cycles.
		// max_bitrate only means anything in VBR (NVENC/QSV), so this is a
		// no-op under CBR; x264 reads 0 here and skips the block.
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
