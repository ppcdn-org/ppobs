#include "whip-output.h"
#include "whip-service.h"
#include "whip-utils.h"
#include "ppcenter-client.h"
#include "ppcenter-signal.h"
#include "nat-probe.h"
#include "device-registration.h"
#include "encoder-report.h"
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
	CheckNatProbeRefresh();

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
// matches wantCodec, sorted highest-resolution-first. H264 (or the sole
// codec, when multitrack is off) reads every video encoder slot 0..N; HEVC
// in multitrack mode reads its own separately-created ladder (see
// OBSBasicSettings_Stream.cpp's HEVC encoder creation, mirroring
// WHIPSimulcastEncoders) which the frontend assigns to distinct encoder
// slots from the H264 ladder.
//
// Collected in slot order first, then explicitly re-sorted by actual pixel
// area (descending) rather than trusting slot order to already be quality
// order: AdvancedOutput.cpp/SimpleOutput.cpp's SetupOutputs() cannot always
// put a codec's full-resolution base encoder in the slot right before its
// scaled layers. When H264+HEVC multitrack is on and the user's Stream
// Encoder (slot 0) is HEVC, whipH264Base has nowhere to go but *after* both
// the H264 Simulcast ladder and the HEVC ladder ("Placed after the
// Simulcast/HEVC ladders so it cannot overwrite one of their slots" - see
// the baseSlot comment there) - so by pure slot order, H264's base ends up
// last, not first, and every downstream consumer that assigns rid=index
// into this vector (WHIPCodecSession::SetVideoLayers) would label the
// highest-quality layer as the lowest-priority rid instead of rid 0. This
// was confirmed in production: mmx's receive-side pointer tracing showed
// rid "0" consistently resolving to the 720p Simulcast layer, never the
// 1080p base, for exactly this multitrack configuration. Sorting by actual
// encoder resolution here fixes rid order regardless of which slot layout
// SetupOutputs() had to use to avoid collisions.
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

	// Stable so two layers configured at the exact same resolution (unusual,
	// but not disallowed) keep their original slot-order relative to each
	// other instead of shuffling on every call.
	std::stable_sort(layers.begin(), layers.end(), [](obs_encoder_t *a, obs_encoder_t *b) {
		const uint64_t areaA = (uint64_t)obs_encoder_get_width(a) * obs_encoder_get_height(a);
		const uint64_t areaB = (uint64_t)obs_encoder_get_width(b) * obs_encoder_get_height(b);
		return areaA > areaB;
	});

	return layers;
}

// ppobs推流限制 (operator-specified limits, not an upstream OBS thing):
//   1. resolution no higher than 3840x2160 for landscape (width >= height)
//      or 2160x3840 for portrait - a single hard ceiling that also covers
//      the 2560x1440 tier the requirement calls out, since anything at or
//      under 1440p is by definition under this 4K ceiling too.
//   2. FPS <= 60
//   3. video bitrate <= 10000 Kbps (10 Mbps)
//
// Enforced here - inside WHIPOutput::Setup(), after every video layer
// encoder has already been assigned - rather than only in the Settings UI,
// so it applies uniformly no matter how the stream was started (manual
// button, hotkey, scheduled streaming, or an automatic reconnect) and
// can't be bypassed by an old profile/service.json that predates this
// limit. Checked against every layer actually being published (all WHIP
// Simulcast/HEVC layers), since a lower Simulcast layer inheriting a
// too-high per-layer override would otherwise slip through.
namespace {
constexpr uint32_t kMaxLandscapeWidth = 3840;
constexpr uint32_t kMaxLandscapeHeight = 2160;
constexpr uint32_t kMaxPortraitWidth = 2160;
constexpr uint32_t kMaxPortraitHeight = 3840;
constexpr double kMaxFps = 60.0;
constexpr int64_t kMaxVideoBitrateKbps = 10000;

bool checkStreamLimits(const std::vector<obs_encoder_t *> &layers, std::string &error)
{
	for (auto *encoder : layers) {
		if (!encoder)
			continue;

		const uint32_t width = obs_encoder_get_width(encoder);
		const uint32_t height = obs_encoder_get_height(encoder);
		const bool landscape = width >= height;
		const uint32_t maxWidth = landscape ? kMaxLandscapeWidth : kMaxPortraitWidth;
		const uint32_t maxHeight = landscape ? kMaxLandscapeHeight : kMaxPortraitHeight;
		if (width > maxWidth || height > maxHeight) {
			error = "resolution " + std::to_string(width) + "x" + std::to_string(height) +
				" exceeds the " + std::to_string(maxWidth) + "x" + std::to_string(maxHeight) +
				" limit for " + (landscape ? "landscape" : "portrait") + " streams";
			return false;
		}

		if (video_t *video = obs_encoder_video(encoder)) {
			const double fps = video_output_get_frame_rate(video);
			if (fps > kMaxFps + 0.01) {
				error = "frame rate " + std::to_string(fps) + " exceeds the " +
					std::to_string((int)kMaxFps) + " FPS limit";
				return false;
			}
		}

		OBSDataAutoRelease settings = obs_encoder_get_settings(encoder);
		const int64_t bitrate = obs_data_get_int(settings, "bitrate");
		if (bitrate > kMaxVideoBitrateKbps) {
			error = "video bitrate " + std::to_string(bitrate) + " Kbps exceeds the " +
				std::to_string(kMaxVideoBitrateKbps) + " Kbps limit";
			return false;
		}
	}
	return true;
}
} // namespace

// ── Encoder-parameter report collection ──────────────────────────────
// Reads the actual configuration off the encoders already assigned to this
// output. These are cheap, local reads done on the start/stop thread; only
// the HTTP POST itself is moved off-thread (see ReportEncoderConfigAsync).
namespace {

bool encoderLooksHardware(const std::string &id)
{
	static const char *needles[] = {"nvenc", "qsv", "amf", "videotoolbox", "vaapi", "mf_", "mediafoundation"};
	for (const char *needle : needles) {
		if (id.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

int encoderBitrateKbps(obs_data_t *settings)
{
	if (!settings)
		return 0;
	const long long bitrate = (long long)obs_data_get_int(settings, "bitrate");
	return bitrate > 0 ? (int)bitrate : 0;
}

// keyint_sec is the OBS-standard seconds knob; some encoders persist only the
// frame-count "keyint", which is converted with the layer's own FPS.
int encoderKeyframeSeconds(obs_data_t *settings, int fps)
{
	if (!settings)
		return 0;
	const double keyint_sec = obs_data_get_double(settings, "keyint_sec");
	if (keyint_sec > 0)
		return (int)(keyint_sec + 0.5);
	const long long keyint = (long long)obs_data_get_int(settings, "keyint");
	if (keyint > 0 && fps > 0) {
		const int seconds = (int)((keyint + fps / 2) / fps);
		return seconds > 0 ? seconds : 0;
	}
	return 0;
}

std::string encoderSettingString(obs_data_t *settings, const char *key)
{
	if (!settings)
		return "";
	const char *value = obs_data_get_string(settings, key);
	return value ? std::string(value) : std::string();
}

ObsEncoderLayer encoderToLayer(obs_encoder_t *encoder, size_t index)
{
	ObsEncoderLayer layer;
	layer.rid = std::to_string(index);
	layer.width = (int)obs_encoder_get_width(encoder);
	layer.height = (int)obs_encoder_get_height(encoder);
	if (video_t *video = obs_encoder_video(encoder))
		layer.fps = (int)(video_output_get_frame_rate(video) + 0.5);
	obs_data_t *settings = obs_encoder_get_settings(encoder);
	layer.bitrate_kbps = encoderBitrateKbps(settings);
	if (settings)
		obs_data_release(settings);
	return layer;
}

void fillVideoFromEncoder(ObsEncoderConfig &config, obs_encoder_t *encoder)
{
	const char *id = obs_encoder_get_id(encoder);
	config.encoder_id = id ? id : "";
	config.hardware = encoderLooksHardware(config.encoder_id);
	config.width = (int)obs_encoder_get_width(encoder);
	config.height = (int)obs_encoder_get_height(encoder);
	if (video_t *video = obs_encoder_video(encoder))
		config.fps = (int)(video_output_get_frame_rate(video) + 0.5);
	obs_data_t *settings = obs_encoder_get_settings(encoder);
	if (settings) {
		config.video_bitrate_kbps = encoderBitrateKbps(settings);
		config.keyframe_interval_sec = encoderKeyframeSeconds(settings, config.fps);
		config.preset = encoderSettingString(settings, "preset");
		config.profile = encoderSettingString(settings, "profile");
		config.rate_control = encoderSettingString(settings, "rate_control");
		obs_data_release(settings);
	}
}

ObsEncoderConfig buildEncoderConfig(obs_output_t *output, const std::vector<obs_encoder_t *> &h264_layers,
				    const std::vector<obs_encoder_t *> &hevc_layers, bool multitrack)
{
	ObsEncoderConfig config;
	config.protocol = "whip";
	config.multitrack = multitrack;
	config.video_codec = "h264";
	config.video_codecs.push_back("h264");
	if (multitrack && !hevc_layers.empty())
		config.video_codecs.push_back("hevc");

	if (!h264_layers.empty()) {
		// The primary (highest) H264 layer is the source feed's own
		// resolution/fps/bitrate; lower layers are its scaled ladder.
		fillVideoFromEncoder(config, h264_layers[0]);
		for (size_t i = 0; i < h264_layers.size(); ++i)
			config.simulcast_layers.push_back(encoderToLayer(h264_layers[i], i));
	}

	if (obs_encoder_t *audio = obs_output_get_audio_encoder(output, 0)) {
		const char *codec = obs_encoder_get_codec(audio);
		config.audio_codec = codec ? codec : "";
		obs_data_t *settings = obs_encoder_get_settings(audio);
		config.audio_bitrate_kbps = encoderBitrateKbps(settings);
		if (settings)
			obs_data_release(settings);
		config.audio_sample_rate = (int)obs_encoder_get_sample_rate(audio);
	}
	return config;
}

} // namespace

bool WHIPOutput::Setup(uint64_t generation)
{
	encoder_report_pending = false;
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

	// wantMultitrack only gates whether the HEVC session is additionally
	// set up and connected below - the publish request itself always asks
	// ppcenter for codec-tagged WHIP tracks (see
	// ppcenter_build_publish_json), since every WHIP publish, H264-only
	// included, lands on the codec-suffixed path (".../h264/whip"),
	// matching the design doc's §3.1 URL convention.
	const bool wantMultitrack = multitrackEnabled(output);

	PPCenterPublishRequest request{ppcenter_url,
				       obs_data_get_string(service_settings, "ppcenter_appid"),
				       obs_data_get_string(service_settings, "ppcenter_secret"),
				       obs_data_get_string(service_settings, "ppcenter_stream"),
				       obs_data_get_string(service_settings, "ppcenter_region")};
	// Register this device's hardware identity with ppcenter before
	// publishing. The server enforces a per-user device binding limit and
	// rejects publishes from unregistered devices (HTTP 403). Registration
	// is best-effort: if it fails, we still attempt to publish with the
	// best deviceId we have, and the server will reject the publish if
	// device binding is required for this account.
	std::string deviceId = RegisterPpobsDevice(request.url, request.app_id, request.app_secret);
	if (deviceId.empty()) {
		deviceId = GetOrCreateP2PClientId();
		do_log(LOG_WARNING,
		       "device registration failed; falling back to generated deviceId=%s",
		       deviceId.c_str());
	} else {
		do_log(LOG_INFO, "device registered: deviceId=%s", deviceId.c_str());
	}
	request.device_id = deviceId;
	natProbeUrl = request.url;
	natProbeAppId = request.app_id;
	natProbeAppSecret = request.app_secret;
	natProbeStreamName = request.stream_name;
	lastNatProbeRefreshMs = (int64_t)(obs_get_video_frame_time() / 1000);
	const auto probe = ProbePublisherNAT(request.url, request.app_id, request.app_secret, request.stream_name);
	if (probe.succeeded) {
		request.nat_probe_id = probe.probe_id;
		do_log(LOG_INFO, "P2P publisher NAT probe registered");
	} else {
		// NAT probing is an optional P2P optimization. Publishing must
		// continue normally; ppcenter will naturally choose edge-only.
		do_log(LOG_WARNING, "P2P publisher NAT probe failed; continuing with edge-only fallback");
	}
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
	p2pStunServers = std::move(resp.stun_servers);
	if (p2pStunServers.empty())
		p2pStunServers.emplace_back("stun:stun.l.google.com:19302");

	auto h264Layers = collectVideoLayers(output, "h264");
	if (h264Layers.empty()) {
		do_log(LOG_ERROR, "No H264 video encoder assigned");
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}

	{
		std::string limitError;
		if (!checkStreamLimits(h264Layers, limitError)) {
			do_log(LOG_ERROR, "H264 stream exceeds ppobs limits: %s", limitError.c_str());
			if (IsActiveGeneration(generation))
				obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
			return false;
		}
	}

	// H264 always publishes on its codec-tagged track (".../h264/whip"),
	// never the legacy bare whipUrl/bearerToken - see the design doc's
	// §3.1 URL convention.
	auto h264Track = resp.whip_tracks.find("h264");
	if (h264Track == resp.whip_tracks.end() || h264Track->second.url.empty() ||
	    h264Track->second.bearer_token.empty()) {
		do_log(LOG_ERROR, "ppcenter did not return an h264 WHIP track");
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
		return false;
	}
	std::string h264Url = h264Track->second.url;
	std::string h264Token = h264Track->second.bearer_token;

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

		{
			std::string limitError;
			if (!checkStreamLimits(hevcLayers, limitError)) {
				do_log(LOG_ERROR, "HEVC stream exceeds ppobs limits: %s", limitError.c_str());
				if (IsActiveGeneration(generation))
					obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
				return false;
			}
		}
		auto hevcTrack = resp.whip_tracks.find("hevc");
		if (hevcTrack == resp.whip_tracks.end() || hevcTrack->second.url.empty() ||
		    hevcTrack->second.bearer_token.empty()) {
			// See the design doc's §3.2: an incomplete whipTracks
			// response must not silently fall back to a
			// single-codec publish.
			do_log(LOG_ERROR,
			       "HEVC/H264 multitrack requested but ppcenter did not return an hevc WHIP track");
			if (IsActiveGeneration(generation))
				obs_output_signal_stop(output, OBS_OUTPUT_BAD_PATH);
			return false;
		}
		hevcUrl = hevcTrack->second.url;
		hevcToken = hevcTrack->second.bearer_token;
	}

	// Capture the actual encoder configuration now that every requested layer
	// is assigned. It is only *sent* once the output is truly running (see
	// StartThread), so a failed start never publishes a config for a stream
	// that never went live. Best-effort: the report degrades to missing
	// evidence if this fails, but never blocks or fails the publish.
	{
		ObsEncoderConfig encoderConfig =
			buildEncoderConfig(output, h264Layers, hevcLayers, wantMultitrack);
		encoder_report_url = DeriveEncoderReportUrl(ppcenter_url);
		encoder_report_body = BuildEncoderReportJson(request.app_id, request.app_secret,
							     request.stream_name, deviceId,
							     EncoderReportNowUtc(), encoderConfig);
		encoder_report_pending = !encoder_report_url.empty() && !encoder_report_body.empty();
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
		// Each codec session is a fully independent WHIP/WHEP path - a
		// player picks exactly one (h264 or hevc), never both at once
		// (see the design doc's §2.2 "codecType" being an exclusive
		// choice), so a player that lands on the hevc path has no
		// other session to get audio from. has_audio must stay true
		// here: see the design doc's §4.1 point 5 and §4.2 "音频同时
		// 发送到两条会话" - both sessions carry their own independent
		// Opus track, doubling upstream audio bandwidth by design
		// (§8.4 upline checklist explicitly expects "两份 Opus").
		hevcCfg.has_audio = true;

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

	// Setup can fail for network, credentials, or codec reasons. Initializing
	// encoders before it succeeds leaves them active even though data capture
	// never began, so libobs's normal stop path cannot release them and the
	// Output settings remain disabled. Initialize only after every requested
	// WHIP session is established.
	if (!obs_output_initialize_encoders(output, 0)) {
		if (h264Session) {
			h264Session->Stop();
			h264Session.reset();
		}
		if (hevcSession) {
			hevcSession->Stop();
			hevcSession.reset();
		}
		if (IsActiveGeneration(generation))
			obs_output_signal_stop(output, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	do_log(LOG_INFO, "WHIPOutput: Started (%s)", hevcSession ? "H264+HEVC multitrack" : "H264 only");

	if (encoder_report_pending) {
		ReportEncoderConfigAsync(encoder_report_url, encoder_report_body);
		encoder_report_pending = false;
	}

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
						      generate_random_u32(), p2pStunServers);
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

// ppcenter's publisher-side NAT observation (submitted once, in Setup(),
// before this session ever starts publishing) expires 5 minutes after that
// single submission (nat_probe_v1.go: ExpiresAt = now+5min). A live stream
// routinely runs far longer than that, and Coordinator.AllocateEligible
// fails every play attempt closed with missing_publisher_probe once it's
// gone stale - not "P2P tried and lost", P2P is never even offered to any
// viewer who joins more than ~5 minutes after the stream started. Confirmed
// in production 2026-09-22: a publisher probe from 11:37 was still the only
// one on record when viewers tried to play at 12:04/12:06, over 25 minutes
// later - see docs/test/ppcdn-debug-log.md's 2026-09-22 entry.
//
// Refreshed here every 4 minutes (comfortably inside the 5-minute TTL) by
// resubmitting the exact same probe. This is safe to repeat: probeIDFor()
// is deterministic from (appId, clientId), and clientId itself is a stable
// per-installation UUID (GetOrCreateP2PClientId()), so every resubmission
// lands on the same ppcenter-side observation slot as an upsert
// (Save()/RegisterTrustedObservation()) rather than creating a new one -
// nothing downstream of the original /v1/publish/requests call (the
// natProbeId already handed to ppcenter, or this session's P2P signaling)
// needs to change when a refresh succeeds.
constexpr int64_t NAT_PROBE_REFRESH_INTERVAL_MS = 4 * 60 * 1000;

void WHIPOutput::CheckNatProbeRefresh()
{
	// obs_get_video_frame_time() returns nanoseconds (obs-video.c:
	// obs->video.video_time = os_gettime_ns()), so this needs /1,000,000 to
	// land in milliseconds - not /1000, which is only microseconds. Caught
	// in production 2026-09-22: with /1000, NAT_PROBE_REFRESH_INTERVAL_MS
	// (240000, intended as 240000ms = 4min) was being compared against a
	// microsecond counter, so the "4 minute" throttle was actually a 240ms
	// one - Data() runs per encoder packet, so this fired on nearly every
	// packet and spawned a probe-submission thread each time (654 publisher
	// /v1/nat/probe submissions in 3 minutes were observed on ppcenter
	// before this fix). See docs/test/ppcdn-debug-log.md's 2026-09-22 entry.
	//
	// CheckUplinkQos() above uses this same obs_get_video_frame_time()/1000
	// pattern and very likely has the identical bug (its "2000" threshold is
	// probably 2ms of real time, not 2s) - not fixed here since it's a
	// separate subsystem (uplink congestion policy) this session never
	// otherwise touched, and changing its actual behavioral cadence deserves
	// its own review rather than a drive-by fix alongside this one.
	auto now_ms = (int64_t)(obs_get_video_frame_time() / 1000000);
	if (now_ms - lastNatProbeRefreshMs < NAT_PROBE_REFRESH_INTERVAL_MS)
		return;
	lastNatProbeRefreshMs = now_ms;
	if (natProbeUrl.empty() || natProbeAppId.empty() || natProbeAppSecret.empty() || natProbeStreamName.empty())
		return;

	// ProbePublisherNAT does its own blocking curl call (up to ~9s worst
	// case: 4s ICE gather + 5s HTTP) - Data() runs on the active encoder's
	// hot path, so this must never run inline there. Detached and
	// fire-and-forget, matching ProbePublisherNAT's existing "must never
	// block or abort the publish attempt" contract (nat-probe.h); the
	// thread only touches copies of plain strings, never `this`, so it's
	// safe even if this WHIPOutput is destroyed while a refresh is still
	// in flight.
	// blog() directly, not the do_log() macro used elsewhere in this file -
	// do_log implicitly references the `output` member (via
	// obs_output_get_name(output)), which this lambda deliberately does not
	// capture.
	std::thread([url = natProbeUrl, appId = natProbeAppId, appSecret = natProbeAppSecret,
		     streamName = natProbeStreamName]() {
		const auto probe = ProbePublisherNAT(url, appId, appSecret, streamName);
		if (probe.succeeded)
			blog(LOG_DEBUG, "[obs-webrtc] [nat-probe-refresh] P2P publisher NAT probe refreshed");
		else
			blog(LOG_DEBUG,
			     "[obs-webrtc] [nat-probe-refresh] P2P publisher NAT probe refresh failed; will retry next interval");
	}).detach();
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
