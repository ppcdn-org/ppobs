#include "whip-output.h"
#include "whip-utils.h"
#include "whip-service.h"
#include "ppcenter-client.h"
#include "ppcenter-signal.h"
#include "ppcenter-node-channel.h"
#include "ppcenter-node-proto.h"
#include "uplink-qos-policy.h"

#include <obs.hpp>
#include <util/dstr.h>
#include <util/ntp-clock.h>

#include <curl/curl.h>

#include <nlohmann/json.hpp>

#ifdef WHIP_DEGRADE_ACTIVE
#include "degrade-client.h"
#endif

/*
 * Sets the maximum size for a video fragment. Effective range is
 * 576-1470, with a lower value equating to more packets created,
 * but also better network compatability.
 */
static uint16_t MAX_VIDEO_FRAGMENT_SIZE = 1200;

/*
 * libdatachannel's ICE agent (unlike e.g. Pion, used on the MMX/server
 * side) reports PeerConnection::State::Disconnected the moment ICE
 * connectivity checks start failing, with no built-in tolerance for a
 * transient blip. Reacting to that immediately by tearing down and
 * rebuilding the whole WHIP session (DELETE + new POST + fresh ICE +
 * DTLS) is far more disruptive than the network hiccup that triggered
 * it - most Disconnected states self-recover within a few seconds.
 * This grace period gives the ICE agent time to recover on its own
 * before we give up; PeerConnection::State::Failed (which libdatachannel
 * only reports once ICE has actually given up) is still handled
 * immediately, with no grace period.
 */
const int signaling_media_id_length = 16;
const char signaling_media_id_valid_char[] = "0123456789"
					     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
					     "abcdefghijklmnopqrstuvwxyz";

const std::string user_agent = generate_user_agent();

const char *audio_mid = "0";
const uint8_t audio_payload_type = 111;
const char *video_mid = "1";
const uint8_t video_payload_type = 96;
const int video_nack_buffer_size = 4000;

const std::string rtpHeaderExtUriMid = "urn:ietf:params:rtp-hdrext:sdes:mid";
const std::string rtpHeaderExtUriRid = "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id";

static size_t curl_write_body(char *data, size_t size, size_t nmemb, void *priv)
{
	auto *buf = static_cast<std::string *>(priv);
	buf->append(data, size * nmemb);
	return size * nmemb;
}

static size_t curl_write_headers(char *data, size_t size, size_t nmemb, void *priv)
{
	auto *buf = static_cast<std::vector<std::string> *>(priv);
	buf->push_back(trim_string(std::string(data, size * nmemb)));
	return size * nmemb;
}

WHIPOutput::WHIPOutput(obs_data_t *, obs_output_t *output)
	: output(output),
	  endpoint_url(),
	  bearer_token(),
	  resource_url(),
	  active_generation(0),
	  running(false),
	  start_stop_mutex(),
	  start_stop_thread(),
	  base_ssrc(generate_random_u32()),
	  peer_connection(nullptr),
	  audio_track(nullptr),
	  video_track(nullptr),
	  total_bytes_sent(0),
	  connect_time_ms(0),
	  start_time_ns(0),
	  last_audio_timestamp(0)
{
}

WHIPOutput::~WHIPOutput()
{
	if (p2pSignal) {
		p2pSignal.reset();
	}
	Stop();

	std::lock_guard<std::mutex> l(start_stop_mutex);
	if (start_stop_thread.joinable())
		start_stop_thread.join();
}

bool WHIPOutput::Start()
{
	std::lock_guard<std::mutex> l(start_stop_mutex);
	const uint64_t generation = active_generation.fetch_add(1) + 1;

	// A fresh (non-reconnect) Start() means the user explicitly asked
	// for a new stream - don't carry over backoff state from whatever
	// reconnect attempts preceded it.
	if (!obs_output_reconnecting(output))
		reconnect_attempt = 0;

	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		auto encoder = obs_output_get_video_encoder2(output, idx);
		if (encoder == nullptr) break;
		auto v = std::make_shared<videoLayerState>();
		v->ssrc = base_ssrc + 1 + idx;
		v->rid = std::to_string(idx);
		videoLayerStates[encoder] = v;
	}

	if (!obs_output_can_begin_data_capture(output, 0))
		return false;
	if (!obs_output_initialize_encoders(output, 0))
		return false;

	if (start_stop_thread.joinable())
		start_stop_thread.join();
	start_stop_thread = std::thread(&WHIPOutput::StartThread, this, generation);
	return true;
}

void WHIPOutput::Stop(bool signal)
{
	// Whatever the reason we're stopping (user request, Failed state, or
	// the grace timer below giving up), any pending Disconnected grace
	// timer is now moot.
	CancelDisconnectGraceTimer();

#ifdef WHIP_DEGRADE_ACTIVE
	WsDegradeClient::Instance().UnregisterOutput();
#endif

	std::lock_guard<std::mutex> l(start_stop_mutex);
	const uint64_t generation = active_generation.load();
	std::string resourceURL;
	{
		std::lock_guard<std::mutex> rl(resource_mutex);
		resourceURL = resource_url;
	}
	if (start_stop_thread.joinable())
		start_stop_thread.join();
	start_stop_thread = std::thread(&WHIPOutput::StopThread, this, signal, generation, resourceURL);
}

void WHIPOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	CheckUplinkQos();

	// Hold the shared (reader) lock for the whole function, not just
	// while snapshotting the shared_ptrs: StartThread()/StopThread()
	// take the exclusive (writer) lock around peer_connection->close(),
	// so this also blocks a close() from running concurrently with the
	// track->send() calls below. Without that, close() can tear down
	// the transport out from under a send() that libdatachannel's RTC
	// worker thread is still in the middle of delivering.
	std::shared_lock<std::shared_mutex> lk(tracks_mutex);
	std::shared_ptr<rtc::Track> local_audio_track = audio_track;
	std::shared_ptr<rtc::Track> local_video_track = video_track;
	std::shared_ptr<rtc::DataChannel> local_timestamp_channel = timestamp_channel;
	std::shared_ptr<rtc::RtcpSrReporter> local_audio_sr_reporter = audio_sr_reporter;
	std::shared_ptr<rtc::RtcpSrReporter> local_video_sr_reporter = video_sr_reporter;

	if (local_audio_track && packet->type == OBS_ENCODER_AUDIO) {
		int64_t duration = packet->dts_usec - last_audio_timestamp;
		Send(packet->data, packet->size, duration, local_audio_track, local_audio_sr_reporter);
		last_audio_timestamp = packet->dts_usec;

		if (p2pSignal) {
			p2pSignal->ForEachPeer([&](P2PPeer &peer) {
				int64_t dur = packet->dts_usec - peer.lastAudioTimestamp;
				peer.lastAudioTimestamp = packet->dts_usec;
				if (peer.audioTrack && peer.audioTrack->isOpen()) {
					std::vector<rtc::byte> sample{(rtc::byte *)packet->data,
								      (rtc::byte *)packet->data + packet->size};
					auto rtp = peer.audioSrReporter->rtpConfig;
					rtp->sequenceNumber = peer.audioSequenceNumber;
					rtp->timestamp = peer.audioRtpTimestamp;
					rtp->ssrc = peer.audioSsrc;
					try { peer.audioTrack->send(sample); } catch (...) {}
					peer.audioSequenceNumber = rtp->sequenceNumber;
					peer.audioRtpTimestamp = rtp->timestamp;
				}
			});
		}
	} else if (local_video_track && packet->type == OBS_ENCODER_VIDEO) {
		auto rtp_config = local_video_sr_reporter->rtpConfig;
		auto videoLayerState = videoLayerStates[packet->encoder];
		if (videoLayerState == nullptr) {
			// Stale encoder packet in flight after reconnect:
			// Start() rebuilt the map with new encoders, but
			// old encoder packets can still arrive. Drop
			// silently instead of killing the stream.
			do_log(LOG_DEBUG,
			       "Data() video packet encoder=%p not found in videoLayerStates (size=%zu) - dropping stale packet",
			       (void *)packet->encoder, videoLayerStates.size());
			return;
		}
		rtp_config->sequenceNumber = videoLayerState->sequenceNumber;
		rtp_config->ssrc = videoLayerState->ssrc;
		rtp_config->rid = videoLayerState->rid;
		rtp_config->timestamp = videoLayerState->rtpTimestamp;
		int64_t duration = packet->dts_usec - videoLayerState->lastVideoTimestamp;

		// frame_no is the starting RTP sequence number for this frame's
		// first packet (it wraps at 65536 same as the RTP field itself).
		// See docs/obs-abs-timestamp-protocol.md.
		if (local_timestamp_channel && local_timestamp_channel->isOpen()) {
			nlohmann::json ts_msg = {
				{"frame_no", videoLayerState->sequenceNumber},
				{"timestamp", ntp_clock_now_ms()},
				{"rid", videoLayerState->rid},
			};
			try {
				local_timestamp_channel->send(ts_msg.dump());
			} catch (const std::exception &e) {
				do_log(LOG_DEBUG, "timestamp_channel send failed: %s", e.what());
			}
		}

		Send(packet->data, packet->size, duration, local_video_track, local_video_sr_reporter);
		videoLayerState->sequenceNumber = rtp_config->sequenceNumber;
		videoLayerState->lastVideoTimestamp = packet->dts_usec;
		videoLayerState->rtpTimestamp = rtp_config->timestamp;

		if (p2pSignal) {
			p2pSignal->ForEachPeer([&](P2PPeer &peer) {
				int64_t dur = packet->dts_usec - peer.lastVideoTimestamp;
				peer.lastVideoTimestamp = packet->dts_usec;
				if (peer.videoTrack && peer.videoTrack->isOpen()) {
					std::vector<rtc::byte> sample{(rtc::byte *)packet->data,
								      (rtc::byte *)packet->data + packet->size};
					auto rtp = peer.videoSrReporter->rtpConfig;
					rtp->sequenceNumber = peer.videoSequenceNumber;
					rtp->timestamp = peer.videoRtpTimestamp;
					rtp->ssrc = peer.videoSsrc;
					rtp->rid = "0";
					try { peer.videoTrack->send(sample); } catch (...) {}
					peer.videoSequenceNumber = rtp->sequenceNumber;
					peer.videoRtpTimestamp = rtp->timestamp;
				}
			});
		}
	}
}

void WHIPOutput::ConfigureAudioTrack(std::string media_stream_id, std::string cname)
{
	if (!obs_output_get_audio_encoder(output, 0)) {
		do_log(LOG_DEBUG, "Not configuring audio track: Audio encoder not assigned");
		return;
	}
	uint32_t ssrc = base_ssrc;
	auto media_stream_track_id = std::string(media_stream_id + "-audio");
	rtc::Description::Audio audio_description(audio_mid, rtc::Description::Direction::SendOnly);
	audio_description.addOpusCodec(audio_payload_type);
	audio_description.addSSRC(ssrc, cname, media_stream_id, media_stream_track_id);
	auto new_audio_track = peer_connection->addTrack(audio_description);
	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, audio_payload_type, rtc::OpusRtpPacketizer::DefaultClockRate);
	auto new_audio_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);
	packetizer->addToChain(new_audio_sr_reporter);
	packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
	new_audio_track->setMediaHandler(packetizer);

	std::unique_lock<std::shared_mutex> lk(tracks_mutex);
	audio_track = new_audio_track;
	audio_sr_reporter = new_audio_sr_reporter;
}

void WHIPOutput::ConfigureVideoTrack(std::string media_stream_id, std::string cname)
{
	if (!obs_output_get_video_encoder(output)) {
		do_log(LOG_DEBUG, "Not configuring video track: Video encoder not assigned");
		return;
	}
	std::shared_ptr<rtc::RtpPacketizer> packetizer;
	uint32_t ssrc = base_ssrc + 1;
	auto media_stream_track_id = std::string(media_stream_id + "-video");
	rtc::Description::Video video_description(video_mid, rtc::Description::Direction::SendOnly);
	video_description.addSSRC(ssrc, cname, media_stream_id, media_stream_track_id);
	video_description.addExtMap(rtc::Description::Entry::ExtMap(1, rtpHeaderExtUriMid));
	video_description.addExtMap(rtc::Description::Entry::ExtMap(2, rtpHeaderExtUriRid));
	if (videoLayerStates.size() >= 2) {
		std::vector<std::pair<int, std::string>> sortedRids;
		for (const auto &[encoder, state] : videoLayerStates)
			sortedRids.push_back({std::stoi(state->rid), state->rid});
		std::sort(sortedRids.begin(), sortedRids.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
		for (const auto &[_, rid] : sortedRids) video_description.addRid(rid);
	}
	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, video_payload_type,
#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR > 22 || RTC_VERSION_MAJOR > 0
									rtc::H264RtpPacketizer::ClockRate);
#else
									rtc::H264RtpPacketizer::defaultClockRate);
#endif
	rtp_config->midId = 1;
	rtp_config->ridId = 2;
	rtp_config->mid = video_mid;

	const obs_encoder_t *encoder = obs_output_get_video_encoder2(output, 0);
	if (!encoder) return;
	OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
	auto video_bitrate = (int)obs_data_get_int(settings, "bitrate");
	const char *codec = obs_encoder_get_codec(encoder);
	if (strcmp("h264", codec) == 0) {
		video_description.addH264Codec(video_payload_type);
		packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::H264RtpPacketizer::Separator::StartSequence, rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
#ifdef ENABLE_HEVC
	} else if (strcmp("hevc", codec) == 0) {
		video_description.addH265Codec(video_payload_type);
		packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::H265RtpPacketizer::Separator::StartSequence, rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
#endif
	} else if (strcmp("av1", codec) == 0) {
		video_description.addAV1Codec(video_payload_type);
		packetizer = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit, rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
	} else {
		do_log(LOG_ERROR, "Video codec not supported: %s", codec);
		return;
	}
	auto new_video_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	packetizer->addToChain(new_video_sr_reporter);
	packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(video_nack_buffer_size));
	if (video_bitrate != 0)
		packetizer->addToChain(std::make_shared<rtc::PacingHandler>(static_cast<double>(video_bitrate * 10000), std::chrono::milliseconds(5)));
	auto new_video_track = peer_connection->addTrack(video_description);
	new_video_track->setMediaHandler(packetizer);

	std::unique_lock<std::shared_mutex> lk(tracks_mutex);
	video_track = new_video_track;
	video_sr_reporter = new_video_sr_reporter;
}

bool WHIPOutput::Init()
{
	OBSDataAutoRelease output_settings = obs_output_get_settings(output);
	disconnect_grace_sec = (int)obs_data_get_int(output_settings, "whip_disconnect_grace_sec");
	reconnect_backoff_sec = (int)obs_data_get_int(output_settings, "whip_reconnect_backoff_sec");
	if (!obs_data_has_user_value(output_settings, "whip_disconnect_grace_sec"))
		disconnect_grace_sec = 10;
	if (!obs_data_has_user_value(output_settings, "whip_reconnect_backoff_sec"))
		reconnect_backoff_sec = 3;
	if (disconnect_grace_sec < 0)
		disconnect_grace_sec = 0;
	if (reconnect_backoff_sec < 0)
		reconnect_backoff_sec = 0;

	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}
	endpoint_url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	bearer_token = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN);
	if (endpoint_url.empty()) {
		obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}
	return true;
}

bool WHIPOutput::Setup(uint64_t generation)
{
	if (!Init()) return false;
	if (!IsActiveGeneration(generation)) return false;

	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}
	obs_data_t *service_settings = obs_service_get_settings(service);
	const bool ppcenter_enabled = obs_data_get_bool(service_settings, "ppcenter_enabled");
	if (ppcenter_enabled) {
		PPCenterPublishRequest request{obs_data_get_string(service_settings, "ppcenter_url"),
					       obs_data_get_string(service_settings, "ppcenter_appid"),
					       obs_data_get_string(service_settings, "ppcenter_secret"),
					       obs_data_get_string(service_settings, "ppcenter_stream"),
					       obs_data_get_string(service_settings, "ppcenter_region")};
		PPCenterPublishResponse resp;
		std::string error;
		if (!ppcenter_resolve_publish(request, resp, error)) {
			do_log(LOG_ERROR, "ppcenter resolve publish failed: %s", error.c_str());
			if (IsActiveGeneration(generation))
				obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
			return false;
		}
		endpoint_url = resp.whip_url;
		bearer_token = resp.bearer_token;
		p2pToken = resp.signal_token;
		p2pSignalUrl = resp.signal_url;
		p2pStreamPath = request.app_id + "/" + request.stream_name;
		do_log(LOG_INFO, "ppcenter resolved WHIP: %s P2P: %s", endpoint_url.c_str(),
		       p2pSignalUrl.empty() ? "disabled" : "enabled");
	}

	// Configure node channel from service settings (derived from ppcenter)
	{
		std::string baseUrl = obs_data_get_string(service_settings, "ppcenter_url");
		if (!baseUrl.empty()) {
			// Derive ws URL from ppcenter URL
			// e.g. http://ppcenter:8090 → ws://ppcenter:8090/ws/mmx
			auto schemeEnd = baseUrl.find("://");
			if (schemeEnd != std::string::npos) {
				std::string hostPart = baseUrl.substr(schemeEnd + 3);
				auto slashPos = hostPart.find('/');
				if (slashPos != std::string::npos)
					hostPart = hostPart.substr(0, slashPos);
				nodeChannelConfig.url = (baseUrl.find("https") == 0 ? "wss://" : "ws://") + hostPart + "/ws/mmx";
			}
			nodeChannelConfig.bearerToken = obs_data_get_string(service_settings, "ppcenter_secret");
			nodeChannelConfig.workerRole = "NODE_ROLE_PUBLISHER";
			// ppcenter_node_id is a stable machine identity string (disk serial);
			// fold it into the wire protocol's int32 workerId field.
			nodeChannelConfig.workerId = fnv1a_hash32(obs_data_get_string(service_settings, "ppcenter_node_id"));
			nodeChannelConfig.nodeVersion = "1.0.0";
			nodeChannelConfig.nodeRegion = obs_data_get_string(service_settings, "ppcenter_region");
			nodeChannelConfig.nodeCapacity = 1;
			nodeChannelConfig.heartbeatIntervalSec = 10;
		}
	}

	return true;
}

bool WHIPOutput::Connect(uint64_t generation, std::string &resourceURL)
{
	rtc::Configuration rtcConfig;
	std::vector<rtc::IceServer> iceServers;
	iceServers.emplace_back("stun:stun.l.google.com:19302");

	std::string media_stream_id;
	media_stream_id.reserve(signaling_media_id_length);
	for (int i = 0; i < signaling_media_id_length; i++)
		media_stream_id += signaling_media_id_valid_char[rand() % (sizeof(signaling_media_id_valid_char) - 1)];
	auto cname = "obs-studio-webrtc-output";

	peer_connection = std::make_shared<rtc::PeerConnection>(rtcConfig);

	peer_connection->onStateChange([this, generation](rtc::PeerConnection::State state) {
		if (!IsActiveGeneration(generation)) {
			do_log(LOG_DEBUG, "[WHIP generation=%llu] ignoring stale PeerConnection state change: %d",
			       static_cast<unsigned long long>(generation), static_cast<int>(state));
			return;
		}
		switch (state) {
		case rtc::PeerConnection::State::New:
			do_log(LOG_INFO, "PeerConnection state is now: New");
			break;
		case rtc::PeerConnection::State::Connecting:
			do_log(LOG_INFO, "PeerConnection state is now: Connecting");
			start_time_ns = os_gettime_ns();
			break;
		case rtc::PeerConnection::State::Connected:
			do_log(LOG_INFO, "PeerConnection state is now: Connected");
			connect_time_ms = (int)((os_gettime_ns() - start_time_ns) / 1000000.0);
			do_log(LOG_INFO, "Connect time: %dms", connect_time_ms.load());
			reconnect_attempt = 0;
			CancelDisconnectGraceTimer();
			break;
		case rtc::PeerConnection::State::Disconnected:
			do_log(LOG_INFO,
			       "PeerConnection state is now: Disconnected - waiting up to %ds for it to recover before tearing down",
			       disconnect_grace_sec);
			StartDisconnectGraceTimer(generation);
			break;
		case rtc::PeerConnection::State::Failed:
			do_log(LOG_INFO, "PeerConnection state is now: Failed");
			PrepareReconnect();
			Stop(false);
			// OBS_OUTPUT_DISCONNECTED (not OBS_OUTPUT_ERROR) so libobs's
			// own can_reconnect() treats this as a normal, reconnectable
			// network drop and silently retries via output_reconnect()
			// instead of immediately popping the "connection failed"
			// dialog in front of the user for something WHIP is already
			// about to recover from on its own.
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
			break;
		case rtc::PeerConnection::State::Closed:
			do_log(LOG_INFO, "PeerConnection state is now: Closed");
			break;
		}
	});

	ConfigureAudioTrack(media_stream_id, cname);
	ConfigureVideoTrack(media_stream_id, cname);

	// Side channel for per-frame push timestamps; see
	// docs/obs-abs-timestamp-protocol.md. Best-effort only: if the
	// remote end doesn't accept it, sends are just no-ops since
	// timestamp_channel never reports isOpen(). Must be created AFTER
	// the addTrack (Configure*Track) calls so libdatachannel includes
	// both the DataChannel and the media m-lines in the SDP offer.
	auto new_timestamp_channel = peer_connection->createDataChannel("obs-timestamp");
	{
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		timestamp_channel = new_timestamp_channel;
	}

	if (!IsActiveGeneration(generation)) {
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		peer_connection = nullptr;
		audio_track = nullptr;
		video_track = nullptr;
		timestamp_channel = nullptr;
		return false;
	}
	peer_connection->setLocalDescription();
	rtc::Description localDescription(peer_connection->localDescription().value());
	if (!video_track && !audio_track) return false;

	std::string sdp;
	localDescription.generateSdp(sdp);

	CURL *c = curl_easy_init();
	std::string body;
	std::vector<std::string> headers;
	curl_easy_setopt(c, CURLOPT_URL, endpoint_url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)sdp.length());
	curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, sdp.c_str());
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_body);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_write_headers);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);

	struct curl_slist *headerList = nullptr;
	std::string contentType = "application/sdp";
	headerList = curl_slist_append(headerList, ("Content-Type: " + contentType).c_str());
	if (!bearer_token.empty())
		headerList = curl_slist_append(headerList, ("Authorization: Bearer " + bearer_token).c_str());
	headerList = curl_slist_append(headerList, ("User-Agent: " + user_agent).c_str());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headerList);

	CURLcode res = curl_easy_perform(c);
	long http_code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headerList);
	curl_easy_cleanup(c);

	if (res != CURLE_OK) {
		do_log(LOG_ERROR, "WHIP request failed: curl error %d", res);
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
		return false;
	}
	if (http_code != 201) {
		do_log(LOG_ERROR, "WHIP request failed: HTTP %ld", http_code);
		if (IsActiveGeneration(generation)) {
			// 401/403/404 mean the server rejected this specific stream
			// (bad key/path/permissions) rather than a transient network
			// problem - retrying via libobs's reconnect timer would just
			// get the identical rejection every time. Signal
			// OBS_OUTPUT_INVALID_STREAM (not reconnectable, see
			// can_reconnect() in libobs/obs-output.c) so the user gets a
			// clear "invalid stream key/path" error and streaming
			// actually stops, instead of retrying forever with no
			// visible error (as OBS_OUTPUT_DISCONNECTED would do). Other
			// codes (5xx, etc.) keep the existing reconnectable behavior.
			bool permanent = http_code == 401 || http_code == 403 || http_code == 404;
			obs_output_signal_stop(output, permanent ? OBS_OUTPUT_INVALID_STREAM : OBS_OUTPUT_DISCONNECTED);
		}
		return false;
	}

	std::string locationHdr;
	for (const auto &h : headers) {
		if (h.size() > 10 && _strnicmp(h.c_str(), "location:", 9) == 0)
			locationHdr = trim_string(h.substr(9));
	}
	if (locationHdr.empty()) {
		do_log(LOG_ERROR, "WHIP response Location header is empty");
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
		return false;
	}
	resourceURL = locationHdr;

	for (const auto &h : headers) {
		if (h.size() > 6 && _strnicmp(h.c_str(), "link:", 5) == 0)
			ParseLinkHeader(trim_string(h.substr(5)), iceServers);
	}

	peer_connection->setRemoteDescription(rtc::Description(sdp, "answer"));
	if (!IsActiveGeneration(generation)) {
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		peer_connection = nullptr;
		audio_track = nullptr;
		video_track = nullptr;
		timestamp_channel = nullptr;
		return false;
	}

	{
		std::lock_guard<std::mutex> rl(resource_mutex);
		resource_url = resourceURL;
	}
	return true;
}

void WHIPOutput::StartThread(uint64_t generation)
{
	if (!Setup(generation)) return;
	std::string resourceURL;
	if (!Connect(generation, resourceURL)) {
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		if (peer_connection)
			peer_connection->close();
		peer_connection = nullptr;
		audio_track = nullptr;
		video_track = nullptr;
		timestamp_channel = nullptr;
		return;
	}
	if (!IsActiveGeneration(generation)) { SendDelete(resourceURL, generation, "obsolete"); return; }
	do_log(LOG_INFO, "WHIPOutput: Started");
	StartP2PSignal();

	// Start node channel to ppcenter (web socket control plane)
	if (!nodeChannel && !nodeChannelConfig.url.empty()) {
		nodeChannel = std::make_unique<NodeChannelClient>(nodeChannelConfig);
		nodeChannel->SetCommandHandler([this](const NodeMsgReq &req) -> NodeMsgRspParams {
			NodeMsgRspParams rsp;
			rsp.workerType = nodeChannelConfig.workerRole;
			rsp.workerId = nodeChannelConfig.workerId;
			rsp.msgId = req.msgId;
			rsp.code = 0;
			return rsp;
		});
		nodeChannel->Start();
	}

	obs_output_begin_data_capture(output, 0);
	running = true;

#ifdef WHIP_DEGRADE_ACTIVE
	WsDegradeClient::Instance().RegisterOutput(output);
#endif
}

void WHIPOutput::SendDelete(const std::string &resourceURL, uint64_t generation, const char *reason)
{
	if (resourceURL.empty() || !IsActiveGeneration(generation)) return;
	CURL *c = curl_easy_init();
	curl_easy_setopt(c, CURLOPT_URL, resourceURL.c_str());
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	struct curl_slist *list = nullptr;
	if (!bearer_token.empty())
		list = curl_slist_append(list, ("Authorization: Bearer " + bearer_token).c_str());
	list = curl_slist_append(list, ("User-Agent: " + user_agent).c_str());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
	CURLcode res = curl_easy_perform(c);
	do_log(LOG_INFO, "WHIPOutput: SendDelete (%s) result: %d", reason, res);
	curl_slist_free_all(list);
	curl_easy_cleanup(c);
}

void WHIPOutput::StopThread(bool signal, uint64_t generation, std::string resourceURL)
{
	running = false;
	if (nodeChannel) { nodeChannel->Stop(); nodeChannel.reset(); }
	if (p2pSignal) { p2pSignal.reset(); }
	{
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		if (audio_track && audio_track->isOpen()) audio_track->close();
		if (video_track && video_track->isOpen()) video_track->close();
		if (peer_connection && peer_connection->state() != rtc::PeerConnection::State::Closed)
			peer_connection->close();
		peer_connection = nullptr;
		audio_track = nullptr;
		video_track = nullptr;
		timestamp_channel = nullptr;
	}
	if (signal) { SendDelete(resourceURL, generation, "stopping"); obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS); }
}

void WHIPOutput::ParseLinkHeader(std::string linkHeader, std::vector<rtc::IceServer> &iceServers)
{
	std::string linkString = linkHeader;
	size_t pos = 0;
	while ((pos = linkString.find('<')) != std::string::npos) {
		size_t end = linkString.find('>', pos);
		if (end == std::string::npos) break;
		std::string iceUrl = linkString.substr(pos + 1, end - pos - 1);
		std::string remaining = linkString.substr(end + 1);
		size_t relpos = remaining.find("rel=\"ice-server\"");
		if (relpos != std::string::npos) {
			rtc::IceServer server(iceUrl);
			if (remaining.find("username=\"") != std::string::npos) {
				size_t userStart = remaining.find("username=\"") + 10;
				size_t userEnd = remaining.find('"', userStart);
				server.username = remaining.substr(userStart, userEnd - userStart);
			}
			if (remaining.find("credential=\"") != std::string::npos) {
				size_t credStart = remaining.find("credential=\"") + 12;
				size_t credEnd = remaining.find('"', credStart);
				server.password = remaining.substr(credStart, credEnd - credStart);
			}
			iceServers.push_back(server);
		}
		linkString = remaining.substr(relpos != std::string::npos ? relpos : 1);
	}
}

void WHIPOutput::Send(void *data, uintptr_t size, uint64_t duration, std::shared_ptr<rtc::Track> track,
		      std::shared_ptr<rtc::RtcpSrReporter> rtcp_sr_reporter)
{
	if (!track || !track->isOpen()) return;
	std::vector<rtc::byte> sample{(rtc::byte *)data, (rtc::byte *)data + size};
	auto rtp = rtcp_sr_reporter->rtpConfig;
	rtp->timestamp = rtp->timestamp + (rtp->secondsToTimestamp(static_cast<double>(duration)) * rtp->clockRate);
	try { track->send(sample); } catch (...) {}
	total_bytes_sent += size;
}

bool WHIPOutput::IsActiveGeneration(uint64_t generation) const { return generation == active_generation.load(); }

bool WHIPOutput::StartP2PSignal()
{
	if (p2pToken.empty() || p2pSignalUrl.empty() || p2pStreamPath.empty()) return false;
	const char *videoCodec = "h264";
	const char *audioCodec = "opus";
	if (obs_output_get_video_encoder(output)) {
		const char *codec = obs_encoder_get_codec(obs_output_get_video_encoder(output));
		if (codec) videoCodec = codec;
	}
	p2pSignal = std::make_unique<P2PSignalClient>(p2pSignalUrl, p2pToken, p2pStreamPath, videoCodec, audioCodec, base_ssrc);
	p2pSignal->Start();
	qosPolicy = std::make_unique<UplinkQosPolicy>(UplinkQosConfig{});
	return true;
}

void WHIPOutput::CheckUplinkQos()
{
	auto now_ms = (int64_t)(obs_get_video_frame_time() / 1000);
	if (now_ms - lastQosCheckMs < 2000) return;
	lastQosCheckMs = now_ms;
	if (!qosPolicy || !p2pSignal) return;

	if (peer_connection && peer_connection->state() != rtc::PeerConnection::State::Connected) return;

	UplinkQosSample sample;
	if (peer_connection) {
		auto state = peer_connection->state();
		if (state == rtc::PeerConnection::State::Connected) {
			sample.rttMs = peer_connection->rtt().value_or(std::chrono::milliseconds(0)).count() / 1000.0;
		}
	}
	sample.nowMs = now_ms;

	std::vector<std::string> peerIds;
	p2pSignal->ForEachPeer([&](P2PPeer &peer) { peerIds.push_back(peer.sessionId); });
	std::reverse(peerIds.begin(), peerIds.end());

	auto decision = qosPolicy->Evaluate(sample, peerIds);
	if (decision.congested) {
		for (const auto &sid : decision.sessionsToClose)
			p2pSignal->SendClose(sid, "uplink_congested");
	}
}

// -------------------------------------------------------------------
// Disconnected grace period (configured through OBS Advanced settings; see
// disconnect_grace_sec)
// -------------------------------------------------------------------
void WHIPOutput::StartDisconnectGraceTimer(uint64_t generation)
{
	// Cancel/join any previous timer first; this always runs on the
	// PeerConnection callback thread, never on disconnect_grace_thread
	// itself, so the join below is safe.
	CancelDisconnectGraceTimer();

	{
		std::lock_guard<std::mutex> lk(disconnect_grace_mutex);
		disconnect_grace_cancel = false;
	}

	disconnect_grace_thread = std::thread([this, generation]() {
		std::unique_lock<std::mutex> lk(disconnect_grace_mutex);
		bool cancelled = disconnect_grace_cv.wait_for(lk, std::chrono::seconds(disconnect_grace_sec),
							      [this]() { return disconnect_grace_cancel; });
		lk.unlock();

		if (cancelled)
			return;
		if (!IsActiveGeneration(generation))
			return;

		do_log(LOG_INFO,
		       "PeerConnection stayed Disconnected for %ds without recovering, tearing down and reconnecting",
		       disconnect_grace_sec);
		PrepareReconnect();
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
	});
}

void WHIPOutput::PrepareReconnect()
{
	const int attempt = reconnect_attempt.fetch_add(1);
	const int delay_sec = reconnect_backoff_sec * attempt;
	obs_output_set_reconnect_delay(output, delay_sec * 1000);
	do_log(LOG_INFO, "WHIP reconnect attempt %d: waiting %ds before next session", attempt + 1, delay_sec);
}

void WHIPOutput::CancelDisconnectGraceTimer()
{
	{
		std::lock_guard<std::mutex> lk(disconnect_grace_mutex);
		disconnect_grace_cancel = true;
	}
	disconnect_grace_cv.notify_all();

	// Guard against self-join: this is called from Stop(), which the
	// timer thread itself calls once its grace period expires. Joining
	// our own thread would deadlock (or throw), so just leave it to be
	// reaped the next time this is called from a different thread.
	if (disconnect_grace_thread.joinable() && disconnect_grace_thread.get_id() != std::this_thread::get_id())
		disconnect_grace_thread.join();
}

void register_whip_output()
{
	const uint32_t base_flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE | OBS_OUTPUT_MULTI_TRACK_AV;

	const char *audio_codecs = "opus";
#ifdef ENABLE_HEVC
	const char *video_codecs = "h264;hevc;av1";
#else
	const char *video_codecs = "h264;av1";
#endif

	struct obs_output_info info = {};
	info.id = "whip_output";
	info.flags = OBS_OUTPUT_AV | base_flags;
	info.get_name = [](void *) -> const char * {
		return obs_module_text("Output.Name");
	};
	info.create = [](obs_data_t *settings, obs_output_t *output) -> void * {
		return new WHIPOutput(settings, output);
	};
	info.destroy = [](void *data) { delete static_cast<WHIPOutput *>(data); };
	info.start = [](void *data) -> bool { return static_cast<WHIPOutput *>(data)->Start(); };
	info.stop = [](void *data, uint64_t) { static_cast<WHIPOutput *>(data)->Stop(); };
	info.encoded_packet = [](void *data, struct encoder_packet *packet) { static_cast<WHIPOutput *>(data)->Data(packet); };
	info.get_defaults = [](obs_data_t *) {};
	info.get_properties = [](void *) -> obs_properties_t * { return obs_properties_create(); };
	info.get_total_bytes = [](void *data) -> uint64_t { return static_cast<WHIPOutput *>(data)->GetTotalBytes(); };
	info.get_connect_time_ms = [](void *data) -> int { return static_cast<WHIPOutput *>(data)->GetConnectTime(); };
	info.encoded_audio_codecs = audio_codecs;
	info.encoded_video_codecs = video_codecs;
	info.protocols = "WHIP";
	obs_register_output(&info);

	info.id = "whip_output_video";
	info.flags = OBS_OUTPUT_VIDEO | base_flags;
	info.encoded_audio_codecs = nullptr;
	obs_register_output(&info);

	info.id = "whip_output_audio";
	info.flags = OBS_OUTPUT_AUDIO | base_flags;
	info.encoded_video_codecs = nullptr;
	info.encoded_audio_codecs = audio_codecs;
	obs_register_output(&info);
}
