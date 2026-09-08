#include "whip-codec-session.h"
#include "whip-utils.h"

#include <obs.hpp>
#include <util/dstr.h>
#include <util/ntp-clock.h>

#include <algorithm>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace {

constexpr uint16_t MAX_VIDEO_FRAGMENT_SIZE = 1200;
constexpr int signaling_media_id_length = 16;
constexpr char signaling_media_id_valid_char[] = "0123456789"
						  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
						  "abcdefghijklmnopqrstuvwxyz";

const char *audio_mid = "0";
constexpr uint8_t audio_payload_type = 111;
const char *video_mid = "1";
constexpr uint8_t video_payload_type = 96;
constexpr int video_nack_buffer_size = 4000;

const std::string rtpHeaderExtUriMid = "urn:ietf:params:rtp-hdrext:sdes:mid";
const std::string rtpHeaderExtUriRid = "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id";

size_t curl_write_body(char *data, size_t size, size_t nmemb, void *priv)
{
	auto *buf = static_cast<std::string *>(priv);
	buf->append(data, size * nmemb);
	return size * nmemb;
}

size_t curl_write_headers(char *data, size_t size, size_t nmemb, void *priv)
{
	auto *buf = static_cast<std::vector<std::string> *>(priv);
	buf->push_back(trim_string(std::string(data, size * nmemb)));
	return size * nmemb;
}

} // namespace

// whip-utils.h's do_log bakes in "output" as the variable name, which this
// class also has (so the macro itself works unmodified) but its format
// string has no room for this session's codec label - redefine it here to
// splice that in, matching every do_log call below.
#undef do_log
#define do_log(level, format, ...) \
	blog(level, "[obs-webrtc] [whip_output: '%s'] [%s] " format, obs_output_get_name(output), label.c_str(), \
	     ##__VA_ARGS__)

WHIPCodecSession::WHIPCodecSession(obs_output_t *output_, std::string label_)
	: output(output_),
	  label(std::move(label_)),
	  base_ssrc(generate_random_u32())
{
}

WHIPCodecSession::~WHIPCodecSession()
{
	Stop();
}

void WHIPCodecSession::SetVideoLayers(const std::vector<obs_encoder_t *> &layers)
{
	video_layers = layers;
	videoLayerStates.clear();
	for (size_t idx = 0; idx < video_layers.size(); idx++) {
		auto encoder = video_layers[idx];
		if (!encoder)
			continue;
		auto v = std::make_shared<videoLayerState>();
		v->ssrc = base_ssrc + 1 + static_cast<uint32_t>(idx);
		v->rid = std::to_string(idx);
		videoLayerStates[encoder] = v;
	}
	if (!video_layers.empty() && video_layers[0]) {
		const char *codec = obs_encoder_get_codec(video_layers[0]);
		video_codec = codec ? codec : "";
	}
}

bool WHIPCodecSession::OwnsVideoEncoder(obs_encoder_t *encoder) const
{
	return videoLayerStates.find(encoder) != videoLayerStates.end();
}

bool WHIPCodecSession::Connect(const Config &config)
{
	cfg = config;
	stop_requested = false;
	reconnect_attempt = 0;
	pending_pc_event = PCEvent::None;

	const uint64_t generation = active_generation.fetch_add(1) + 1;
	std::string resourceURL;
	last_failure_permanent = false;
	if (!DoConnect(generation, resourceURL)) {
		TeardownGeneration(generation, resourceURL, "connect-failed", /*wasConnected=*/false);
		return false;
	}

	sending_enabled = true;
	connected_unsupervised = true;
	resourceURL_unsupervised = resourceURL;
	do_log(LOG_INFO, "WHIPCodecSession: first connect succeeded");
	return true;
}

void WHIPCodecSession::Supervise(Callbacks cb)
{
	callbacks = std::move(cb);
	connected_unsupervised = false;

	std::lock_guard<std::mutex> l(start_stop_mutex);
	worker_thread = std::thread(&WHIPCodecSession::WorkerLoop, this, active_generation.load());
}

void WHIPCodecSession::Abort()
{
	if (!connected_unsupervised.load())
		return;
	const uint64_t generation = active_generation.load();
	std::string resourceURL = resourceURL_unsupervised;
	connected_unsupervised = false;
	TeardownGeneration(generation, resourceURL, "aborted", /*wasConnected=*/true);
}

void WHIPCodecSession::Stop()
{
	{
		std::lock_guard<std::mutex> lk(cv_mutex);
		stop_requested = true;
	}
	cv.notify_all();

	std::lock_guard<std::mutex> l(start_stop_mutex);
	if (worker_thread.joinable() && worker_thread.get_id() != std::this_thread::get_id())
		worker_thread.join();

	// Supervise() was never called after a successful Connect() (e.g.
	// the sibling session's first connect failed and the whole output
	// aborted) - tear down the still-live unsupervised connection too.
	Abort();
}

void WHIPCodecSession::SendAudio(struct encoder_packet *packet)
{
	std::shared_lock<std::shared_mutex> lk(tracks_mutex);
	if (!sending_enabled.load())
		return;
	std::shared_ptr<rtc::Track> local_audio_track = audio_track;
	std::shared_ptr<rtc::RtcpSrReporter> local_audio_sr_reporter = audio_sr_reporter;
	if (!local_audio_track)
		return;

	int64_t duration = packet->dts_usec - last_audio_timestamp;
	Send(packet->data, packet->size, duration, local_audio_track, local_audio_sr_reporter);
	last_audio_timestamp = packet->dts_usec;
}

void WHIPCodecSession::SendVideo(struct encoder_packet *packet)
{
	std::shared_lock<std::shared_mutex> lk(tracks_mutex);
	if (!sending_enabled.load())
		return;
	std::shared_ptr<rtc::Track> local_video_track = video_track;
	std::shared_ptr<rtc::DataChannel> local_timestamp_channel = timestamp_channel;
	std::shared_ptr<rtc::RtcpSrReporter> local_video_sr_reporter = video_sr_reporter;
	if (!local_video_track || !local_video_sr_reporter)
		return;

	auto rtp_config = local_video_sr_reporter->rtpConfig;
	auto it = videoLayerStates.find(packet->encoder);
	if (it == videoLayerStates.end()) {
		// Stale encoder packet in flight after reconnect: SetVideoLayers()
		// rebuilt the map with new encoders, but old encoder packets can
		// still arrive. Drop silently instead of killing the stream -
		// mirrors the pre-multitrack WHIPOutput::Data().
		do_log(LOG_DEBUG,
		       "SendVideo() packet encoder=%p not found in videoLayerStates (size=%zu) - dropping stale packet",
		       (void *)packet->encoder, videoLayerStates.size());
		return;
	}
	auto videoLayerState = it->second;

	rtp_config->sequenceNumber = videoLayerState->sequenceNumber;
	rtp_config->ssrc = videoLayerState->ssrc;
	rtp_config->rid = videoLayerState->rid;
	rtp_config->timestamp = videoLayerState->rtpTimestamp;
	int64_t duration = packet->dts_usec - videoLayerState->lastVideoTimestamp;

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
}

void WHIPCodecSession::ConfigureAudioTrack(const std::string &media_stream_id, const std::string &cname)
{
	if (!cfg.has_audio)
		return;
	uint32_t ssrc = base_ssrc;
	auto media_stream_track_id = media_stream_id + "-audio";
	rtc::Description::Audio audio_description(audio_mid, rtc::Description::Direction::SendOnly);
	audio_description.addOpusCodec(audio_payload_type);
	audio_description.addSSRC(ssrc, cname, media_stream_id, media_stream_track_id);
	auto new_audio_track = peer_connection->addTrack(audio_description);
	auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, audio_payload_type,
									 rtc::OpusRtpPacketizer::DefaultClockRate);
	auto new_audio_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);
	packetizer->addToChain(new_audio_sr_reporter);
	packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
	new_audio_track->setMediaHandler(packetizer);

	std::unique_lock<std::shared_mutex> lk(tracks_mutex);
	audio_track = new_audio_track;
	audio_sr_reporter = new_audio_sr_reporter;
}

void WHIPCodecSession::ConfigureVideoTrack(const std::string &media_stream_id, const std::string &cname)
{
	if (video_layers.empty() || !video_layers[0])
		return;

	std::shared_ptr<rtc::RtpPacketizer> packetizer;
	uint32_t ssrc = base_ssrc + 1;
	auto media_stream_track_id = media_stream_id + "-video";
	rtc::Description::Video video_description(video_mid, rtc::Description::Direction::SendOnly);
	video_description.addSSRC(ssrc, cname, media_stream_id, media_stream_track_id);
	video_description.addExtMap(rtc::Description::Entry::ExtMap(1, rtpHeaderExtUriMid));
	video_description.addExtMap(rtc::Description::Entry::ExtMap(2, rtpHeaderExtUriRid));
	if (videoLayerStates.size() >= 2) {
		std::vector<std::pair<int, std::string>> sortedRids;
		for (const auto &[encoder, state] : videoLayerStates)
			sortedRids.push_back({std::stoi(state->rid), state->rid});
		std::sort(sortedRids.begin(), sortedRids.end(),
			  [](const auto &a, const auto &b) { return a.first < b.first; });
		for (const auto &[_, rid] : sortedRids)
			video_description.addRid(rid);
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

	if (strcmp("h264", video_codec.c_str()) == 0) {
		video_description.addH264Codec(video_payload_type);
		packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::H264RtpPacketizer::Separator::StartSequence,
								       rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
#ifdef ENABLE_HEVC
	} else if (strcmp("hevc", video_codec.c_str()) == 0) {
		video_description.addH265Codec(video_payload_type);
		packetizer = std::make_shared<rtc::H265RtpPacketizer>(rtc::H265RtpPacketizer::Separator::StartSequence,
								       rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
#endif
	} else if (strcmp("av1", video_codec.c_str()) == 0) {
		video_description.addAV1Codec(video_payload_type);
		packetizer = std::make_shared<rtc::AV1RtpPacketizer>(rtc::AV1RtpPacketizer::Packetization::TemporalUnit,
								     rtp_config, MAX_VIDEO_FRAGMENT_SIZE);
	} else {
		do_log(LOG_ERROR, "Video codec not supported: %s", video_codec.c_str());
		return;
	}
	auto new_video_sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
	packetizer->addToChain(new_video_sr_reporter);
	packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(video_nack_buffer_size));

	if (cfg.enable_pacing) {
		int64_t total_bitrate_kbps = 0;
		for (auto *layer : video_layers) {
			if (!layer)
				continue;
			OBSDataAutoRelease layer_settings = obs_encoder_get_settings(layer);
			total_bitrate_kbps += obs_data_get_int(layer_settings, "bitrate");
		}
		if (total_bitrate_kbps > 0) {
			const double pacing_bits_per_sec = static_cast<double>(total_bitrate_kbps) * 1000.0 * 1.5;
			do_log(LOG_INFO, "Pacing video track at %.1f Mbit/s (%lld kbps across all layers + 50%% headroom)",
			       pacing_bits_per_sec / 1000000.0, (long long)total_bitrate_kbps);
			packetizer->addToChain(
				std::make_shared<rtc::PacingHandler>(pacing_bits_per_sec, std::chrono::milliseconds(5)));
		}
	}

	auto new_video_track = peer_connection->addTrack(video_description);
	new_video_track->setMediaHandler(packetizer);

	std::unique_lock<std::shared_mutex> lk(tracks_mutex);
	video_track = new_video_track;
	video_sr_reporter = new_video_sr_reporter;
}

bool WHIPCodecSession::DoConnect(uint64_t generation, std::string &resourceURL)
{
	rtc::Configuration rtcConfig;
	std::vector<rtc::IceServer> iceServers;
	iceServers.emplace_back("stun:stun.l.google.com:19302");

#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR > 20 || RTC_VERSION_MAJOR > 0
	rtcConfig.disableAutoGathering = true;
#endif

	std::string media_stream_id;
	media_stream_id.reserve(signaling_media_id_length);
	for (int i = 0; i < signaling_media_id_length; i++)
		media_stream_id += signaling_media_id_valid_char[rand() % (sizeof(signaling_media_id_valid_char) - 1)];
	auto cname = "obs-studio-webrtc-output";

	peer_connection = std::make_shared<rtc::PeerConnection>(rtcConfig);

	peer_connection->onStateChange([this, generation](rtc::PeerConnection::State state) {
		if (!IsActiveGeneration(generation)) {
			do_log(LOG_DEBUG, "[generation=%llu] ignoring stale PeerConnection state change: %d",
			       static_cast<unsigned long long>(generation), static_cast<int>(state));
			return;
		}

		PCEvent evt = PCEvent::None;
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
			evt = PCEvent::Connected;
			break;
		case rtc::PeerConnection::State::Disconnected:
			do_log(LOG_INFO,
			       "PeerConnection state is now: Disconnected - waiting up to %ds for it to recover before tearing down",
			       cfg.disconnect_grace_sec);
			evt = PCEvent::Disconnected;
			break;
		case rtc::PeerConnection::State::Failed:
			do_log(LOG_INFO, "PeerConnection state is now: Failed");
			evt = PCEvent::Failed;
			break;
		case rtc::PeerConnection::State::Closed:
			do_log(LOG_INFO, "PeerConnection state is now: Closed");
			// A remote close can leave application-side tracks looking
			// open while libdatachannel is destroying their transport.
			// Invalidate them immediately so encoder threads cannot
			// enter send() during that teardown window.
			sending_enabled = false;
			{
				std::unique_lock<std::shared_mutex> lk(tracks_mutex);
				peer_connection = nullptr;
				audio_track = nullptr;
				video_track = nullptr;
				timestamp_channel = nullptr;
				audio_sr_reporter = nullptr;
				video_sr_reporter = nullptr;
			}
			evt = PCEvent::Closed;
			break;
		}

		if (evt != PCEvent::None) {
			{
				std::lock_guard<std::mutex> lk(cv_mutex);
				pending_pc_event = evt;
			}
			cv.notify_all();
		}
	});

	ConfigureAudioTrack(media_stream_id, cname);
	ConfigureVideoTrack(media_stream_id, cname);

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
	if (!video_track && !audio_track)
		return false;

	std::string sdp = localDescription.generateSdp();

	CURL *c = curl_easy_init();
	std::string body;
	std::vector<std::string> headers;
	char error_buffer[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(c, CURLOPT_URL, cfg.endpoint_url.c_str());
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)sdp.length());
	curl_easy_setopt(c, CURLOPT_COPYPOSTFIELDS, sdp.c_str());
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_body);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_write_headers);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 1L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error_buffer);

	struct curl_slist *headerList = nullptr;
	headerList = curl_slist_append(headerList, "Content-Type: application/sdp");
	if (!cfg.bearer_token.empty())
		headerList = curl_slist_append(headerList, ("Authorization: Bearer " + cfg.bearer_token).c_str());
	headerList = curl_slist_append(headerList, ("User-Agent: " + generate_user_agent()).c_str());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headerList);

	auto cleanupCurl = [&]() {
		curl_slist_free_all(headerList);
		curl_easy_cleanup(c);
	};

	CURLcode res = curl_easy_perform(c);
	if (res != CURLE_OK) {
		do_log(LOG_ERROR, "WHIP request failed: %s", error_buffer[0] ? error_buffer : curl_easy_strerror(res));
		cleanupCurl();
		return false;
	}

	long http_code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 201) {
		do_log(LOG_ERROR, "WHIP request failed: HTTP %ld", http_code);
		cleanupCurl();
		last_failure_permanent =
			(http_code == 401 || http_code == 403 || http_code == 404 || http_code == 406);
		return false;
	}

	if (body.empty()) {
		do_log(LOG_ERROR, "WHIP request failed: no SDP answer returned from the endpoint");
		cleanupCurl();
		return false;
	}

	long redirect_count = 0;
	curl_easy_getinfo(c, CURLINFO_REDIRECT_COUNT, &redirect_count);
	std::string lastLocation;
	size_t locationCount = 0;
	for (const auto &h : headers) {
		auto value = value_for_header("location", h);
		if (value.empty())
			continue;
		locationCount++;
		lastLocation = value;
	}
	if (locationCount < static_cast<size_t>(redirect_count) + 1) {
		do_log(LOG_ERROR, "WHIP server did not provide a resource URL via the Location header");
		cleanupCurl();
		return false;
	}

	for (const auto &h : headers) {
		auto value = value_for_header("link", h);
		if (value.empty())
			continue;
		for (auto end = value.find(","); end != std::string::npos; end = value.find(",")) {
			ParseLinkHeader(value.substr(0, end), iceServers);
			value = value.substr(end + 1);
		}
		ParseLinkHeader(value, iceServers);
	}

	CURLU *url_builder = curl_url();
	if (lastLocation.find("http") != 0) {
		char *effective_url = nullptr;
		curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &effective_url);
		if (!effective_url) {
			do_log(LOG_ERROR, "Failed to build WHIP resource URL");
			curl_url_cleanup(url_builder);
			cleanupCurl();
			return false;
		}
		curl_url_set(url_builder, CURLUPART_URL, effective_url, 0);
		curl_url_set(url_builder, CURLUPART_PATH, lastLocation.c_str(), 0);
		curl_url_set(url_builder, CURLUPART_QUERY, "", 0);
	} else {
		curl_url_set(url_builder, CURLUPART_URL, lastLocation.c_str(), 0);
	}

	char *builtUrl = nullptr;
	CURLUcode urc = curl_url_get(url_builder, CURLUPART_URL, &builtUrl, CURLU_NO_DEFAULT_PORT);
	if (urc) {
		do_log(LOG_ERROR, "WHIP server provided an invalid resource URL via the Location header");
		curl_url_cleanup(url_builder);
		cleanupCurl();
		return false;
	}
	resourceURL = builtUrl;
	curl_free(builtUrl);
	curl_url_cleanup(url_builder);
	cleanupCurl();
	do_log(LOG_DEBUG, "[generation=%llu] WHIP Resource URL is: %s", static_cast<unsigned long long>(generation),
	       resourceURL.c_str());

	auto answerSdp = body;
	answerSdp.erase(0, answerSdp.find("v=0"));

	if (videoLayerStates.size() != 1) {
		auto layersAccepted = simulcast_layers_in_answer(answerSdp);
		if (videoLayerStates.size() != layersAccepted) {
			do_log(LOG_ERROR, "WHIP server only accepted %zu of %zu simulcast layers", layersAccepted,
			       videoLayerStates.size());
			SendDeleteResource(resourceURL, generation, "simulcast-rejected");
			return false;
		}
	}

	try {
		peer_connection->setRemoteDescription(rtc::Description(answerSdp, "answer"));
	} catch (const std::invalid_argument &err) {
		do_log(LOG_ERROR, "WHIP server responded with invalid SDP: %s", err.what());
		SendDeleteResource(resourceURL, generation, "invalid-sdp");
		return false;
	} catch (const std::exception &err) {
		do_log(LOG_ERROR, "Failed to set remote description: %s", err.what());
		SendDeleteResource(resourceURL, generation, "no-remote-description");
		return false;
	}

	if (!IsActiveGeneration(generation)) {
		SendDeleteResource(resourceURL, generation, "stale-after-answer");
		std::shared_ptr<rtc::PeerConnection> closing;
		{
			std::unique_lock<std::shared_mutex> lk(tracks_mutex);
			closing = peer_connection;
			peer_connection = nullptr;
			audio_track = nullptr;
			video_track = nullptr;
			timestamp_channel = nullptr;
		}
		ClosePeerConnectionWithTimeout(closing);
		return false;
	}

	{
		std::lock_guard<std::mutex> rl(resource_mutex);
		resource_url = resourceURL;
	}

#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR > 20 || RTC_VERSION_MAJOR > 0
	peer_connection->gatherLocalCandidates(iceServers);
#endif
	return true;
}

void WHIPCodecSession::TeardownGeneration(uint64_t generation, const std::string &resourceURL, const char *reason,
					  bool wasConnected)
{
	sending_enabled = false;
	std::shared_ptr<rtc::PeerConnection> closing;
	{
		std::unique_lock<std::shared_mutex> lk(tracks_mutex);
		if (audio_track && audio_track->isOpen())
			audio_track->close();
		if (video_track && video_track->isOpen())
			video_track->close();
		closing = peer_connection;
		peer_connection = nullptr;
		audio_track = nullptr;
		video_track = nullptr;
		timestamp_channel = nullptr;
		audio_sr_reporter = nullptr;
		video_sr_reporter = nullptr;
	}
	ClosePeerConnectionWithTimeout(closing);
	if (wasConnected)
		SendDeleteResource(resourceURL, generation, reason);
}

// Blocks until the live connection should be torn down (or Stop() was
// called), returning a short reason string. Polls the watchdog on
// cfg.watchdog_interval_sec (or every 1s if the watchdog is disabled, just
// to notice stop_requested/pc events promptly) and reacts to whichever
// PeerConnection state event DoConnect()'s callback posted in the meantime.
const char *WHIPCodecSession::RunUntilDisconnect(uint64_t generation)
{
	size_t last_bytes = total_bytes_sent.load();
	int64_t last_progress_ns = os_gettime_ns();
	const auto pollInterval =
		std::chrono::seconds(cfg.watchdog_interval_sec > 0 ? cfg.watchdog_interval_sec : 1);

	for (;;) {
		PCEvent evt;
		{
			std::unique_lock<std::mutex> lk(cv_mutex);
			cv.wait_for(lk, pollInterval,
				    [this]() { return stop_requested.load() || pending_pc_event.load() != PCEvent::None; });
			if (stop_requested.load())
				return "stopping";
			evt = pending_pc_event.exchange(PCEvent::None);
		}

		if (!IsActiveGeneration(generation))
			return "superseded";

		if (evt == PCEvent::Failed)
			return "failed";
		if (evt == PCEvent::Closed)
			return "closed";
		if (evt == PCEvent::Disconnected) {
			// Grace period: most Disconnected states self-recover
			// within a few seconds (see whip-codec-session.h's
			// class comment on why libdatachannel reports this
			// eagerly). Wait for either a Connected/Failed/Closed
			// event or the grace deadline.
			const auto deadline =
				std::chrono::steady_clock::now() + std::chrono::seconds(cfg.disconnect_grace_sec);
			for (;;) {
				std::unique_lock<std::mutex> lk(cv_mutex);
				bool woke = cv.wait_until(lk, deadline, [this]() {
					return stop_requested.load() || pending_pc_event.load() != PCEvent::None;
				});
				if (stop_requested.load())
					return "stopping";
				if (!woke)
					return "disconnect-grace-expired";
				PCEvent graceEvt = pending_pc_event.exchange(PCEvent::None);
				lk.unlock();
				if (graceEvt == PCEvent::Connected)
					break; // recovered - resume the outer polling loop
				if (graceEvt == PCEvent::Failed)
					return "failed";
				if (graceEvt == PCEvent::Closed)
					return "closed";
				// PCEvent::None or another Disconnected: keep waiting
				// out the same grace deadline.
			}
			last_bytes = total_bytes_sent.load();
			last_progress_ns = os_gettime_ns();
			continue;
		}

		// evt is Connected (redundant/no-op here) or None (plain
		// watchdog tick) - either way, check stall progress.
		const size_t bytes = total_bytes_sent.load();
		const int64_t now_ns = os_gettime_ns();
		if (bytes != last_bytes) {
			last_bytes = bytes;
			last_progress_ns = now_ns;
		}
		const double stalled_sec = (double)(now_ns - last_progress_ns) / 1e9;
		if (cfg.watchdog_stall_sec > 0 && stalled_sec >= (double)cfg.watchdog_stall_sec) {
			do_log(LOG_WARNING,
			       "Watchdog: no bytes sent for %.0fs (threshold %ds) - treating session as dead, tearing down and reconnecting",
			       stalled_sec, cfg.watchdog_stall_sec);
			return "watchdog-stall";
		}
	}
}

bool WHIPCodecSession::WaitBackoff()
{
	const int attempt = reconnect_attempt.load();
	const int delay_sec = cfg.reconnect_backoff_sec * std::max(attempt, 1);
	do_log(LOG_INFO, "WHIP reconnect attempt %d: waiting %ds before next session", attempt, delay_sec);

	std::unique_lock<std::mutex> lk(cv_mutex);
	cv.wait_for(lk, std::chrono::seconds(delay_sec), [this]() { return stop_requested.load(); });
	return !stop_requested.load();
}

bool WHIPCodecSession::ReconnectOnce(const char *disconnectReason)
{
	// Loops (rather than recurses) across repeated connect failures so an
	// extended outage with max_reconnect_attempts == 0 (retry forever)
	// can't grow the call stack without bound.
	std::string reason = disconnectReason;
	for (;;) {
		reconnect_attempt.fetch_add(1);
		if (cfg.max_reconnect_attempts > 0 && reconnect_attempt.load() > cfg.max_reconnect_attempts) {
			do_log(LOG_WARNING, "WHIP reconnect budget exhausted (%d attempts) after %s",
			       cfg.max_reconnect_attempts, reason.c_str());
			if (callbacks.onPermanentFailure)
				callbacks.onPermanentFailure(reason);
			return false;
		}
		if (!WaitBackoff())
			return false;

		const uint64_t generation = active_generation.fetch_add(1) + 1;
		last_failure_permanent = false;
		std::string resourceURL;
		if (DoConnect(generation, resourceURL)) {
			do_log(LOG_INFO, "WHIPCodecSession: reconnected");
			sending_enabled = true;
			return true;
		}

		TeardownGeneration(generation, resourceURL, "reconnect-failed", /*wasConnected=*/false);
		if (stop_requested.load())
			return false;
		if (last_failure_permanent.load()) {
			do_log(LOG_WARNING, "WHIP reconnect permanently rejected, not retrying");
			if (callbacks.onPermanentFailure)
				callbacks.onPermanentFailure("whip-rejected");
			return false;
		}
		reason = "connect-failed";
	}
}

void WHIPCodecSession::WorkerLoop(uint64_t generation)
{
	for (;;) {
		const char *reason = RunUntilDisconnect(generation);

		if (strcmp(reason, "superseded") == 0)
			return; // a newer generation's own worker owns things from here

		std::string resourceURL;
		{
			std::lock_guard<std::mutex> rl(resource_mutex);
			resourceURL = resource_url;
		}
		TeardownGeneration(generation, resourceURL, reason, /*wasConnected=*/true);

		if (strcmp(reason, "stopping") == 0)
			return;

		if (!ReconnectOnce(reason))
			return;
		generation = active_generation.load();
	}
}

void WHIPCodecSession::SendDeleteResource(const std::string &resourceURL, uint64_t generation, const char *reason)
{
	if (resourceURL.empty())
		return;
	CURL *c = curl_easy_init();
	char error_buffer[CURL_ERROR_SIZE] = {};
	curl_easy_setopt(c, CURLOPT_URL, resourceURL.c_str());
	curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error_buffer);
	struct curl_slist *list = nullptr;
	if (!cfg.bearer_token.empty())
		list = curl_slist_append(list, ("Authorization: Bearer " + cfg.bearer_token).c_str());
	list = curl_slist_append(list, ("User-Agent: " + generate_user_agent()).c_str());
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);

	CURLcode res = curl_easy_perform(c);
	long response_code = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response_code);
	curl_slist_free_all(list);
	curl_easy_cleanup(c);

	if (res != CURLE_OK) {
		do_log(LOG_WARNING, "[generation=%llu] DELETE for resource URL failed (%s): %s",
		       static_cast<unsigned long long>(generation), reason ? reason : "unknown",
		       error_buffer[0] ? error_buffer : curl_easy_strerror(res));
		return;
	}
	if (response_code != 200) {
		do_log(LOG_WARNING, "[generation=%llu] DELETE for resource URL failed (%s). HTTP Code: %ld",
		       static_cast<unsigned long long>(generation), reason ? reason : "unknown", response_code);
		return;
	}

	do_log(LOG_DEBUG, "[generation=%llu] DELETE for resource URL succeeded (%s)",
	       static_cast<unsigned long long>(generation), reason ? reason : "unknown");
	if (IsActiveGeneration(generation)) {
		std::lock_guard<std::mutex> rl(resource_mutex);
		if (resource_url == resourceURL)
			resource_url.clear();
	}
}

void WHIPCodecSession::ParseLinkHeader(std::string linkHeader, std::vector<rtc::IceServer> &iceServers)
{
	std::string linkString = linkHeader;
	size_t pos = 0;
	while ((pos = linkString.find('<')) != std::string::npos) {
		size_t end = linkString.find('>', pos);
		if (end == std::string::npos)
			break;
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

void WHIPCodecSession::Send(const void *data, uintptr_t size, uint64_t duration, std::shared_ptr<rtc::Track> track,
			    std::shared_ptr<rtc::RtcpSrReporter> rtcp_sr_reporter)
{
	if (!track || !track->isOpen())
		return;
	std::vector<rtc::byte> sample{(const rtc::byte *)data, (const rtc::byte *)data + size};
	auto rtp = rtcp_sr_reporter->rtpConfig;

	auto elapsed_seconds = static_cast<double>(duration) / (1000.0 * 1000.0);
	uint32_t elapsed_timestamp = rtp->secondsToTimestamp(elapsed_seconds);
	rtp->timestamp = rtp->timestamp + elapsed_timestamp;

#if RTC_VERSION_MAJOR == 0 && RTC_VERSION_MINOR < 23
	auto report_elapsed_timestamp = rtp->timestamp - rtcp_sr_reporter->lastReportedTimestamp();
	if (rtp->timestampToSeconds(report_elapsed_timestamp) > 1)
		rtcp_sr_reporter->setNeedsToReport();
#endif

	try {
		track->send(sample);
		total_bytes_sent += sample.size();
	} catch (const std::exception &e) {
		do_log(LOG_ERROR, "error: %s", e.what());
	}
}

bool WHIPCodecSession::IsActiveGeneration(uint64_t generation) const
{
	return generation == active_generation.load();
}

void WHIPCodecSession::ClosePeerConnectionWithTimeout(std::shared_ptr<rtc::PeerConnection> pc)
{
	if (!pc)
		return;

	auto done = std::make_shared<std::promise<void>>();
	auto done_future = done->get_future();
	std::string name = obs_output_get_name(output);
	std::string sessionLabel = label;

	std::thread([pc, done, name, sessionLabel]() {
		try {
			pc->close();
		} catch (const std::exception &e) {
			blog(LOG_WARNING, "[obs-webrtc] [whip_output: '%s'] [%s] PeerConnection close() threw: %s",
			     name.c_str(), sessionLabel.c_str(), e.what());
		} catch (...) {
			blog(LOG_WARNING,
			     "[obs-webrtc] [whip_output: '%s'] [%s] PeerConnection close() threw an unknown exception",
			     name.c_str(), sessionLabel.c_str());
		}
		done->set_value();
	}).detach();

	if (done_future.wait_for(std::chrono::seconds(close_timeout_sec)) == std::future_status::timeout) {
		do_log(LOG_WARNING,
		       "PeerConnection close() did not finish within %ds - abandoning it and continuing teardown",
		       close_timeout_sec);
	}
}
