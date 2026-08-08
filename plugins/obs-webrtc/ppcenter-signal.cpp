#include "ppcenter-signal.h"
#include "ppcenter-signal-message.h"
#include "ppcenter-signal-outgoing.h"
#include "ppcenter-websocket-frame.h"
#include "ppcenter-websocket-utils.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#undef send
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define SOCK_ERR SOCKET_ERROR
#define SOCK_CLOSE(s) closesocket(s)
#define SOCK_RECV(s,b,l) recv(s,b,l,0)
#define SOCK_SEND(s,b,l) send(s,b,l,0)
#else
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define SOCK_ERR (-1)
#define SOCK_CLOSE(s) close(s)
#define SOCK_RECV(s,b,l) (int)recv(s,b,l,0)
#define SOCK_SEND(s,b,l) (int)send(s,b,l,0)
#endif

#include <obs.h>
#include <curl/curl.h>

#include <sstream>
#include <cstring>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace {
const int MAX_VIDEO_FRAGMENT_SIZE = 1200;
const int VIDEO_NACK_BUFFER_SIZE = 512;
const size_t MAX_WS_HANDSHAKE_BYTES = 16384;
const size_t MAX_WS_PAYLOAD_BYTES = 1024 * 1024;
const int WS_SOCKET_TIMEOUT_MS = 5000;
const std::string VIDEO_MID = "0";
const std::string AUDIO_MID = "1";

std::vector<unsigned char> RandomBytes(size_t count)
{
	std::vector<unsigned char> bytes(count);
	std::random_device rd;
	for (auto &byte : bytes)
		byte = static_cast<unsigned char>(rd());
	return bytes;
}

std::string Base64Encode(const unsigned char *data, size_t len)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		const uint32_t b0 = data[i];
		const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
		const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
		const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
		out.push_back(table[(triple >> 18) & 0x3F]);
		out.push_back(table[(triple >> 12) & 0x3F]);
		out.push_back((i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=');
		out.push_back((i + 2 < len) ? table[triple & 0x3F] : '=');
	}
	return out;
}

bool SetSocketTimeouts(sock_t socket)
{
#ifdef _WIN32
	DWORD timeoutMs = WS_SOCKET_TIMEOUT_MS;
	return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs)) != SOCK_ERR &&
	       setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&timeoutMs), sizeof(timeoutMs)) != SOCK_ERR;
#else
	timeval timeout{};
	timeout.tv_sec = WS_SOCKET_TIMEOUT_MS / 1000;
	timeout.tv_usec = (WS_SOCKET_TIMEOUT_MS % 1000) * 1000;
	return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != SOCK_ERR &&
	       setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != SOCK_ERR;
#endif
}

bool SetSocketBlocking(sock_t socket, bool blocking)
{
#ifdef _WIN32
	u_long mode = blocking ? 0 : 1;
	return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(socket, F_GETFL, 0);
	if (flags < 0)
		return false;
	if (blocking)
		flags &= ~O_NONBLOCK;
	else
		flags |= O_NONBLOCK;
	return fcntl(socket, F_SETFL, flags) == 0;
#endif
}

bool ConnectWithTimeout(sock_t socket, const sockaddr *addr, socklen_t addrLen)
{
	if (!SetSocketBlocking(socket, false))
		return false;

	int rc = ::connect(socket, addr, (int)addrLen);
	if (rc == SOCK_ERR) {
#ifdef _WIN32
		const int err = WSAGetLastError();
		if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL)
			return false;
#else
		if (errno != EINPROGRESS)
			return false;
#endif
	}

	fd_set writeSet;
	FD_ZERO(&writeSet);
	FD_SET(socket, &writeSet);
	timeval timeout{};
	timeout.tv_sec = WS_SOCKET_TIMEOUT_MS / 1000;
	timeout.tv_usec = (WS_SOCKET_TIMEOUT_MS % 1000) * 1000;
	const int ready = select((int)socket + 1, nullptr, &writeSet, nullptr, &timeout);
	if (ready <= 0 || !FD_ISSET(socket, &writeSet))
		return false;

	int socketError = 0;
#ifdef _WIN32
	int optLen = sizeof(socketError);
#else
	socklen_t optLen = sizeof(socketError);
#endif
	if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&socketError), &optLen) == SOCK_ERR)
		return false;
	if (socketError != 0)
		return false;

	return SetSocketBlocking(socket, true);
}

} // namespace

class P2PSignalImpl {
public:
	P2PSignalImpl(const std::string &url, const std::string &token)
		: signalUrl(url), signalToken(token)
	{
		auto parsed = ParseWebSocketSignalURL(signalUrl);
		validEndpoint = parsed.valid;
		tls = parsed.tls;
		host = std::move(parsed.host);
		path = std::move(parsed.path);
	}

	~P2PSignalImpl() { stop(); }

	bool start(std::function<void(const std::string &)> onMessage)
	{
		if (!validEndpoint) {
			blog(LOG_ERROR, "[ppobs P2P] refusing invalid signaling URL; only ws:// and wss:// are supported");
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
		{
			std::lock_guard<std::mutex> lock(sendMutex);
			closeTransport();
			sock = SOCK_INVALID;
		}
		if (thread.joinable()) thread.join();
	}

	bool wsSend(const std::string &data)
	{
		std::lock_guard<std::mutex> lock(sendMutex);
		if (!isTransportConnected()) return false;
		return sendMaskedFrame(0x01, data);
	}

private:
	bool transportSend(const char *data, size_t length)
	{
		size_t sent = 0;
		while (sent < length) {
			size_t n = 0;
			if (curl) {
				CURLcode rc = curl_easy_send(curl, data + sent, length - sent, &n);
				if (rc != CURLE_OK || n == 0) return false;
			} else {
				const int rc = SOCK_SEND(sock, data + sent, (int)(length - sent));
				if (rc <= 0) return false;
				n = static_cast<size_t>(rc);
			}
			sent += n;
		}
		return true;
	}

	int transportRecv(char *buffer, size_t length)
	{
		if (curl) {
			size_t n = 0;
			CURLcode rc = curl_easy_recv(curl, buffer, length, &n);
			if (rc != CURLE_OK || n == 0) return -1;
			return static_cast<int>(n);
		}
		return SOCK_RECV(sock, buffer, (int)length);
	}

	bool isTransportConnected() const { return curl != nullptr || sock != SOCK_INVALID; }

	void closeTransport()
	{
		if (curl) {
			curl_easy_cleanup(curl);
			curl = nullptr;
		}
		if (sock != SOCK_INVALID) {
			SOCK_CLOSE(sock);
			sock = SOCK_INVALID;
		}
	}

	bool sendMaskedFrame(unsigned char opcode, const std::string &data)
	{
		auto maskKey = RandomBytes(4);
		std::array<unsigned char, 4> mask = {maskKey[0], maskKey[1], maskKey[2], maskKey[3]};
		auto frame = BuildClientWebSocketFrame(opcode, data, mask);
		return transportSend(frame.data(), frame.size());
	}

	bool readExact(char *buffer, size_t length)
	{
		size_t copied = 0;
		if (!pendingRead.empty()) {
			copied = std::min(length, pendingRead.size());
			memcpy(buffer, pendingRead.data(), copied);
			pendingRead.erase(pendingRead.begin(), pendingRead.begin() + copied);
		}
		while (copied < length) {
			const int n = transportRecv(buffer + copied, length - copied);
			if (n <= 0)
				return false;
			copied += n;
		}
		return true;
	}

	void run()
	{
		while (running) {
			if (!connectWS()) { if (!running) return; std::this_thread::sleep_for(std::chrono::seconds(5)); continue; }
			blog(LOG_INFO, "[ppobs P2P] signaling connected");
			processMessages();
			{
				std::lock_guard<std::mutex> lock(sendMutex);
				closeTransport();
			}
			if (!running) return;
			std::this_thread::sleep_for(std::chrono::seconds(2));
		}
	}

	bool connectWS()
	{
		pendingRead.clear();
		if (tls) return connectWSS();
		struct addrinfo hints = {}, *result = nullptr;
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		auto portPos = host.find(':');
		std::string h = host;
		std::string port = "80";
		if (portPos != std::string::npos) { h = host.substr(0, portPos); port = host.substr(portPos + 1); }

		if (getaddrinfo(h.c_str(), port.c_str(), &hints, &result) != 0) return false;

		sock = SOCK_INVALID;
		for (auto rp = result; rp; rp = rp->ai_next) {
			sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (sock == SOCK_INVALID) continue;
			if (!SetSocketTimeouts(sock)) { SOCK_CLOSE(sock); sock = SOCK_INVALID; continue; }
			if (ConnectWithTimeout(sock, rp->ai_addr, (socklen_t)rp->ai_addrlen)) break;
			SOCK_CLOSE(sock);
			sock = SOCK_INVALID;
		}
		freeaddrinfo(result);
		if (sock == SOCK_INVALID) return false;

		return completeWebSocketHandshake();
	}

	bool connectWSS()
	{
		curl = curl_easy_init();
		if (!curl) return false;
		std::string httpsUrl = "https://" + host + path;
		char curlError[CURL_ERROR_SIZE] = {};
		curl_easy_setopt(curl, CURLOPT_URL, httpsUrl.c_str());
		curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, WS_SOCKET_TIMEOUT_MS / 1000L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, WS_SOCKET_TIMEOUT_MS / 1000L);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
		CURLcode rc = curl_easy_perform(curl);
		if (rc != CURLE_OK) {
			blog(LOG_WARNING, "[ppobs P2P] wss TLS connect failed: %s", curlError[0] ? curlError : curl_easy_strerror(rc));
			closeTransport();
			return false;
		}
		return completeWebSocketHandshake();
	}

	bool completeWebSocketHandshake()
	{
		auto nonce = RandomBytes(16);
		const auto websocketKey = Base64Encode(nonce.data(), nonce.size());

		std::ostringstream wsReq;
		wsReq << "GET " << path << " HTTP/1.1\r\n"
		      << "Host: " << host << "\r\n"
		      << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
		      << "Sec-WebSocket-Key: " << websocketKey << "\r\n"
		      << "Sec-WebSocket-Version: 13\r\n"
		      << "Sec-WebSocket-Protocol: ppcdn-p2p-v1, ppcdn-token." << signalToken << "\r\n\r\n";
		std::string reqStr = wsReq.str();

		if (!transportSend(reqStr.data(), reqStr.size())) { closeTransport(); return false; }

		std::string response;
		while (response.find("\r\n\r\n") == std::string::npos) {
			char buf[1024];
			const int n = transportRecv(buf, sizeof(buf));
			if (n <= 0) { closeTransport(); return false; }
			response.append(buf, n);
			if (response.size() > MAX_WS_HANDSHAKE_BYTES) { closeTransport(); return false; }
		}
		const auto headerEnd = response.find("\r\n\r\n") + 4;
		pendingRead.assign(response.begin() + headerEnd, response.end());
		response.resize(headerEnd);
		if (!WebSocketHandshakeAccepted(response) || !HasP2PSubprotocol(response) || !HasExpectedAccept(response, websocketKey)) {
			closeTransport();
			pendingRead.clear();
			return false;
		}
		return true;
	}

	void processMessages()
	{
		while (running && isTransportConnected()) {
			unsigned char header[2];
			if (!readExact((char *)header, 2)) return;

			bool fin = (header[0] & 0x80) != 0;
			int opcode = header[0] & 0x0F;
			bool masked = (header[1] & 0x80) != 0;
			size_t payloadLen = header[1] & 0x7F;
			if (masked) {
				blog(LOG_WARNING, "[ppobs P2P] refusing masked server WebSocket frame");
				return;
			}
			if (opcode == 0x00 || (opcode == 0x01 && !fin)) {
				blog(LOG_WARNING, "[ppobs P2P] refusing fragmented signaling frame");
				return;
			}
			if (opcode >= 0x08 && !fin) {
				blog(LOG_WARNING, "[ppobs P2P] refusing fragmented WebSocket control frame");
				return;
			}
			if ((opcode >= 0x03 && opcode <= 0x07) || opcode > 0x0A) {
				blog(LOG_WARNING, "[ppobs P2P] refusing unsupported WebSocket opcode: %d", opcode);
				return;
			}

			if (payloadLen == 126) { unsigned char ext[2]; if (!readExact((char *)ext, 2)) return; payloadLen = ((size_t)ext[0] << 8) | ext[1]; }
			else if (payloadLen == 127) { unsigned char ext[8]; if (!readExact((char *)ext, 8)) return; payloadLen = 0; for (int i = 0; i < 8; i++) payloadLen = (payloadLen << 8) | ext[i]; }
			if (opcode >= 0x08 && payloadLen > 125) {
				blog(LOG_WARNING, "[ppobs P2P] refusing oversized WebSocket control frame");
				return;
			}
			if (payloadLen > MAX_WS_PAYLOAD_BYTES) {
				blog(LOG_WARNING, "[ppobs P2P] signaling frame too large: %zu bytes", payloadLen);
				return;
			}

			std::vector<char> payload(payloadLen);
			if (payloadLen > 0 && !readExact(payload.data(), payloadLen)) return;

			if (opcode == 0x08) { closeTransport(); return; }
			if (opcode == 0x09) {
				std::lock_guard<std::mutex> lock(sendMutex);
				sendMaskedFrame(0x0A, std::string(payload.data(), payloadLen));
				continue;
			}
			if (opcode == 0x01) {
				std::string msg(payload.data(), payloadLen);
				if (onMessage) onMessage(msg);
			}
		}
	}

	std::string signalUrl;
	std::string signalToken;
	std::string host;
	std::string path;
	bool validEndpoint = false;
	bool tls = false;
	std::vector<char> pendingRead;
	sock_t sock = SOCK_INVALID;
	CURL *curl = nullptr;
	std::mutex sendMutex;
	std::thread thread;
	std::atomic<bool> running{false};
	std::function<void(const std::string &)> onMessage;
};

P2PSignalClient::P2PSignalClient(const std::string &url, const std::string &token, const std::string &streamPath,
				 const std::string &videoCodec, const std::string &audioCodec, uint32_t baseSsrc)
	: impl(std::make_unique<P2PSignalImpl>(url, token)),
	  streamPath(streamPath), videoCodec(videoCodec), audioCodec(audioCodec), baseSsrc(baseSsrc)
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
	auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, 96, rtc::H264RtpPacketizer::defaultClockRate);
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
