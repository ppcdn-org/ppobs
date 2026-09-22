#include "ppcenter-signal.h"
#include "ppcenter-signal-message.h"
#include "ppcenter-signal-outgoing.h"
#include "ppcenter-websocket-utils.h"

#include <rtc/websocket.hpp>

#include <obs.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
const int MAX_VIDEO_FRAGMENT_SIZE = 1200;
const int VIDEO_NACK_BUFFER_SIZE = 512;
const std::string VIDEO_MID = "0";
const std::string AUDIO_MID = "1";
} // namespace

class P2PSignalImpl {
public:
	P2PSignalImpl(const std::string &url, const std::string &token)
		: signalUrl(url), signalToken(token)
	{
		validEndpoint = ParseWebSocketSignalURL(signalUrl).valid;
	}

	~P2PSignalImpl() { stop(); }

	bool start(std::function<void(const std::string &)> onMessage)
	{
		if (!validEndpoint) {
			blog(LOG_ERROR, "[ppobs P2P] refusing invalid signaling URL; only ws:// and wss:// are supported");
			return false;
		}
		if (!IsAllowedWebSocketSignalURL(signalUrl)) {
			blog(LOG_ERROR,
			     "[ppobs P2P] refusing insecure remote signaling URL; use wss:// (ws:// is allowed only for loopback development)");
			return false;
		}
		this->onMessage = std::move(onMessage);
		running = true;
		thread = std::thread(&P2PSignalImpl::run, this);
		return true;
	}

	void stop()
	{
		if (!running.exchange(false)) return;
		std::shared_ptr<rtc::WebSocket> sock;
		{
			std::lock_guard<std::mutex> lock(wsMutex);
			sock = ws;
			ws.reset();
			wsConnected = false;
		}
		wsCv.notify_all();
		if (sock) {
			try {
				sock->close();
			} catch (...) {
			}
		}
		if (thread.joinable()) thread.join();
	}

	bool wsSend(const std::string &data)
	{
		std::lock_guard<std::mutex> lock(wsMutex);
		return ws && ws->isOpen() && ws->send(data);
	}

private:
	void run()
	{
		while (running) {
			if (!connectWS()) {
				blog(LOG_WARNING, "[ppobs P2P] signal connect failed; retrying in 5s");
				if (!running) return;
				waitFor(std::chrono::seconds(5));
				continue;
			}
			blog(LOG_INFO, "[ppobs P2P] signaling connected");
			{
				std::unique_lock<std::mutex> lock(wsMutex);
				wsCv.wait(lock, [this] { return !running.load() || !wsConnected; });
			}
			if (!running) return;
			blog(LOG_WARNING, "[ppobs P2P] signal connection closed; reconnecting");
			std::shared_ptr<rtc::WebSocket> sock;
			{
				std::lock_guard<std::mutex> lock(wsMutex);
				sock = ws;
				ws.reset();
				wsConnected = false;
			}
			if (sock) {
				try {
					sock->close();
				} catch (...) {
				}
			}
			waitFor(std::chrono::seconds(2));
		}
	}

	void waitFor(std::chrono::milliseconds delay)
	{
		std::unique_lock<std::mutex> lock(wsMutex);
		wsCv.wait_for(lock, delay, [this] { return !running.load(); });
	}

	bool connectWS()
	{
		auto sock = std::make_shared<rtc::WebSocket>(makeWebSocketConfig());
		auto opened = std::make_shared<std::promise<bool>>();
		auto openedFuture = opened->get_future();

		sock->onOpen([this, opened]() {
			{
				std::lock_guard<std::mutex> lock(wsMutex);
				wsConnected = true;
			}
			try {
				opened->set_value(true);
			} catch (...) {
			}
			wsCv.notify_all();
		});
		sock->onError([this, opened](rtc::string err) {
			blog(LOG_WARNING, "[ppobs P2P] signal error: %s", err.c_str());
			try {
				opened->set_value(false);
			} catch (...) {
			}
		});
		sock->onClosed([this, opened]() {
			{
				std::lock_guard<std::mutex> lock(wsMutex);
				wsConnected = false;
			}
			// Resolve a still-pending handshake so connectWS() (and therefore
			// stop()'s join) does not block for the full 10s timeout when the
			// socket is closed without an error.
			try {
				opened->set_value(false);
			} catch (...) {
			}
			wsCv.notify_all();
		});
		sock->onMessage([](rtc::binary) {}, [this](rtc::string text) {
			if (onMessage) onMessage(text);
		});

		{
			std::lock_guard<std::mutex> lock(wsMutex);
			ws = sock;
		}
		try {
			sock->open(signalUrl);
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[ppobs P2P] signal open failed: %s", e.what());
			std::lock_guard<std::mutex> lock(wsMutex);
			if (ws == sock) ws.reset();
			return false;
		}
		if (openedFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
			return false;
		}
		return openedFuture.get();
	}

	rtc::WebSocketConfiguration makeWebSocketConfig() const
	{
		rtc::WebSocketConfiguration config;
		// ppcenter reads the P2P token from the Sec-WebSocket-Protocol header
		// (ppcdn-token.<token>) and echoes back ppcdn-p2p-v1. libdatachannel
		// sends/validates exactly these subprotocols, so no server change is
		// needed.
		config.protocols = {"ppcdn-p2p-v1", "ppcdn-token." + signalToken};
		// Explicitly disabled ("zero to disable", rtc/configuration.hpp), not
		// left unset. A prior non-zero value here (15000ms) reproduced a
		// signaling reconnect every ~15.0-15.05s for the entire life of a
		// stream (confirmed identically in this plugin's own log output and
		// in ppcenter's access log - GET /v1/p2p/signal living exactly
		// ~15.03-15.05s per connection) with nginx (proxy_read_timeout 3600s
		// on this route) and the server's own ping/pong (30s ping, 90s read
		// deadline) both ruled out as the cause - the period tracked this
		// setting too precisely to be anything else. ppcenter already pings
		// every 30s server->client with a 90s read deadline
		// (ws/p2p_signal.go), so there is nothing this client-side ping was
		// adding except the outage: every reconnect makes the coordinator
		// drop every one of this publisher's active P2P sessions
		// (Attach()->releasePublisherLocked in coordinator.go).
		config.pingInterval = std::chrono::milliseconds(0);
		config.connectionTimeout = std::chrono::milliseconds(10000);
		return config;
	}

	std::string signalUrl;
	std::string signalToken;
	bool validEndpoint = false;
	std::thread thread;
	std::atomic<bool> running{false};
	std::function<void(const std::string &)> onMessage;

	std::shared_ptr<rtc::WebSocket> ws;
	std::mutex wsMutex;
	std::condition_variable wsCv;
	bool wsConnected = false;
};

P2PSignalClient::P2PSignalClient(const std::string &url, const std::string &token, const std::string &streamPath,
				 const std::string &videoCodec, const std::string &audioCodec, uint32_t baseSsrc,
				 std::vector<std::string> stunServers)
	: impl(std::make_unique<P2PSignalImpl>(url, token)),
	  streamPath(streamPath), videoCodec(videoCodec), audioCodec(audioCodec), baseSsrc(baseSsrc),
	  stunServers(std::move(stunServers))
{
}

P2PSignalClient::~P2PSignalClient() { Stop(); }

void P2PSignalClient::SetOnPeerUpdated(std::function<void()> callback) { onPeerUpdated = std::move(callback); }

bool P2PSignalClient::Start()
{
	if (!impl->start([this](const std::string &data) { OnMessage(data); }))
		return false;
	running = true;
	return true;
}

void P2PSignalClient::Stop()
{
	if (!running.exchange(false)) return;
	impl->stop();
	{ std::lock_guard<std::mutex> lock(peersMutex); peers.clear(); }
}

void P2PSignalClient::OnMessage(const std::string &data)
{
	auto msg = ParseP2PSignalMessage(data);
	if (!msg.valid) return;
	if (msg.type == "allocated") HandleAllocated(msg.sessionId);
	else if (msg.type == "offer") HandleOffer(msg.sessionId, msg.sdp);
	else if (msg.type == "ice") HandleIce(msg.sessionId, msg.candidate, msg.sdpMid);
	else if (msg.type == "close" || msg.type == "error") HandleClose(msg.sessionId);
}

void P2PSignalClient::HandleAllocated(const std::string &sessionId)
{
	std::lock_guard<std::mutex> lock(peersMutex);
	if (peers.size() >= 3 || peers.count(sessionId)) return;
	auto peer = std::make_shared<P2PPeer>();
	peer->sessionId = sessionId;
	peer->videoSsrc = baseSsrc + 1 + (uint32_t)(peers.size() * 2);
	peer->audioSsrc = baseSsrc + 2 + (uint32_t)(peers.size() * 2);
	peers[sessionId] = peer;
	if (onPeerUpdated) onPeerUpdated();
}

void P2PSignalClient::HandleOffer(const std::string &sessionId, const std::string &sdp)
{
	std::shared_ptr<P2PPeer> peer;
	{ std::lock_guard<std::mutex> lock(peersMutex); auto it = peers.find(sessionId); if (it == peers.end()) return; peer = it->second; }

	rtc::Configuration cfg;
	for (const auto &server : stunServers) {
		try {
			cfg.iceServers.emplace_back(server);
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[ppobs P2P] ignoring invalid STUN server '%s': %s", server.c_str(), e.what());
		}
	}
	peer->pc = std::make_shared<rtc::PeerConnection>(cfg);
	CreatePeerVideo(*peer); CreatePeerAudio(*peer);

	peer->pc->onStateChange([this, sessionId](rtc::PeerConnection::State state) {
		if (state == rtc::PeerConnection::State::Connected) SendConnected(sessionId);
		else if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Disconnected)
			HandleClose(sessionId);
	});

	peer->pc->onLocalCandidate([this, sessionId](const rtc::Candidate &candidate) {
		SendIce(sessionId, candidate.candidate(), candidate.mid(), 0);
	});

	try {
		peer->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
		peer->pc->gatherLocalCandidates();
		auto answer = peer->pc->localDescription().value_or(rtc::Description("", "answer"));
		SendAnswer(sessionId, std::string(answer));
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[ppobs P2P] handle offer failed: %s", e.what());
		HandleClose(sessionId);
	}
}

void P2PSignalClient::HandleClose(const std::string &sessionId) { RemovePeer(sessionId); }
void P2PSignalClient::HandleIce(const std::string &sessionId, const std::string &candidate, const std::string &sdpMid)
{
	if (candidate.empty()) return;
	std::shared_ptr<P2PPeer> peer;
	{ std::lock_guard<std::mutex> lock(peersMutex); auto it = peers.find(sessionId); if (it == peers.end()) return; peer = it->second; }
	if (!peer->pc) return;
	try {
		peer->pc->addRemoteCandidate(rtc::Candidate(candidate, sdpMid));
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[ppobs P2P] add remote ICE candidate failed: %s", e.what());
	}
}
void P2PSignalClient::RemovePeer(const std::string &sessionId) { { std::lock_guard<std::mutex> lock(peersMutex); peers.erase(sessionId); } if (onPeerUpdated) onPeerUpdated(); }

void P2PSignalClient::CreatePeerVideo(P2PPeer &peer)
{
	uint32_t ssrc = peer.videoSsrc;
	std::string cname = "p2p-video-" + peer.sessionId, msid = "p2p-" + peer.sessionId, trackId = msid + "-video";
	auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, 96,
#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR > 22 || RTC_VERSION_MAJOR > 0
									 rtc::H264RtpPacketizer::ClockRate);
#else
									 rtc::H264RtpPacketizer::defaultClockRate);
#endif
	if (videoCodec == "h264") {
		rtc::Description::Video desc(VIDEO_MID, rtc::Description::Direction::SendOnly);
		desc.addH264Codec(96); desc.addSSRC(ssrc, cname, msid, trackId);
		peer.videoTrack = peer.pc->addTrack(desc);
		auto pk = std::make_shared<rtc::H264RtpPacketizer>(rtc::H264RtpPacketizer::Separator::StartSequence, rtpConfig, MAX_VIDEO_FRAGMENT_SIZE);
		peer.videoSrReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
		pk->addToChain(peer.videoSrReporter); pk->addToChain(std::make_shared<rtc::RtcpNackResponder>(VIDEO_NACK_BUFFER_SIZE));
		peer.videoTrack->setMediaHandler(pk);
	}
}

void P2PSignalClient::CreatePeerAudio(P2PPeer &peer)
{
	uint32_t ssrc = peer.audioSsrc;
	std::string cname = "p2p-audio-" + peer.sessionId, msid = "p2p-" + peer.sessionId, trackId = msid + "-audio";
	if (audioCodec == "opus") {
		rtc::Description::Audio desc(AUDIO_MID, rtc::Description::Direction::SendOnly);
		desc.addOpusCodec(97); desc.addSSRC(ssrc, cname, msid, trackId);
		peer.audioTrack = peer.pc->addTrack(desc);
		auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, 97, rtc::OpusRtpPacketizer::DefaultClockRate);
		auto pk = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
		peer.audioSrReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
		pk->addToChain(peer.audioSrReporter); pk->addToChain(std::make_shared<rtc::RtcpNackResponder>());
		peer.audioTrack->setMediaHandler(pk);
	}
}

void P2PSignalClient::SendAnswer(const std::string &sessionId, const std::string &sdp)
{
	impl->wsSend(BuildP2PAnswerJSON(sessionId, sdp));
}

void P2PSignalClient::SendIce(const std::string &sessionId, const std::string &candidate, const std::string &sdpMid, int sdpMLineIndex)
{
	impl->wsSend(BuildP2PIceJSON(sessionId, candidate, sdpMid, sdpMLineIndex));
}

void P2PSignalClient::SendConnected(const std::string &sessionId)
{
	impl->wsSend(BuildP2PConnectedJSON(sessionId));
	{ std::lock_guard<std::mutex> lock(peersMutex); auto it = peers.find(sessionId); if (it != peers.end()) it->second->connected = true; }
	if (onPeerUpdated) onPeerUpdated();
}

void P2PSignalClient::SendClose(const std::string &sessionId, const std::string &reason)
{
	impl->wsSend(BuildP2PCloseJSON(sessionId, reason));
}

size_t P2PSignalClient::PeerCount() { std::lock_guard<std::mutex> lock(peersMutex); return peers.size(); }
void P2PSignalClient::ForEachPeer(std::function<void(P2PPeer &)> callback)
{
	std::vector<std::shared_ptr<P2PPeer>> snapshot;
	{
		std::lock_guard<std::mutex> lock(peersMutex);
		for (auto &pair : peers) snapshot.push_back(pair.second);
	}
	for (auto &peer : snapshot) callback(*peer);
}
