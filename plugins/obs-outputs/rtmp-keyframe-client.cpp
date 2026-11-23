#include "rtmp-keyframe-client.h"

#include <obs.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define ASIO_STANDALONE 1
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#define do_log(level, fmt, ...) blog(level, "[rtmp-keyframe-client] " fmt, ##__VA_ARGS__)

namespace {

constexpr int kReconnectBackoffMinMs = 2000;
constexpr int kReconnectBackoffMaxMs = 30000;

using steady_clock = std::chrono::steady_clock;

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
	return layers;
}

void RequestKeyFrames(obs_output_t *output, const char *codec)
{
	auto encoders = collectEncoders(output);
	for (auto *enc : encoders)
		obs_encoder_request_keyframe(enc);
	do_log(LOG_INFO, "Keyframe requested [%s] (%zu layer(s))", codec && *codec ? codec : "rtmp",
	       encoders.size());
}

struct Channel {
	using client_t = websocketpp::client<websocketpp::config::asio_tls_client>;
	using handle_t = websocketpp::connection_hdl;
	using conn_ptr = client_t::connection_ptr;
	using context_ptr = websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>;

	obs_output_t *output = nullptr;
	std::string codec;
	std::string ws_url;
	std::string token;

	std::unique_ptr<client_t> client;
	conn_ptr conn;
	std::thread worker;
	std::atomic<bool> running{true};

	steady_clock::time_point next_reconnect_at{};
	int reconnect_backoff_ms = kReconnectBackoffMinMs;
};

class RtmpKeyframeRegistry {
public:
	static RtmpKeyframeRegistry &Instance()
	{
		static RtmpKeyframeRegistry instance;
		return instance;
	}

	void Register(obs_output_t *output, const char *codec, const std::string &ws_url,
		      const std::string &token)
	{
		if (!output)
			return;

		std::lock_guard<std::mutex> lk(mtx);
		auto it = channels.find(output);
		Channel *chp = nullptr;
		if (it == channels.end()) {
			auto ch = std::make_unique<Channel>();
			ch->output = output;
			chp = ch.get();
			InitChannel(*chp);
			channels.emplace(output, std::move(ch));
			StartWorker(chp);
		} else {
			chp = it->second.get();
		}

		if (codec)
			chp->codec = codec;
		ConfigureChannelLocked(*chp, ws_url, token);
	}

	void Unregister(obs_output_t *output)
	{
		std::unique_ptr<Channel> channel;
		{
			std::lock_guard<std::mutex> lk(mtx);
			auto it = channels.find(output);
			if (it == channels.end())
				return;
			channel = std::move(it->second);
			channels.erase(it);
			channel->running = false;
			channel->ws_url.clear();
			channel->token.clear();
			if (channel->conn) {
				websocketpp::lib::error_code ec;
				channel->conn->close(websocketpp::close::status::going_away, "output-stopped", ec);
				channel->conn.reset();
			}
		}
		if (channel->worker.joinable())
			channel->worker.join();
	}

private:
	void InitChannel(Channel &ch)
	{
		ch.client = std::make_unique<Channel::client_t>();
		auto &c = *ch.client;
		c.clear_access_channels(websocketpp::log::alevel::all);
		c.clear_error_channels(websocketpp::log::elevel::all);
		c.init_asio();
		c.set_tls_init_handler([](Channel::handle_t) -> Channel::context_ptr {
			auto context = websocketpp::lib::make_shared<websocketpp::lib::asio::ssl::context>(
				websocketpp::lib::asio::ssl::context::tls_client);
			context->set_options(websocketpp::lib::asio::ssl::context::default_workarounds |
					     websocketpp::lib::asio::ssl::context::no_sslv2 |
					     websocketpp::lib::asio::ssl::context::no_sslv3);
			context->set_verify_mode(websocketpp::lib::asio::ssl::verify_none);
			return context;
		});

		c.set_open_handler([this, &ch](Channel::handle_t) {
			std::lock_guard<std::mutex> lk(mtx);
			ch.reconnect_backoff_ms = kReconnectBackoffMinMs;
			do_log(LOG_INFO, "WS connected [%s] to %s", ch.codec.c_str(), ch.ws_url.c_str());
		});

		c.set_close_handler([this, &ch](Channel::handle_t) {
			std::lock_guard<std::mutex> lk(mtx);
			ch.conn.reset();
			if (!ch.ws_url.empty() && !ch.token.empty()) {
				ch.next_reconnect_at = steady_clock::now() +
						       std::chrono::milliseconds(ch.reconnect_backoff_ms);
				ch.reconnect_backoff_ms =
					std::min(ch.reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
			}
		});

		c.set_fail_handler([this, &ch](Channel::handle_t) {
			std::lock_guard<std::mutex> lk(mtx);
			ch.conn.reset();
			if (!ch.ws_url.empty() && !ch.token.empty()) {
				ch.next_reconnect_at = steady_clock::now() +
						       std::chrono::milliseconds(ch.reconnect_backoff_ms);
				ch.reconnect_backoff_ms =
					std::min(ch.reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
			}
		});

		c.set_message_handler([this, &ch](Channel::handle_t, Channel::client_t::message_ptr msg) {
			try {
				auto j = nlohmann::json::parse(msg->get_payload());
				if (j.value("type", "") == "REQUEST_KEYFRAME")
					RequestKeyFrames(ch.output, ch.codec.c_str());
			} catch (const std::exception &) {
			}
		});
	}

	void StartWorker(Channel *ch)
	{
		ch->worker = std::thread([this, ch]() {
			while (ch->running.load()) {
				if (ch->client->stopped())
					ch->client->reset();
				ch->client->run();
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				if (!ch->running.load())
					break;

				std::lock_guard<std::mutex> lk(mtx);
				if (ShouldReconnectLocked(*ch))
					ConnectLocked(*ch);
			}
		});
	}

	bool ShouldReconnectLocked(const Channel &ch) const
	{
		return !ch.conn && !ch.ws_url.empty() && !ch.token.empty() &&
		       ch.next_reconnect_at != steady_clock::time_point{} &&
		       steady_clock::now() >= ch.next_reconnect_at;
	}

	void ConnectLocked(Channel &ch)
	{
		ch.next_reconnect_at = steady_clock::time_point{};
		do_log(LOG_INFO, "Connecting [%s] to %s", ch.codec.c_str(), ch.ws_url.c_str());

		websocketpp::lib::error_code ec;
		ch.conn = ch.client->get_connection(ch.ws_url, ec);
		if (ec) {
			do_log(LOG_ERROR, "Failed to create connection [%s] %s: %s", ch.codec.c_str(),
			       ch.ws_url.c_str(), ec.message().c_str());
			ch.conn.reset();
			ch.next_reconnect_at = steady_clock::now() +
					       std::chrono::milliseconds(ch.reconnect_backoff_ms);
			ch.reconnect_backoff_ms =
				std::min(ch.reconnect_backoff_ms * 2, kReconnectBackoffMaxMs);
			return;
		}

		ch.conn->append_header("Authorization", "Bearer " + ch.token);
		ch.client->connect(ch.conn);
	}

	void ConfigureChannelLocked(Channel &ch, const std::string &ws_url, const std::string &token)
	{
		if (ws_url.empty() || token.empty()) {
			if (ch.conn) {
				websocketpp::lib::error_code ec;
				ch.conn->close(websocketpp::close::status::going_away, "unused", ec);
				ch.conn.reset();
			}
			ch.ws_url.clear();
			ch.token.clear();
			ch.next_reconnect_at = steady_clock::time_point{};
			return;
		}

		if (ws_url == ch.ws_url && token == ch.token)
			return;

		if (ch.conn) {
			websocketpp::lib::error_code ec;
			ch.conn->close(websocketpp::close::status::going_away, "credentials-changed", ec);
			ch.conn.reset();
		}
		ch.ws_url = ws_url;
		ch.token = token;
		ch.reconnect_backoff_ms = kReconnectBackoffMinMs;
		ch.next_reconnect_at = steady_clock::now();
	}

	std::mutex mtx;
	std::unordered_map<obs_output_t *, std::unique_ptr<Channel>> channels;
};

} // namespace

extern "C" void rtmp_keyframe_register(obs_output_t *output, const char *ws_url, const char *token)
{
	const char *codec = "rtmp";
	obs_encoder_t *venc = obs_output_get_video_encoder(output);
	if (venc) {
		const char *c = obs_encoder_get_codec(venc);
		if (c && *c)
			codec = c;
	}
	RtmpKeyframeRegistry::Instance().Register(output, codec, ws_url ? ws_url : "",
						 token ? token : "");
}

extern "C" void rtmp_keyframe_unregister(obs_output_t *output)
{
	RtmpKeyframeRegistry::Instance().Unregister(output);
}
