#include "whip-output.h"
#include "whip-service.h"
#include "whip-utils.h"
#include "ppcenter-client.h"
#include "ppcenter-signal.h"
#include "uplink-qos-policy.h"

#include <obs.hpp>
#include <algorithm>
#include <cstring>

#ifdef WHIP_DEGRADE_ACTIVE
#include "degrade-client.h"
#endif

namespace {

// Reads Stream1.WHIPHevcH264Multitrack, ppobs's persistent toggle for the
// HEVC/H264 multitrack feature (see the design doc's §4.1 "设置层" and the
// Settings UI checkbox that writes this same key). Not read from the
// service's own runtime obs_data_t, since - like WHIPSimulcastTotalLayers -
// it's an output/encoder-shape decision made before Setup() runs, not a
// per-connection service field.
bool multitrackEnabled(obs_output_t *output)
{
	obs_data_t *private_settings = obs_output_get_settings(output);
	bool enabled = obs_data_get_bool(private_settings, "whip_hevc_h264_multitrack");
	obs_data_release(private_settings);
	return enabled;
}

} // namespace

WHIPOutput::WHIPOutput(obs_data_t *, obs_output_t *output_) : output(output_)
{
	// Declared at output creation so the frontend can connect before
	// streaming starts; emitted per scored sample from the quality
	// scorer's decode thread (see quality-score.cpp).
	signal_handler_add(obs_output_get_signal_handler(output),
			   "void quality_score(ptr output, float score, float psnr)");
}

WHIPOutput::~WHIPOutput()
{
	if (p2pSignal)
		p2pSignal.reset();
	Stop();

	std::lock_guard<std::mutex> l(start_stop_mutex);
	if (start_stop_thread.joinable())
		start_stop_thread.join();
}

bool WHIPOutput::Start()
{
	std::lock_guard<std::mutex> l(start_stop_mutex);
	const uint64_t generation = active_generation.fetch_add(1) + 1;

	if (!obs_output_reconnecting(output))
		multitrack_permanent_failure = false;

	if (!obs_output_can_begin_data_capture(output, 0))
		return false;
	if (!obs_output_initialize_encoders(output, 0))
		return false;

	if (start_stop_thread.joinable())
		start_stop_thread.join();

	{
		obs_service_t *service = obs_output_get_service(output);
		OBSDataAutoRelease service_settings = service ? obs_service_get_settings(service) : nullptr;
		if (service_settings && obs_data_get_bool(service_settings, "quality_score"))
			quality_scorer.Start(output);
	}

	start_stop_thread = std::thread(&WHIPOutput::StartThread, this, generation);
	return true;
}

void WHIPOutput::Stop(bool signal)
{
	quality_scorer.Stop();

#ifdef WHIP_DEGRADE_ACTIVE
	WsDegradeClient::Instance().UnregisterOutput();
#endif

	std::lock_guard<std::mutex> l(start_stop_mutex);
	const uint64_t generation = active_generation.load();
	if (start_stop_thread.joinable())
		start_stop_thread.join();
	start_stop_thread = std::thread(&WHIPOutput::StopThread, this, signal, generation);
	start_stop_thread.join();
}

void WHIPOutput::Data(struct encoder_packet *packet)
{
	if (!packet) {
		Stop(false);
		obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	CheckUplinkQos();

	if (packet->type == OBS_ENCODER_AUDIO) {
		// Shared Opus output: both codec sessions send the same
		// encoded audio packet on their own independent RTP state -
		// see the design doc's §4.1 "音频不复制编码结果".
		if (h264Session)
			h264Session->SendAudio(packet);
		if (hevcSession)
			hevcSession->SendAudio(packet);

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
					try {
						peer.audioTrack->send(sample);
					} catch (...) {
					}
					peer.audioSequenceNumber = rtp->sequenceNumber;
					peer.audioRtpTimestamp = rtp->timestamp;
				}
			});
		}
	} else if (packet->type == OBS_ENCODER_VIDEO) {
		quality_scorer.OnPacket(packet);

		// Route to exactly the one session that owns this encoder -
		// H264 layers and HEVC layers are always disjoint encoder
		// sets (see Setup()).
		if (h264Session && h264Session->OwnsVideoEncoder(packet->encoder))
			h264Session->SendVideo(packet);
		else if (hevcSession && hevcSession->OwnsVideoEncoder(packet->encoder))
			hevcSession->SendVideo(packet);

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
					try {
						peer.videoTrack->send(sample);
					} catch (...) {
					}
					peer.videoSequenceNumber = rtp->sequenceNumber;
					peer.videoRtpTimestamp = rtp->timestamp;
				}
			});
		}
	}
}

bool WHIPOutput::Init()
{
	OBSDataAutoRelease output_settings = obs_output_get_settings(output);
	disconnect_grace_sec = (int)obs_data_get_int(output_settings, "whip_disconnect_grace_sec");
	reconnect_backoff_sec = (int)obs_data_get_int(output_settings, "whip_reconnect_backoff_sec");
	if (!obs_data_has_user_value(output_settings, "whip_disconnect_grace_sec"))
		disconnect_grace_sec = 5;
	if (!obs_data_has_user_value(output_settings, "whip_reconnect_backoff_sec"))
		reconnect_backoff_sec = 3;
	if (disconnect_grace_sec < 0)
		disconnect_grace_sec = 0;
	if (reconnect_backoff_sec < 0)
		reconnect_backoff_sec = 0;

	watchdog_interval_sec = (int)obs_data_get_int(output_settings, "whip_watchdog_interval_sec");
	watchdog_stall_sec = (int)obs_data_get_int(output_settings, "whip_watchdog_stall_sec");
	if (!obs_data_has_user_value(output_settings, "whip_watchdog_interval_sec"))
		watchdog_interval_sec = 10;
	if (!obs_data_has_user_value(output_settings, "whip_watchdog_stall_sec"))
		watchdog_stall_sec = 30;
	if (watchdog_interval_sec < 0)
		watchdog_interval_sec = 0;
	if (watchdog_stall_sec > 0 && watchdog_stall_sec < watchdog_interval_sec)
		watchdog_stall_sec = watchdog_interval_sec;

	enable_pacing = obs_data_get_bool(output_settings, "whip_enable_pacing");

	// Finite reconnect budget only in multitrack mode - see the design
	// doc's "双轨重连超过配置上限时，停止整组输出". A single-session (no
	// multitrack) output keeps the pre-multitrack "retry forever"
	// behavior (0 = unlimited).
	multitrack_max_reconnect_attempts = multitrackEnabled(output) ? 20 : 0;

	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}
	return true;
}

// Builds the video layer list (in rid order) for one codec from the
// output's assigned video encoders: encoders whose obs_encoder_get_codec()
// matches wantCodec, in slot order. H264 (or the sole codec, when
// multitrack is off) reads every video encoder slot 0..N; HEVC in
// multitrack mode reads its own separately-created ladder (see
// OBSBasicSettings_Stream.cpp's HEVC encoder creation, mirroring
// WHIPSimulcastEncoders) which the frontend assigns to distinct encoder
// slots from the H264 ladder.
static std::vector<obs_encoder_t *> collectVideoLayers(obs_output_t *output, const char *wantCodec)
{
	std::vector<obs_encoder_t *> layers;
	for (uint32_t idx = 0; idx < MAX_OUTPUT_VIDEO_ENCODERS; idx++) {
		auto encoder = obs_output_get_video_encoder2(output, idx);
		if (!encoder)
			break;
		const char *codec = obs_encoder_get_codec(encoder);
		if (codec && strcmp(codec, wantCodec) == 0)
			layers.push_back(encoder);
	}
	return layers;
}

bool WHIPOutput::Setup(uint64_t generation)
{
	if (!Init())
		return false;
	if (!IsActiveGeneration(generation))
		return false;

	obs_service_t *service = obs_output_get_service(output);
	if (!service) {
		obs_output_signal_stop(output, OBS_OUTPUT_ERROR);
		return false;
	}
	OBSDataAutoRelease service_settings = obs_service_get_settings(service);
	const std::string ppcenter_url = obs_data_get_string(service_settings, "ppcenter_url");
	if (ppcenter_url.empty()) {
		do_log(LOG_ERROR, "ppcenter url not configured");
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	const bool wantMultitrack = multitrackEnabled(output);

	PPCenterPublishRequest request{ppcenter_url,
				       obs_data_get_string(service_settings, "ppcenter_appid"),
				       obs_data_get_string(service_settings, "ppcenter_secret"),
				       obs_data_get_string(service_settings, "ppcenter_stream"),
				       obs_data_get_string(service_settings, "ppcenter_region"),
				       wantMultitrack};
	PPCenterPublishResponse resp;
	std::string error;
	if (!ppcenter_resolve_publish(request, resp, error)) {
		do_log(LOG_ERROR, "ppcenter resolve publish failed: %s", error.c_str());
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
		return false;
	}
	p2pToken = resp.signal_token;
	p2pSignalUrl = resp.signal_url;
	p2pStreamPath = request.app_id + "/" + request.stream_name;

	auto h264Layers = collectVideoLayers(output, "h264");
	if (h264Layers.empty()) {
		do_log(LOG_ERROR, "No H264 video encoder assigned");
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	std::string h264Url = resp.whip_url;
	std::string h264Token = resp.bearer_token;

	std::vector<obs_encoder_t *> hevcLayers;
	std::string hevcUrl, hevcToken;
	if (wantMultitrack) {
		hevcLayers = collectVideoLayers(output, "hevc");
		if (hevcLayers.empty()) {
			do_log(LOG_ERROR,
			       "HEVC/H264 multitrack is enabled but no HEVC video encoder is assigned");
			if (IsActiveGeneration(generation))
				obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
			return false;
		}
		auto h264Track = resp.whip_tracks.find("h264");
		auto hevcTrack = resp.whip_tracks.find("hevc");
		if (h264Track == resp.whip_tracks.end() || h264Track->second.url.empty() ||
		    h264Track->second.bearer_token.empty() || hevcTrack == resp.whip_tracks.end() ||
		    hevcTrack->second.url.empty() || hevcTrack->second.bearer_token.empty()) {
			// See the design doc's §3.2: an incomplete whipTracks
			// response (old ppcenter, or ppcenter rejected the
			// capability) must not silently fall back to a
			// single-codec publish.
			do_log(LOG_ERROR,
			       "HEVC/H264 multitrack requested but ppcenter did not return a complete whipTracks response");
			if (IsActiveGeneration(generation))
				obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
			return false;
		}
		h264Url = h264Track->second.url;
		h264Token = h264Track->second.bearer_token;
		hevcUrl = hevcTrack->second.url;
		hevcToken = hevcTrack->second.bearer_token;
	}

	WHIPCodecSession::Config h264Cfg;
	h264Cfg.endpoint_url = h264Url;
	h264Cfg.bearer_token = h264Token;
	h264Cfg.has_audio = true;
	h264Cfg.enable_pacing = enable_pacing;
	h264Cfg.disconnect_grace_sec = disconnect_grace_sec;
	h264Cfg.reconnect_backoff_sec = reconnect_backoff_sec;
	h264Cfg.watchdog_interval_sec = watchdog_interval_sec;
	h264Cfg.watchdog_stall_sec = watchdog_stall_sec;
	h264Cfg.max_reconnect_attempts = multitrack_max_reconnect_attempts;

	h264Session = std::make_unique<WHIPCodecSession>(output, "h264");
	h264Session->SetVideoLayers(h264Layers);
	if (!h264Session->Connect(h264Cfg)) {
		do_log(LOG_ERROR, "H264 WHIP connect failed");
		h264Session.reset();
		if (IsActiveGeneration(generation)) {
			// See whip-output's pre-multitrack PrepareReconnect()
			// comment for why this must be primed before signaling
			// DISCONNECTED: without it libobs reuses whatever
			// reconnect_retry_cur_msec last was.
			obs_output_set_reconnect_delay(output, reconnect_backoff_sec * 1000);
			obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
		}
		return false;
	}

	if (wantMultitrack) {
		WHIPCodecSession::Config hevcCfg = h264Cfg;
		hevcCfg.endpoint_url = hevcUrl;
		hevcCfg.bearer_token = hevcToken;
		// Audio already goes out on the H264 session; sending it
		// twice on the wire for no player-visible benefit would
		// double the audio bitrate for nothing, since no client ever
		// plays both codec sessions' audio at once (see the design
		// doc's §2.2 "codecType" being an exclusive choice).
		hevcCfg.has_audio = false;

		hevcSession = std::make_unique<WHIPCodecSession>(output, "hevc");
		hevcSession->SetVideoLayers(hevcLayers);
		if (!hevcSession->Connect(hevcCfg)) {
			do_log(LOG_ERROR, "HEVC WHIP connect failed - aborting H264 session too (strong-consistency start)");
			hevcSession.reset();
			h264Session->Stop();
			h264Session.reset();
			if (IsActiveGeneration(generation)) {
				obs_output_set_reconnect_delay(output, reconnect_backoff_sec * 1000);
				obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
			}
			return false;
		}
	}

	do_log(LOG_INFO, "ppcenter resolved WHIP: h264=%s hevc=%s P2P: %s", h264Url.c_str(),
	       hevcUrl.empty() ? "disabled" : hevcUrl.c_str(), p2pSignalUrl.empty() ? "disabled" : "enabled");

	return true;
}

bool WHIPOutput::IsActiveGeneration(uint64_t generation) const
{
	return generation == active_generation.load();
}

void WHIPOutput::OnSessionPermanentFailure(const std::string &codecLabel, const std::string &reason)
{
	if (multitrack_permanent_failure.exchange(true))
		return; // already handling a failure from the sibling session

	do_log(LOG_WARNING, "WHIP session '%s' permanently failed (%s) - stopping the whole output", codecLabel.c_str(),
	       reason.c_str());

	// Run the actual teardown on a detached thread: this callback runs
	// on the failing session's own worker thread, and Stop() on the
	// *sibling* session blocks joining that sibling's worker thread -
	// safe in general, but must not run inline on a WHIPCodecSession
	// worker thread that a caller might be about to join from
	// elsewhere. obs_output_signal_stop() itself is safe to call from
	// any thread.
	std::thread([this]() {
		if (h264Session)
			h264Session->Stop();
		if (hevcSession)
			hevcSession->Stop();
		obs_output_set_reconnect_delay(output, reconnect_backoff_sec * 1000);
		obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
	}).detach();
}

void WHIPOutput::StartThread(uint64_t generation)
{
	if (!Setup(generation))
		return;
	if (!IsActiveGeneration(generation)) {
		if (h264Session) {
			h264Session->Stop();
			h264Session.reset();
		}
		if (hevcSession) {
			hevcSession->Stop();
			hevcSession.reset();
		}
		return;
	}

	do_log(LOG_INFO, "WHIPOutput: Started (%s)", hevcSession ? "H264+HEVC multitrack" : "H264 only");
	StartP2PSignal();

	WHIPCodecSession::Callbacks cb;
	cb.onPermanentFailure = [this](const std::string &reason) { OnSessionPermanentFailure("h264", reason); };
	h264Session->Supervise(cb);
	if (hevcSession) {
		WHIPCodecSession::Callbacks hevcCb;
		hevcCb.onPermanentFailure = [this](const std::string &reason) {
			OnSessionPermanentFailure("hevc", reason);
		};
		hevcSession->Supervise(hevcCb);
	}

	obs_output_begin_data_capture(output, 0);
	running = true;

#ifdef WHIP_DEGRADE_ACTIVE
	// Degrade control channel is derived from the H264 endpoint only -
	// it has never modeled per-codec sessions, and layer degrade
	// commands apply to the H264 simulcast ladder regardless of
	// whether HEVC multitrack is also running.
	WsDegradeClient::Instance().RegisterOutput(output, "");
#endif
}

void WHIPOutput::StopThread(bool signal, uint64_t generation)
{
	running = false;
	if (p2pSignal)
		p2pSignal.reset();

	if (h264Session) {
		h264Session->Stop();
		h264Session.reset();
	}
	if (hevcSession) {
		hevcSession->Stop();
		hevcSession.reset();
	}

	if (signal && IsActiveGeneration(generation))
		obs_output_signal_stop(output, OBS_OUTPUT_SUCCESS);
}

bool WHIPOutput::StartP2PSignal()
{
	if (p2pToken.empty() || p2pSignalUrl.empty() || p2pStreamPath.empty())
		return false;
	// P2P always advertises H264 - see this class's header comment: it
	// mirrors the pre-multitrack single-session behavior and is
	// unrelated to whether HEVC multitrack is also running.
	const char *videoCodec = "h264";
	const char *audioCodec = "opus";
	p2pSignal = std::make_unique<P2PSignalClient>(p2pSignalUrl, p2pToken, p2pStreamPath, videoCodec, audioCodec,
						      generate_random_u32());
	p2pSignal->Start();
	qosPolicy = std::make_unique<UplinkQosPolicy>(UplinkQosConfig{});
	return true;
}

void WHIPOutput::CheckUplinkQos()
{
	auto now_ms = (int64_t)(obs_get_video_frame_time() / 1000);
	if (now_ms - lastQosCheckMs < 2000)
		return;
	lastQosCheckMs = now_ms;
	if (!qosPolicy || !p2pSignal)
		return;

	UplinkQosSample sample;
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
	info.get_name = [](void *) -> const char * { return obs_module_text("Output.Name"); };
	info.create = [](obs_data_t *settings, obs_output_t *output) -> void * { return new WHIPOutput(settings, output); };
	info.destroy = [](void *data) { delete static_cast<WHIPOutput *>(data); };
	info.start = [](void *data) -> bool { return static_cast<WHIPOutput *>(data)->Start(); };
	info.stop = [](void *data, uint64_t) { static_cast<WHIPOutput *>(data)->Stop(); };
	info.encoded_packet = [](void *data, struct encoder_packet *packet) {
		static_cast<WHIPOutput *>(data)->Data(packet);
	};
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
