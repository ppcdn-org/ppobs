#include "ppcenter-node-channel.h"
#include "ppcenter-node-proto.h"
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
#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace {

const size_t MAX_WS_HANDSHAKE_BYTES = 16384;
const size_t MAX_WS_PAYLOAD_BYTES = 1024 * 1024;
const int WS_SOCKET_TIMEOUT_MS = 5000;

std::vector<unsigned char> RandomBytes(size_t count)
{
	std::vector<unsigned char> bytes(count);
	std::random_device rd;
	for (auto &byte : bytes)
		byte = static_cast<unsigned char>(rd());
	return bytes;
}

std::string LocalBase64Encode(const unsigned char *data, size_t len)
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
	if (flags < 0) return false;
	if (blocking) flags &= ~O_NONBLOCK;
	else flags |= O_NONBLOCK;
	return fcntl(socket, F_SETFL, flags) == 0;
#endif
}

bool ConnectWithTimeout(sock_t socket, const sockaddr *addr, socklen_t addrLen)
{
	if (!SetSocketBlocking(socket, false)) return false;
	int rc = ::connect(socket, addr, (int)addrLen);
	if (rc == SOCK_ERR) {
#ifdef _WIN32
		const int err = WSAGetLastError();
		if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL)
			return false;
#else
		if (errno != EINPROGRESS) return false;
#endif
	}
	fd_set writeSet;
	FD_ZERO(&writeSet);
	FD_SET(socket, &writeSet);
	timeval timeout{};
	timeout.tv_sec = WS_SOCKET_TIMEOUT_MS / 1000;
	timeout.tv_usec = (WS_SOCKET_TIMEOUT_MS % 1000) * 1000;
	const int ready = select((int)socket + 1, nullptr, &writeSet, nullptr, &timeout);
	if (ready <= 0 || !FD_ISSET(socket, &writeSet)) return false;

	int socketError = 0;
#ifdef _WIN32
	int optLen = sizeof(socketError);
#else
	socklen_t optLen = sizeof(socketError);
#endif
	if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&socketError), &optLen) == SOCK_ERR)
		return false;
	if (socketError != 0) return false;

	return SetSocketBlocking(socket, true);
}

} // namespace

// ── Transport layer (abstracts raw vs TLS) ──

class WSTransport {
public:
	~WSTransport() { closeTransport(); }

	bool openRaw(const std::string &host, const std::string &port)
	{
		struct addrinfo hints = {}, *result = nullptr;
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) return false;
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
		return sock != SOCK_INVALID;
	}

	bool openTLS(const std::string &host, const std::string &path)
	{
		curl = curl_easy_init();
		if (!curl) return false;
		std::string httpsUrl = "https://" + host + path;
		char curlError[CURL_ERROR_SIZE] = {};
		curl_easy_setopt(curl, CURLOPT_URL, httpsUrl.c_str());
		curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(WS_SOCKET_TIMEOUT_MS) / 1000L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(WS_SOCKET_TIMEOUT_MS) / 1000L);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
		CURLcode rc = curl_easy_perform(curl);
		if (rc != CURLE_OK) {
			blog(LOG_WARNING, "[ppobs node] WSS TLS connect failed: %s",
			     curlError[0] ? curlError : curl_easy_strerror(rc));
			closeTransport();
			return false;
		}
		return true;
	}

	bool isOpen() const { return sock != SOCK_INVALID || curl != nullptr; }

	void closeTransport()
	{
		if (curl) { curl_easy_cleanup(curl); curl = nullptr; }
		if (sock != SOCK_INVALID) { SOCK_CLOSE(sock); sock = SOCK_INVALID; }
	}

	bool sendAll(const char *data, size_t length)
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

	int readNet(char *buffer, size_t length)
	{
		if (curl) {
			size_t n = 0;
			CURLcode rc = curl_easy_recv(curl, buffer, length, &n);
			if (rc != CURLE_OK || n == 0) return -1;
			return static_cast<int>(n);
		}
		return SOCK_RECV(sock, buffer, (int)length);
	}

private:
	sock_t sock = SOCK_INVALID;
	CURL *curl = nullptr;
};

// ── NodeChannelClient implementation ──

NodeChannelClient::NodeChannelClient(const NodeChannelConfig &config)
	: config(config)
{
}

NodeChannelClient::~NodeChannelClient()
{
	Stop();
}

void NodeChannelClient::SetCommandHandler(CommandHandler handler)
{
	this->handler = std::move(handler);
}

bool NodeChannelClient::Start()
{
	if (running.exchange(true))
		return true;
	workerThread = std::thread(&NodeChannelClient::Run, this);
	return true;
}

void NodeChannelClient::Stop()
{
	if (!running.exchange(false))
		return;
	{
		std::lock_guard<std::mutex> lock(sendMutex);
	}
	if (workerThread.joinable())
		workerThread.join();
}

bool NodeChannelClient::IsConnected() const
{
	return connected;
}

void NodeChannelClient::Run()
{
	int backoffSec = 1;
	const int maxBackoffSec = 30;
	int consecutiveWSFailures = 0;
	bool inHTTPMode = false;
	bool connectedHTTPMode = false;
	int attemptWSBeforeHTTP = 0;

	while (running) {
		if (running && !inHTTPMode) {
			// Attempt WS connection
			if (ConnectWS()) {
				consecutiveWSFailures = 0;
				backoffSec = 1;
				blog(LOG_INFO, "[ppobs node] WS control channel connected to %s", config.url.c_str());

				// Send initial indication immediately.
				{
					auto frame = BuildIndication();
					SendBinaryFrame(frame);
				}

				auto lastHeartbeat = std::chrono::steady_clock::now();
				const auto heartbeatInterval = std::chrono::seconds(config.heartbeatIntervalSec);

				// WS main loop
				while (running && transport && transport->isOpen()) {
					unsigned char header[2];
					if (!ReadExact(reinterpret_cast<char *>(header), 2)) {
						auto now = std::chrono::steady_clock::now();
						if (now - lastHeartbeat >= heartbeatInterval) {
							auto frame = BuildIndication();
							if (!SendBinaryFrame(frame)) {
								blog(LOG_WARNING, "[ppobs node] WS heartbeat failed, disconnecting");
								break;
							}
							lastHeartbeat = now;
						}
						if (!transport->isOpen()) break;
						continue;
					}

					int opcode = header[0] & 0x0F;
					size_t payloadLen = header[1] & 0x7F;
					if (!ReadExtendedLength(payloadLen)) break;

					if (opcode == 0x08) {
						blog(LOG_INFO, "[ppobs node] server closed WS");
						break;
					}
					if (opcode == 0x09) {
						std::vector<char> payload;
						if (!ReadPayload(payloadLen, payload)) break;
						SendPong(std::string(payload.data(), payloadLen));
						continue;
					}
					if (opcode == 0x02) {
						std::vector<uint8_t> payload;
						if (!ReadBinaryPayload(payloadLen, payload)) break;

						std::string msgStr(payload.begin(), payload.end());
						if (msgStr.find("RECONNECT_HINT") != std::string::npos) {
							blog(LOG_INFO, "[ppobs node] received RECONNECT_HINT, reconnecting");
							break;
						}

						auto req = ParseNodeMsgReq(payload);
						if (req.valid && handler) {
							auto rsp = handler(req);
							SendBinaryFrame(BuildNodeMsgRsp(rsp));
						}
						continue;
					}
					// unknown opcode: skip
					{
						std::vector<char> dummy;
						if (!ReadPayload(payloadLen, dummy)) break;
					}
				}
			} else {
				consecutiveWSFailures++;
				blog(LOG_DEBUG, "[ppobs node] WS connect failed (%d/%d)", consecutiveWSFailures, config.wsFailThreshold);
				if (consecutiveWSFailures >= config.wsFailThreshold) {
					inHTTPMode = true;
					blog(LOG_WARNING, "[ppobs node] switching to HTTP fallback after %d WS failures",
                             consecutiveWSFailures);
				} else if (!running) {
					return;
				}
			}
		}

		// Cleanup WS transport before entering next state
		connected = false;
		if (transport) {
			transport->closeTransport();
			transport.reset();
		}

		if (!running) return;

		if (inHTTPMode) {
			// HTTP fallback loop
			auto httpPollInterval = std::chrono::seconds(config.httpPollIntervalSec);
			blog(LOG_INFO, "[ppobs node] entering HTTP fallback mode");
			connectedHTTPMode = true;
			HTTPSendHeartbeat();  // register heartbeat
			while (running && connectedHTTPMode) {
				auto nextTick = std::chrono::steady_clock::now();
				for (int i = 0; i < config.httpPollIntervalSec; i++) {
					if (!running) return;
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
				if (!running) return;
				if (!HTTPSendHeartbeat()) {
					blog(LOG_WARNING, "[ppobs node] HTTP heartbeat failed, retrying WS");
					connectedHTTPMode = false;
					continue;
				}
				HTTPPollCommands();

				// Periodically try to reconnect via WS
				if (++attemptWSBeforeHTTP >= config.httpPollIntervalSec * 2) {
					attemptWSBeforeHTTP = 0;
					if (ConnectWS()) {
						inHTTPMode = false;
						break;
					}
				}
			}
			connectedHTTPMode = false;
		}

		// Backoff before retrying
		if (!running) return;
		if (!inHTTPMode) {
			blog(LOG_DEBUG, "[ppobs node] WS will retry in %ds", backoffSec);
			std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
			backoffSec = std::min(backoffSec * 2, maxBackoffSec);
		}
	}
}

bool NodeChannelClient::ConnectWS()
{
	auto parsed = ParseWebSocketSignalURL(config.url);
	if (!parsed.valid) {
		blog(LOG_WARNING, "[ppobs node] invalid node channel URL");
		return false;
	}

	transport = std::make_unique<WSTransport>();
	std::string host = parsed.host;
	std::string port = parsed.tls ? "443" : "80";
	std::string h = host;
	auto portPos = host.find(':');
	if (portPos != std::string::npos) {
		h = host.substr(0, portPos);
		port = host.substr(portPos + 1);
	}

	bool opened = false;
	if (parsed.tls) {
		opened = transport->openTLS(h, parsed.path);
	} else {
		opened = transport->openRaw(h, port);
	}
	if (!opened) return false;

	// Complete WebSocket handshake
	auto nonce = RandomBytes(16);
	const auto wsKey = LocalBase64Encode(nonce.data(), nonce.size());

	std::ostringstream req;
	req << "GET " << parsed.path << " HTTP/1.1\r\n"
	    << "Host: " << host << "\r\n"
	    << "Upgrade: websocket\r\n"
	    << "Connection: Upgrade\r\n"
	    << "Sec-WebSocket-Key: " << wsKey << "\r\n"
	    << "Sec-WebSocket-Version: 13\r\n";
	if (!config.bearerToken.empty())
		req << "Authorization: Bearer " << config.bearerToken << "\r\n";
	req << "\r\n";

	std::string reqStr = req.str();
	if (!transport->sendAll(reqStr.data(), reqStr.size())) {
		transport->closeTransport();
		return false;
	}

	// Read response
	std::string response;
	std::vector<char> readAhead;
	while (response.find("\r\n\r\n") == std::string::npos) {
		char buf[1024];
		const int n = transport->readNet(buf, sizeof(buf));
		if (n <= 0) {
			transport->closeTransport();
			return false;
		}
		response.append(buf, n);
		if (response.size() > MAX_WS_HANDSHAKE_BYTES) {
			transport->closeTransport();
			return false;
		}
	}
	const auto headerEnd = response.find("\r\n\r\n") + 4;
	readAhead.assign(response.begin() + headerEnd, response.end());
	response.resize(headerEnd);

	if (!WebSocketHandshakeAccepted(response) || !HasExpectedAccept(response, wsKey)) {
		blog(LOG_WARNING, "[ppobs node] WebSocket handshake rejected");
		transport->closeTransport();
		return false;
	}

	// Save remaining bytes after handshake as pending read
	readAhead.swap(pendingRead);

	connected = true;
	return true;
}

bool NodeChannelClient::ReadExact(char *buffer, size_t length)
{
	size_t copied = 0;
	if (!pendingRead.empty()) {
		copied = std::min(length, pendingRead.size());
		memcpy(buffer, pendingRead.data(), copied);
		pendingRead.erase(pendingRead.begin(), pendingRead.begin() + copied);
	}
	while (copied < length) {
		const int n = transport->readNet(buffer + copied, length - copied);
		if (n <= 0) return false;
		copied += n;
	}
	return true;
}

bool NodeChannelClient::ReadExtendedLength(size_t &payloadLen)
{
	if (payloadLen == 126) {
		unsigned char ext[2];
		if (!ReadExact(reinterpret_cast<char *>(ext), 2)) return false;
		payloadLen = (static_cast<size_t>(ext[0]) << 8) | ext[1];
	} else if (payloadLen == 127) {
		unsigned char ext[8];
		if (!ReadExact(reinterpret_cast<char *>(ext), 8)) return false;
		payloadLen = 0;
		for (int i = 0; i < 8; i++)
			payloadLen = (payloadLen << 8) | ext[i];
	}
	return true;
}

bool NodeChannelClient::ReadPayload(size_t payloadLen, std::vector<char> &payload)
{
	if (payloadLen > MAX_WS_PAYLOAD_BYTES) {
		blog(LOG_WARNING, "[ppobs node] payload too large: %zu bytes", payloadLen);
		return false;
	}
	payload.resize(payloadLen);
	if (payloadLen > 0 && !ReadExact(payload.data(), payloadLen)) return false;
	return true;
}

bool NodeChannelClient::ReadBinaryPayload(size_t payloadLen, std::vector<uint8_t> &payload)
{
	if (payloadLen > MAX_WS_PAYLOAD_BYTES) {
		blog(LOG_WARNING, "[ppobs node] control frame too large: %zu bytes", payloadLen);
		return false;
	}
	payload.resize(payloadLen);
	if (payloadLen > 0 && !ReadExact(reinterpret_cast<char *>(payload.data()), payloadLen)) return false;
	return true;
}

void NodeChannelClient::SendPong(const std::string &payload)
{
	std::lock_guard<std::mutex> lock(sendMutex);
	if (!transport || !transport->isOpen()) return;
	auto mask = RandomBytes(4);
	std::array<unsigned char, 4> key = {mask[0], mask[1], mask[2], mask[3]};
	auto frame = BuildClientWebSocketFrame(0x0A, payload, key);
	transport->sendAll(frame.data(), frame.size());
}

bool NodeChannelClient::SendBinaryFrame(const std::vector<uint8_t> &data)
{
	std::lock_guard<std::mutex> lock(sendMutex);
	if (!transport || !transport->isOpen()) return false;
	auto mask = RandomBytes(4);
	std::array<unsigned char, 4> key = {mask[0], mask[1], mask[2], mask[3]};
	std::string framePayload(data.begin(), data.end());
	auto frame = BuildClientWebSocketFrame(0x02, framePayload, key);
	return transport->sendAll(frame.data(), frame.size());
}

std::vector<uint8_t> NodeChannelClient::BuildIndication() const
{
	WorkerIndication ind;
	ind.msgType = "COMMAND_TYPE_INDICATION";
	ind.workerType = config.workerRole;
	ind.workerId = config.workerId;
	ind.version = config.nodeVersion;
	ind.region = config.nodeRegion;
	ind.capacity = config.nodeCapacity;
	return ind.Encode();
}

std::string NodeChannelClient::BuildHTTPBase() const
{
	std::string base;
	auto u = config.url;
	auto schemeEnd = u.find("://");
	if (schemeEnd == std::string::npos) return base;
	auto hostStart = schemeEnd + 3;
	auto pathStart = u.find('/', hostStart);
	std::string host = (pathStart == std::string::npos) ? u.substr(hostStart) : u.substr(hostStart, pathStart - hostStart);
	base = (u.compare(0, 4, "wss:") == 0 ? "https://" : "http://") + host;
	return base;
}

bool NodeChannelClient::HTTPSendHeartbeat()
{
	CURL *curl = curl_easy_init();
	if (!curl) return false;
	std::string url = BuildHTTPBase() + "/internal/mmx/v1/heartbeat";
	auto body = BuildIndication();
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/x-protobuf");
	if (!config.bearerToken.empty())
		headers = curl_slist_append(headers, ("Authorization: Bearer " + config.bearerToken).c_str());
	long http_code = 0;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.data());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode rc = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	return rc == CURLE_OK && (http_code >= 200 && http_code < 300);
}

bool NodeChannelClient::HTTPPollCommands()
{
	CURL *curl = curl_easy_init();
	if (!curl) return false;
	std::string url = BuildHTTPBase() + "/internal/mmx/v1/commands/pending?workerId=" + std::to_string(config.workerId);
	struct curl_slist *headers = nullptr;
	if (!config.bearerToken.empty())
		headers = curl_slist_append(headers, ("Authorization: Bearer " + config.bearerToken).c_str());
	std::vector<uint8_t> body;
	auto writeCb = [](char *ptr, size_t n, size_t l, void *user) -> size_t {
		auto *buf = static_cast<std::vector<uint8_t> *>(user);
		buf->insert(buf->end(), ptr, ptr + n * l);
		return n * l;
	};
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	CURLcode rc = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK || body.empty()) return false;

	auto req = ParseNodeMsgReq(body);
	if (req.valid && handler) {
		auto rsp = handler(req);
		HTTPSendCommandResponse(BuildNodeMsgRsp(rsp));
	}
	return true;
}

void NodeChannelClient::HTTPSendCommandResponse(const std::vector<uint8_t> &data)
{
	CURL *curl = curl_easy_init();
	if (!curl) return;
	std::string url = BuildHTTPBase() + "/internal/mmx/v1/commands/response";
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/x-protobuf");
	if (!config.bearerToken.empty())
		headers = curl_slist_append(headers, ("Authorization: Bearer " + config.bearerToken).c_str());
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(data.size()));
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, data.data());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
}