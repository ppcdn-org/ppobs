#pragma once

#include <rtc/rtc.hpp>

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <map>
#include <cstdint>
#include <vector>

struct P2PPeer {
	std::string sessionId;
	bool connected = false;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<rtc::Track> videoTrack;
	std::shared_ptr<rtc::Track> audioTrack;
	std::shared_ptr<rtc::RtcpSrReporter> videoSrReporter;
	std::shared_ptr<rtc::RtcpSrReporter> audioSrReporter;
	uint32_t videoSsrc = 0;
	uint32_t audioSsrc = 0;
	// Payload types the browser offered for H264/opus, parsed from the offer in
	// HandleOffer(). libdatachannel's answer hardcodes 96 (video) / 97 (audio),
	// but an answer must reuse the offer's payload-type mapping (RFC 3264).
	// Chrome offers H264 as e.g. 102/108/118 and PT 96 as VP8, so answering
	// "96=H264" leaves Chrome unable to bind the codec: the RTP still arrives
	// (packetsReceived climbs) but framesReceived/codecId stay at 0/none.
	int videoPayloadType = 96;
	int audioPayloadType = 97;
	std::string videoFmtp;
	std::string audioFmtp;
	uint16_t videoSequenceNumber = 0;
	uint16_t audioSequenceNumber = 0;
	uint32_t videoRtpTimestamp = 0;
	uint32_t audioRtpTimestamp = 0;
	int64_t lastVideoTimestamp = 0;
	int64_t lastAudioTimestamp = 0;
	// A peer joins mid-stream, so the first packet it would see is almost
	// never a keyframe and carries no SPS/PPS - the browser then has
	// nothing to initialize its decoder with and drops everything until
	// the next IDR. Hold the feed until a keyframe arrives so the very
	// first RTP the peer receives starts a decodable access unit.
	// (videoStarted also marks "lastVideoTimestamp is meaningful", so the
	// first duration is 0 instead of the whole stream's elapsed time.)
	bool videoStarted = false;
	bool audioStarted = false;
};

class P2PSignalImpl;

class P2PSignalClient {
public:
	P2PSignalClient(const std::string &url, const std::string &token, const std::string &streamPath,
			const std::string &videoCodec, const std::string &audioCodec, uint32_t baseSsrc,
			std::vector<std::string> stunServers);
	~P2PSignalClient();

	bool Start();
	void Stop();
	void SetOnPeerUpdated(std::function<void()> callback);
	size_t PeerCount();
	void ForEachPeer(std::function<void(P2PPeer &)> callback);
	void SendAnswer(const std::string &sessionId, const std::string &sdp);
	void SendIce(const std::string &sessionId, const std::string &candidate, const std::string &sdpMid,
		     int sdpMLineIndex);
	void SendConnected(const std::string &sessionId);
	void SendClose(const std::string &sessionId, const std::string &reason);

private:
	void OnMessage(const std::string &data);
	void HandleAllocated(const std::string &sessionId);
	void HandleOffer(const std::string &sessionId, const std::string &sdp);
	void HandleIce(const std::string &sessionId, const std::string &candidate, const std::string &sdpMid);
	void HandleClose(const std::string &sessionId);
	void CreatePeerVideo(P2PPeer &peer);
	void CreatePeerAudio(P2PPeer &peer);
	void RemovePeer(const std::string &sessionId);

	std::unique_ptr<P2PSignalImpl> impl;
	std::string streamPath;
	std::string videoCodec;
	std::string audioCodec;
	uint32_t baseSsrc;
	std::vector<std::string> stunServers;
	std::function<void()> onPeerUpdated;
	std::mutex peersMutex;
	std::map<std::string, std::shared_ptr<P2PPeer>> peers;
	std::atomic<bool> running{false};
};
