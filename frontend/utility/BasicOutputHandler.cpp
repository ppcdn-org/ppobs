#include "BasicOutputHandler.hpp"
#include "AdvancedOutput.hpp"
#include "SimpleOutput.hpp"

#include <abs-ts.h>
#include <utility/ForceNoBFrames.hpp>
#include <utility/MultitrackVideoError.hpp>
#include <utility/PPCDNPublishEndpoint.hpp>
#include <utility/PublishCodecCheck.hpp>
#include <utility/StartMultiTrackVideoStreamingGuard.hpp>
#include <utility/VCamConfig.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QThreadPool>

#include <algorithm>
#include <cmath>

using namespace std;

extern bool EncoderAvailable(const char *encoder);

volatile bool streaming_active = false;
volatile bool recording_active = false;
volatile bool recording_paused = false;
volatile bool replaybuf_active = false;
volatile bool virtualcam_active = false;

void OBSStreamStarting(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0)
		return;

	output->delayActive = true;
	QMetaObject::invokeMethod(output->main, "StreamDelayStarting", Q_ARG(int, sec));
}

void OBSStreamStopping(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0)
		QMetaObject::invokeMethod(output->main, "StreamStopping");
	else
		QMetaObject::invokeMethod(output->main, "StreamDelayStopping", Q_ARG(int, sec));
}

void OBSStartStreaming(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	output->streamingActive = true;
	os_atomic_set_bool(&streaming_active, true);
	QMetaObject::invokeMethod(output->main, "StreamingStart");
}

void OBSStopStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");

	QString arg_last_error = QString::fromUtf8(last_error);

	// However the primary publish ended - user stop, reconnects exhausted,
	// a frontend API call - the companion must not be left publishing on
	// its own. StopCompanionStream is a no-op once it has already stopped.
	output->StopCompanionStream(false);
	// The P2P companion (if any) is tied to the primary publish's lifetime the
	// same way - never leave it publishing on its own.
	output->StopP2PCompanion();
	// The crop publish is likewise never left publishing on its own.
	output->StopCropOutput(true);

	output->streamingActive = false;
	output->delayActive = false;
	output->multitrackVideoActive = false;
	os_atomic_set_bool(&streaming_active, false);
	QMetaObject::invokeMethod(output->main, "StreamingStop", Q_ARG(int, code), Q_ARG(QString, arg_last_error));
}

void OBSStartRecording(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->recordingActive = true;
	os_atomic_set_bool(&recording_active, true);
	QMetaObject::invokeMethod(output->main, "RecordingStart");
}

void OBSStopRecording(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");

	QString arg_last_error = QString::fromUtf8(last_error);

	output->recordingActive = false;
	os_atomic_set_bool(&recording_active, false);
	os_atomic_set_bool(&recording_paused, false);
	QMetaObject::invokeMethod(output->main, "RecordingStop", Q_ARG(int, code), Q_ARG(QString, arg_last_error));
}

void OBSRecordStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "RecordStopping");
}

void OBSRecordFileChanged(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	const char *next_file = calldata_string(params, "next_file");

	QString arg_last_file = QString::fromUtf8(output->lastRecordingPath.c_str());

	QMetaObject::invokeMethod(output->main, "RecordingFileChanged", Q_ARG(QString, arg_last_file));

	output->lastRecordingPath = next_file;
}

void OBSStartReplayBuffer(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->replayBufferActive = true;
	os_atomic_set_bool(&replaybuf_active, true);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStart");
}

void OBSStopReplayBuffer(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->replayBufferActive = false;
	os_atomic_set_bool(&replaybuf_active, false);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStop", Q_ARG(int, code));
}

void OBSReplayBufferStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStopping");
}

void OBSReplayBufferSaved(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "ReplayBufferSaved", Qt::QueuedConnection);
}

static void OBSStartVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->virtualCamActive = true;
	os_atomic_set_bool(&virtualcam_active, true);
	QMetaObject::invokeMethod(output->main, "OnVirtualCamStart");
}

static void OBSStopVirtualCam(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->virtualCamActive = false;
	os_atomic_set_bool(&virtualcam_active, false);
	QMetaObject::invokeMethod(output->main, "OnVirtualCamStop", Q_ARG(int, code));
}

static void OBSDeactivateVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	output->DestroyVirtualCamView();
}

bool return_first_id(void *data, const char *id)
{
	const char **output = (const char **)data;

	*output = id;
	return false;
}

const char *GetStreamOutputType(const obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	const char *output = nullptr;

	if (!protocol) {
		blog(LOG_WARNING, "The service '%s' has no protocol set", obs_service_get_id(service));
		return nullptr;
	}

	if (!obs_is_output_protocol_registered(protocol)) {
		blog(LOG_WARNING, "The protocol '%s' is not registered", protocol);
		return nullptr;
	}

	/* Check if the service has a preferred output type */
	output = obs_service_get_preferred_output_type(service);
	if (output) {
		if ((obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0)
			return output;

		blog(LOG_WARNING, "The output '%s' is not registered, fallback to another one", output);
	}

	/* Otherwise, prefer first-party output types */
	if (can_use_output(protocol, "rtmp_output", "RTMP", "RTMPS")) {
		return "rtmp_output";
	} else if (can_use_output(protocol, "ffmpeg_hls_muxer", "HLS")) {
		return "ffmpeg_hls_muxer";
	} else if (can_use_output(protocol, "ffmpeg_mpegts_muxer", "SRT", "RIST")) {
		return "ffmpeg_mpegts_muxer";
	}

	/* If third-party protocol, use the first enumerated type */
	obs_enum_output_types_with_protocol(protocol, &output, return_first_id);
	if (output)
		return output;

	blog(LOG_WARNING, "No output compatible with the service '%s' is registered", obs_service_get_id(service));

	return nullptr;
}

// OBSCompanionStopStreaming fires when the HEVC publish stops on its own.
// During a deliberate teardown that's expected and already handled; outside
// one it means the HEVC half died while the H264 half is still on air, which
// leaves HEVC viewers with nothing and no indication why - so stop the pair
// and report it through the primary output's normal failure path.
static void OBSCompanionStopStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	if (output->stoppingStreamPair)
		return;

	const char *last_error = calldata_string(params, "last_error");
	blog(LOG_WARNING, "HEVC publish stopped on its own (%s); stopping the H264 publish too",
	     (last_error && *last_error) ? last_error : "no error given");

	if (output->streamOutput)
		obs_output_stop(output->streamOutput);
}

// BuildCodecServiceSettings copies a service's settings and points the
// publish URL at that codec's own path, which is the only thing that differs
// between the two connections of a multitrack publish.
static OBSDataAutoRelease BuildCodecServiceSettings(obs_service_t *primary, const char *codec)
{
	// obs_service_get_settings hands back the service's own live settings
	// object, not a copy, so this has to duplicate before rewriting -
	// otherwise each call mutates the configured service (and the previous
	// call's result, since they'd all be the same object).
	OBSDataAutoRelease original = obs_service_get_settings(primary);
	if (!original)
		return nullptr;

	OBSDataAutoRelease settings = obs_data_create_from_json(obs_data_get_json(original));
	if (!settings)
		return nullptr;

	// Derived from the *configured* server string rather than the resolved
	// URL, so a placeholder like {ppcenter_appid} is expanded once by the
	// service itself instead of being baked in here.
	const char *server = obs_data_get_string(settings, "server");
	if (!server || !*server)
		return nullptr;

	obs_data_set_string(settings, "server", WithPublishCodec(server, codec).c_str());

	// SRT ingest can be token-gated on the Origin (mmx srtPublishTokenRequired)
	// and the token is bound to the exact publish path, so this codec's
	// connection must carry *its* token, not the primary's codec-less one.
	// These per-codec settings are filled by ResolvePPCDNPublishEndpoint (see
	// PPCDNPublishEndpoint.cpp); empty for WHIP/RTMP or an Origin without the
	// check, in which case the copied primary token is left untouched.
	const char *codecTokenKey = (codec == std::string("hevc")) ? "ppcdn_publish_token_hevc"
								   : "ppcdn_publish_token_h264";
	const std::string codecToken = obs_data_get_string(settings, codecTokenKey);
	if (!codecToken.empty())
		obs_data_set_string(settings, "ppcdn_publish_token", codecToken.c_str());

	return settings;
}

obs_service_t *BasicOutputHandler::PrimaryPublishService(obs_service_t *service)
{
	if (h264StreamService)
		return h264StreamService.Get();
	if (singleCodecStreamService)
		return singleCodecStreamService.Get();
	return service;
}

void BasicOutputHandler::SetupCompanionStream(const char *outputType, const char *streamCodec)
{
	singleCodecStreamService = nullptr;

	const bool wanted = whipHevcEncoders != nullptr && whipHevcEncoders->HasMainEncoder() &&
			    StreamTransportIsSingleCodec(main->GetService());

	if (!wanted) {
		// Configuration changed since the last publish (multitrack
		// turned off, or the service switched to WHIP): drop the
		// companion rather than leave a stale one attached.
		hevcStopStreaming.Disconnect();
		hevcStreamOutput = nullptr;
		hevcStreamService = nullptr;
		h264StreamService = nullptr;

		// Multitrack off: publish on the path for the codec actually being
		// encoded (".../h264" or ".../hevc") instead of the bare stream path.
		// pplayer and the degrade channel both address a stream by codec
		// path, so a bare-path publish is unplayable there. Only for a
		// PPCenter-managed URL (the placeholder is what makes ppcenter mint
		// the per-codec token) that doesn't already pin a codec itself.
		obs_service_t *service = main->GetService();
		const char *codec = streamCodec;
		if (!service || whipHevcEncoders || !codec || (strcmp(codec, "h264") != 0 && strcmp(codec, "hevc") != 0) ||
		    !StreamTransportIsSingleCodec(service) || strcmp(obs_service_get_id(service), "rtmp_custom") != 0)
			return;

		OBSDataAutoRelease original = obs_service_get_settings(service);
		const char *server = obs_data_get_string(original, "server");
		if (!server || !strstr(server, PPCDN_PUBLISH_ENDPOINT_PLACEHOLDER) ||
		    !PublishCodecFromURL(server).empty())
			return;

		OBSDataAutoRelease settings = BuildCodecServiceSettings(service, codec);
		if (!settings)
			return;
		singleCodecStreamService = obs_service_create(obs_service_get_id(service), "single_codec_stream_service",
							      settings, nullptr);
		if (singleCodecStreamService)
			blog(LOG_INFO, "Publishing on the /%s path (multitrack off)", codec);
		return;
	}

	if (hevcStreamOutput)
		return;

	obs_service_t *primary = main->GetService();

	// Both halves publish to their own codec path, derived from the one
	// configured URL - the user configures a single stream path and does
	// not have to know the pair exists.
	OBSDataAutoRelease h264Settings = BuildCodecServiceSettings(primary, "h264");
	OBSDataAutoRelease settings = BuildCodecServiceSettings(primary, "hevc");
	if (!h264Settings || !settings) {
		blog(LOG_ERROR, "HEVC multitrack: could not derive the per-codec publish URLs from the service");
		return;
	}

	h264StreamService = obs_service_create(obs_service_get_id(primary), "h264_stream_service", h264Settings,
					       nullptr);
	hevcStreamService = obs_service_create(obs_service_get_id(primary), "hevc_stream_service", settings, nullptr);
	if (!h264StreamService || !hevcStreamService) {
		blog(LOG_ERROR, "HEVC multitrack: failed to create the per-codec publish services");
		h264StreamService = nullptr;
		hevcStreamService = nullptr;
		return;
	}

	hevcStreamOutput = obs_output_create(outputType, "hevc_stream", nullptr, nullptr);
	if (!hevcStreamOutput) {
		blog(LOG_ERROR, "HEVC multitrack: failed to create the HEVC publish output");
		hevcStreamService = nullptr;
		h264StreamService = nullptr;
		return;
	}

	// Without this, the HEVC connection never carries the SEI timestamp
	// abs_ts_sei_inject stamps into every frame (see AdvancedOutput.cpp's
	// identical registration on streamOutput), so mmx never forwards an
	// OBS_TIMESTAMP for HEVC viewers and pplayer's P2P Delay reads N/A.
	obs_output_add_packet_callback(hevcStreamOutput, abs_ts_sei_inject, nullptr);

	obs_output_set_service(hevcStreamOutput, hevcStreamService);
	hevcStopStreaming.Connect(obs_output_get_signal_handler(hevcStreamOutput), "stop", OBSCompanionStopStreaming,
				  this);

	blog(LOG_INFO, "HEVC multitrack: publishing HEVC separately to %s",
	     obs_service_get_connect_info(hevcStreamService, OBS_SERVICE_CONNECT_INFO_SERVER_URL));
}

bool BasicOutputHandler::P2PStreamingEnabled() const
{
	config_t *config = main->Config();
	return !config_has_user_value(config, "P2P", "Enabled") ||
	       config_get_bool(config, "P2P", "Enabled");
}

int BasicOutputHandler::P2PStreamingMaxSessions() const
{
	config_t *config = main->Config();
	if (!config_has_user_value(config, "P2P", "MaxSessions"))
		return 3;
	int value = (int)config_get_int(config, "P2P", "MaxSessions");
	return value > 0 ? value : 3;
}

bool BasicOutputHandler::EnforceNoBFrames(obs_encoder_t *videoEncoder)
{
	if (!videoEncoder)
		return true;

	// Every PPCDN publish must carry no B-frames, regardless of transport or
	// whether P2P is enabled: the WebRTC leg cannot consume them, and the
	// SRT/RTMP receive side should not have to cope with reordered DTS. The
	// WHIP service only sets "bf" on its own settings object, which never
	// reaches encoders created from a saved profile (and misses QSV's
	// "bframes" key), so enforce it on the encoder itself here.
	ForceEncoderNoBFrames(videoEncoder);

	OBSDataAutoRelease applied = obs_encoder_get_settings(videoEncoder);
	const bool bfOff = obs_data_get_int(applied, "bf") == 0;
	const bool bframesOff = obs_data_get_int(applied, "bframes") == 0 ||
				!obs_data_has_user_value(applied, "bframes");
	if (bfOff && bframesOff)
		return true;

	// The encoder clamped or rejected the setting (e.g. a preset that
	// mandates B-frames, or a custom option overriding it). Warn instead of
	// aborting the publish.
	blog(LOG_WARNING, "[PPCDN] streaming video encoder did not accept B-frames=0");
	OBSBasic *m = main;
	QMetaObject::invokeMethod(
		m,
		[m]() {
			OBSMessageBox::warning(
				m, QStringLiteral("PPCDN encoder did not accept B-frames=0"),
				QStringLiteral("The streaming video encoder could not be forced to 0 B-frames. "
					       "Set B-frames / bframes to 0 in Settings > Output for this encoder; "
					       "otherwise the stream carries B-frames and P2P/low-latency playback may misbehave."));
		},
		Qt::QueuedConnection);
	return false;
}

void BasicOutputHandler::StartP2PCompanion(obs_encoder_t *videoEnc, obs_encoder_t *audioEnc)
{
	if (p2pOutput)
		return; // already running

	obs_service_t *primary = main->GetService();
	if (!primary || !videoEnc)
		return;

	// A WHIP publish already runs P2P inside its own whip_output; a companion
	// would register the publisher with ppcenter twice. Only non-WHIP
	// transports (SRT, ...) need this.
	const char *serviceId = obs_service_get_id(primary);
	if (serviceId && strcmp(serviceId, "whip_custom") == 0)
		return;

	// ERTSP never attempts P2P (see the ERTSP cross-repo plan's confirmed
	// scope - no NAT probe, no capability toggle, same as pplayer's read
	// side). Without this check the companion blindly inherited the
	// primary service's resolved "server" setting - for ERTSP a complete
	// rtsp://user:pass@host:port/path URL - and tried to open it as a WHIP
	// endpoint, failing immediately with a nonsensical "Protocol not
	// found"/connect error logged under the companion's own name (found in
	// the field: this fired for every ERTSP publish attempt until this
	// check existed).
	const char *protocol = obs_service_get_protocol(primary);
	if (protocol && astrcmpi(protocol, "RTSP") == 0)
		return;

	// P2P is allowed for WHIP publishes only (they run it inside whip_output,
	// handled above). The companion code stays; flip this to re-enable it for
	// SRT/ERTMP.
	constexpr bool kP2PCompanionForNonWHIP = false;
	if (!kP2PCompanionForNonWHIP) {
		blog(LOG_INFO, "[PPCDN] P2P companion disabled: P2P is only allowed for WHIP publishes");
		return;
	}

	if (!P2PStreamingEnabled()) {
		blog(LOG_INFO, "[PPCDN] P2P companion disabled in Settings > Advanced");
		return;
	}

	// Only a PPCDN publish (ppcenter configured) has anywhere to signal to.
	OBSDataAutoRelease serviceSettings = obs_service_get_settings(primary);
	if (!serviceSettings || !*obs_data_get_string(serviceSettings, "ppcenter_url"))
		return;

	// P2P broadcasts H264 only (see whip-output.cpp's StartP2PSignal). Feeding
	// the companion an HEVC encoder's packets would produce an unplayable P2P
	// track, so leave P2P off rather than serve a broken one.
	const char *codec = obs_encoder_get_codec(videoEnc);
	if (!codec || strcmp(codec, "h264") != 0) {
		blog(LOG_WARNING, "[PPCDN] P2P companion skipped: streaming video codec is '%s', P2P needs h264",
		     codec ? codec : "?");
		return;
	}

	OBSDataAutoRelease p2pSettings = obs_data_create();
	obs_data_set_int(p2pSettings, "ppcdn_p2p_max_sessions", P2PStreamingMaxSessions());
	p2pOutput = obs_output_create("ppcenter_p2p_output", "ppcenter_p2p", p2pSettings, nullptr);
	if (!p2pOutput) {
		blog(LOG_WARNING, "[PPCDN] P2P companion: failed to create ppcenter_p2p_output");
		return;
	}
	// Same absolute-timestamp SEI the primary output gets. Packet callbacks are
	// per-output (libobs send_interleaved runs them on that output's own packet
	// copy), so without this the SRT/ERTMP P2P companion would carry no SEI and
	// pplayer could only show the estimated P2P delay ("est."). WHIP needs no
	// equivalent because its P2P feed runs inside the output that already has
	// this callback.
	obs_output_add_packet_callback(p2pOutput, abs_ts_sei_inject, nullptr);
	// Its own service instance, not the primary's: see p2pService's declaration
	// in the header. A copy of the primary's settings carries the same ppcenter
	// creds; obs_service_create() does not expand anything the primary's own
	// update already resolved.
	p2pService = obs_service_create(serviceId, "ppcenter_p2p_service", serviceSettings, nullptr);
	if (!p2pService) {
		blog(LOG_WARNING, "[PPCDN] P2P companion: failed to create its service");
		p2pOutput = nullptr;
		return;
	}
	// Shares the primary's H264/audio encoders - the P2P output only reads
	// encoded packets, it opens no WHIP media session.
	obs_output_set_service(p2pOutput, p2pService);
	obs_output_set_video_encoder(p2pOutput, videoEnc);
	if (audioEnc)
		obs_output_set_audio_encoder(p2pOutput, audioEnc, 0);

	// Best-effort: the main publish is already live by now, so unlike the HEVC
	// companion's strong-consistency start, a P2P failure is only logged - it
	// never tears down or blocks the main publish (P2P is an optional
	// enhancement over the edge path).
	if (obs_output_start(p2pOutput)) {
		blog(LOG_INFO, "[PPCDN] P2P companion started alongside the %s publish", serviceId ? serviceId : "");
	} else {
		const char *err = obs_output_get_last_error(p2pOutput);
		blog(LOG_WARNING, "[PPCDN] P2P companion failed to start (%s); main publish continues without P2P",
		     (err && *err) ? err : "unknown");
		p2pOutput = nullptr;
		p2pService = nullptr;
	}
}

void BasicOutputHandler::StopP2PCompanion()
{
	if (!p2pOutput)
		return;
	// force_stop, not stop: WHIPOutput::Start() only spawns its start thread,
	// so the output is still inactive for a while after obs_output_start()
	// returned. obs_output_stop() no-ops on an inactive output, and then
	// dropping the reference below runs obs_output_destroy -> the output's
	// destructor Stop(), which joins the start thread, signals stop and
	// spawns an end_data_capture_thread *after* obs_output_destroy's join
	// check - it then stops the shared encoders against an already-freed
	// output (crash in obs_encoder_stop). force_stop always runs the output's
	// stop path first, so that end_data_capture_thread exists before we
	// release, and obs_output_destroy joins it.
	obs_output_force_stop(p2pOutput);
	p2pOutput = nullptr;
	// After the output, which clears its service->output back-pointer - see
	// p2pService's declaration.
	p2pService = nullptr;
}

bool BasicOutputHandler::StartCompanionStream()
{
	if (!hevcStreamOutput)
		return true;

	stoppingStreamPair = false;

	if (obs_output_start(hevcStreamOutput))
		return true;

	const char *error = obs_output_get_last_error(hevcStreamOutput);
	lastError = (error && *error) ? error
				      : "The HEVC publish failed to start. Both codecs must be publishing before "
					"streaming begins, so the H264 publish was not started either.";
	blog(LOG_WARNING, "HEVC publish failed to start: %s", (error && *error) ? error : "no error given");
	return false;
}

void BasicOutputHandler::StopCompanionStream(bool force)
{
	if (!hevcStreamOutput)
		return;

	stoppingStreamPair = true;
	if (force)
		obs_output_force_stop(hevcStreamOutput);
	else
		obs_output_stop(hevcStreamOutput);
}

uint64_t BasicOutputHandler::TotalStreamingBytes() const
{
	uint64_t total = obs_output_get_total_bytes(StreamingOutput());
	if (hevcStreamOutput)
		total += obs_output_get_total_bytes(hevcStreamOutput);
	if (cropOutput)
		total += obs_output_get_total_bytes(cropOutput);
	return total;
}

// Bitrate for the crop publish: the main stream's bitrate scaled by the crop
// region's share of the main output's pixel area (Settings > Video > General
// documents it as crop area / main output area * main bitrate).
static int64_t CropOutputProportionalBitrate(int64_t mainBitrate, uint32_t mainWidth, uint32_t mainHeight,
					     uint32_t cropWidth, uint32_t cropHeight)
{
	if (mainWidth == 0 || mainHeight == 0)
		return mainBitrate > 0 ? mainBitrate : 1;

	int64_t bitrate =
		mainBitrate * (int64_t)cropWidth * cropHeight / ((int64_t)mainWidth * mainHeight);
	return bitrate > 0 ? bitrate : 1;
}

void BasicOutputHandler::StartCropOutput(obs_service_t *service, obs_encoder_t *videoEncoder,
					 obs_encoder_t *audioEncoder)
{
	if (cropOutput)
		return;
	if (!config_get_bool(main->Config(), "Video", "CropOutputEnabled"))
		return;
	if (!service || !videoEncoder || !audioEncoder)
		return;

	video_t *mainVideo = obs_get_video();
	struct obs_video_info ovi;
	if (!mainVideo || !obs_get_video_info(&ovi)) {
		blog(LOG_WARNING, "[crop] no main video to crop");
		return;
	}

	const uint32_t outWidth = video_output_get_width(mainVideo);
	const uint32_t outHeight = video_output_get_height(mainVideo);
	if (outWidth == 0 || outHeight == 0)
		return;

	int x = (int)config_get_int(main->Config(), "Video", "CropX");
	int y = (int)config_get_int(main->Config(), "Video", "CropY");
	int width = (int)config_get_int(main->Config(), "Video", "CropWidth");
	int height = (int)config_get_int(main->Config(), "Video", "CropHeight");

	// Crop coordinates are in the main *output* (scaled) resolution, the
	// same space the bitrate ratio is computed in.
	x = std::max(x, 0);
	y = std::max(y, 0);
	if (x >= (int)outWidth || y >= (int)outHeight) {
		blog(LOG_WARNING, "[crop] crop origin %d,%d is outside the %ux%u output", x, y, outWidth, outHeight);
		return;
	}
	width = std::min(width, (int)outWidth - x);
	height = std::min(height, (int)outHeight - y);
	if (width < 2 || height < 2) {
		blog(LOG_WARNING, "[crop] invalid crop size %dx%d", width, height);
		return;
	}

	// H264 wants even dimensions.
	width &= ~1;
	height &= ~1;

	const double scaleX = (double)ovi.base_width / (double)outWidth;
	const double scaleY = (double)ovi.base_height / (double)outHeight;
	int64_t baseX = (int64_t)std::llround(x * scaleX);
	int64_t baseY = (int64_t)std::llround(y * scaleY);
	uint32_t baseWidth = (uint32_t)std::max<int64_t>(2, ((int64_t)std::llround(width * scaleX)) & ~1LL);
	uint32_t baseHeight = (uint32_t)std::max<int64_t>(2, ((int64_t)std::llround(height * scaleY)) & ~1LL);

	baseX = std::max<int64_t>(baseX, 0);
	baseY = std::max<int64_t>(baseY, 0);
	if (baseX >= (int64_t)ovi.base_width || baseY >= (int64_t)ovi.base_height) {
		blog(LOG_WARNING, "[crop] crop origin %lld,%lld is outside the %ux%u canvas", (long long)baseX,
		     (long long)baseY, ovi.base_width, ovi.base_height);
		return;
	}
	baseWidth = std::min<uint32_t>(baseWidth, (ovi.base_width - (uint32_t)baseX) & ~1u);
	baseHeight = std::min<uint32_t>(baseHeight, (ovi.base_height - (uint32_t)baseY) & ~1u);
	if (baseWidth < 2 || baseHeight < 2) {
		blog(LOG_WARNING, "[crop] invalid canvas crop size %ux%u", baseWidth, baseHeight);
		return;
	}

	// A private canvas renders the program's scene source offset by the crop
	// origin, clipped to the crop size, then scaled to the requested output
	// size. Keeping it separate from the main canvas leaves the normal
	// program output untouched.
	struct obs_video_info cropOvi = ovi;
	cropOvi.base_width = baseWidth;
	cropOvi.base_height = baseHeight;
	cropOvi.output_width = (uint32_t)width;
	cropOvi.output_height = (uint32_t)height;
	if (cropOvi.scale_type == OBS_SCALE_DISABLE)
		cropOvi.scale_type = OBS_SCALE_BICUBIC;

	cropCanvas = obs_canvas_create_private("crop_output_canvas", &cropOvi, 0);
	if (!cropCanvas) {
		blog(LOG_WARNING, "[crop] failed to create the crop canvas");
		return;
	}

	cropScene = obs_canvas_scene_create(cropCanvas, "crop_output_scene");
	if (!cropScene) {
		blog(LOG_WARNING, "[crop] failed to create the crop scene");
		StopCropOutput(true);
		return;
	}

	OBSSourceAutoRelease program = obs_get_output_source(0);
	if (!program) {
		blog(LOG_WARNING, "[crop] no program source to crop");
		StopCropOutput(true);
		return;
	}

	obs_sceneitem_t *item = obs_scene_add(cropScene, program);
	if (!item) {
		blog(LOG_WARNING, "[crop] failed to add the program source to the crop scene");
		StopCropOutput(true);
		return;
	}
	// Sources with audio are added hidden by libobs; the crop scene always
	// shows the program.
	obs_sceneitem_set_visible(item, true);

	struct vec2 pos;
	pos.x = (float)(-baseX);
	pos.y = (float)(-baseY);
	obs_sceneitem_set_pos(item, &pos);

	obs_canvas_set_channel(cropCanvas, 0, obs_scene_get_source(cropScene));

	// Derive the crop publish URL from the service the main publish actually
	// uses (so a PPCenter per-codec token/path is inherited), appending the
	// "-h5" marker to the stream name. The crop publish is always H264.
	obs_service_t *primary = PrimaryPublishService(service);
	OBSDataAutoRelease original = obs_service_get_settings(primary);
	OBSDataAutoRelease settings = original ? obs_data_create_from_json(obs_data_get_json(original)) : nullptr;
	if (!settings) {
		blog(LOG_WARNING, "[crop] failed to copy the publish service settings");
		StopCropOutput(true);
		return;
	}

	const char *server = obs_data_get_string(settings, "server");
	if (!server || !*server) {
		blog(LOG_WARNING, "[crop] the publish service has no Server URL");
		StopCropOutput(true);
		return;
	}

	std::string cropUrl = AppendPublishPathSuffix(server, "-h5");
	// Mirror the main publish's codec-path convention, but always as H264 -
	// e.g. a HEVC main publish's ".../h264" becomes "...-h5/h264".
	if (!PublishCodecFromURL(server).empty())
		cropUrl = WithPublishCodec(cropUrl, "h264");
	obs_data_set_string(settings, "server", cropUrl.c_str());

	cropService =
		obs_service_create(obs_service_get_id(primary), "crop_stream_service", settings, nullptr);
	if (!cropService) {
		blog(LOG_WARNING, "[crop] failed to create the crop publish service");
		StopCropOutput(true);
		return;
	}

	const char *mainEncoderId = obs_encoder_get_id(videoEncoder);
	const std::string h264EncoderId = ResolveWHIPH264EncoderId(mainEncoderId ? mainEncoderId : "");
	OBSDataAutoRelease mainSettings = obs_encoder_get_settings(videoEncoder);
	OBSDataAutoRelease cropSettings =
		mainSettings ? obs_data_create_from_json(obs_data_get_json(mainSettings)) : obs_data_create();

	// The settings are copied from the main encoder, which may be HEVC/AV1.
	// Drop a profile H264 cannot use (e.g. HEVC's "main10") so the H264
	// encoder gets a valid one.
	const char *profile = obs_data_get_string(cropSettings, "profile");
	if (profile && *profile && strcmp(profile, "baseline") != 0 && strcmp(profile, "main") != 0 &&
	    strcmp(profile, "high") != 0)
		obs_data_set_string(cropSettings, "profile", "high");

	const int64_t mainBitrate = obs_data_get_int(cropSettings, "bitrate");
	const int64_t cropBitrate =
		CropOutputProportionalBitrate(mainBitrate, outWidth, outHeight, (uint32_t)width, (uint32_t)height);
	obs_data_set_int(cropSettings, "bitrate", cropBitrate);

	const int64_t mainMaxBitrate = obs_data_get_int(cropSettings, "max_bitrate");
	if (mainMaxBitrate > 0) {
		int64_t cropMaxBitrate = mainBitrate > 0 ? mainMaxBitrate * cropBitrate / mainBitrate : mainMaxBitrate;
		if (cropMaxBitrate < cropBitrate)
			cropMaxBitrate = cropBitrate;
		obs_data_set_int(cropSettings, "max_bitrate", cropMaxBitrate);
	}

	cropVideoEncoder =
		obs_video_encoder_create(h264EncoderId.c_str(), "crop_stream_video", cropSettings, nullptr);
	if (!cropVideoEncoder) {
		blog(LOG_WARNING, "[crop] failed to create the H264 encoder '%s'", h264EncoderId.c_str());
		StopCropOutput(true);
		return;
	}
	obs_encoder_set_video(cropVideoEncoder, obs_canvas_get_video(cropCanvas));
	// The crop publish is always 8-bit H264, even when the main stream is
	// 10-bit HEVC/AV1 (the copied settings must not force P010/I010 on it).
	obs_encoder_set_preferred_video_format(cropVideoEncoder, VIDEO_FORMAT_NV12);
	ForceEncoderNoBFrames(cropVideoEncoder);

	const char *type = GetStreamOutputType(cropService);
	if (!type) {
		blog(LOG_WARNING, "[crop] no output type for the crop publish");
		StopCropOutput(true);
		return;
	}

	cropOutput = obs_output_create(type, "crop_stream", nullptr, nullptr);
	if (!cropOutput) {
		blog(LOG_WARNING, "[crop] failed to create the crop output");
		StopCropOutput(true);
		return;
	}

	obs_output_add_packet_callback(cropOutput, abs_ts_sei_inject, nullptr);
	obs_output_set_service(cropOutput, cropService);
	obs_output_set_video_encoder(cropOutput, cropVideoEncoder);
	obs_output_set_audio_encoder(cropOutput, audioEncoder, 0);

	bool reconnect = config_get_bool(main->Config(), "Output", "Reconnect");
	int retryDelay = config_get_uint(main->Config(), "Output", "RetryDelay");
	int maxRetries = config_get_uint(main->Config(), "Output", "MaxRetries");
	if (!reconnect)
		maxRetries = 0;
	obs_output_set_reconnect_settings(cropOutput, maxRetries, retryDelay);

	if (!obs_output_start(cropOutput)) {
		const char *error = obs_output_get_last_error(cropOutput);
		blog(LOG_WARNING, "[crop] failed to start the crop publish (%s); main publish continues",
		     (error && *error) ? error : "unknown error");
		StopCropOutput(true);
		return;
	}

	blog(LOG_INFO, "[crop] publishing %dx%d at %lld kbps to %s", width, height, (long long)cropBitrate,
	     cropUrl.c_str());
}

void BasicOutputHandler::StopCropOutput(bool force)
{
	UNUSED_PARAMETER(force);

	if (!cropOutput && !cropCanvas && !cropVideoEncoder && !cropService)
		return;

	// Always force-stop: the output is about to be released, and its stop
	// path must have finished before the video/encoder it references goes
	// away (same reasoning as StopP2PCompanion).
	if (cropOutput)
		obs_output_force_stop(cropOutput);
	cropOutput = nullptr;
	cropVideoEncoder = nullptr;
	cropService = nullptr;

	if (cropCanvas)
		obs_canvas_set_channel(cropCanvas, 0, nullptr);
	cropCanvas = nullptr;
	cropScene = nullptr;
}

BasicOutputHandler::~BasicOutputHandler()
{
	// The crop publish references a private canvas/encoder owned here, so it
	// must be torn down before those members are destroyed.
	StopCropOutput(true);
}

BasicOutputHandler::BasicOutputHandler(OBSBasic *main_) : main(main_)
{
	if (main->vcamEnabled) {
		virtualCam = obs_output_create(VIRTUAL_CAM_ID, "virtualcam_output", nullptr, nullptr);

		signal_handler_t *signal = obs_output_get_signal_handler(virtualCam);
		startVirtualCam.Connect(signal, "start", OBSStartVirtualCam, this);
		stopVirtualCam.Connect(signal, "stop", OBSStopVirtualCam, this);
		deactivateVirtualCam.Connect(signal, "deactivate", OBSDeactivateVirtualCam, this);
	}

	auto service = main_->GetService();
	OBSDataAutoRelease settings = obs_service_get_settings(service);
	auto multitrack_enabled = config_get_bool(main->Config(), "Stream1", "EnableMultitrackVideo") &&
				  (obs_data_has_user_value(settings, "multitrack_video_configuration_url") ||
				   strcmp(obs_service_get_id(service), "rtmp_custom") == 0);

	if (multitrack_enabled)
		multitrackVideo = make_unique<MultitrackVideoOutput>();

	const char *service_protocol = obs_service_get_protocol(service);
	const bool is_whip = service_protocol && astrcmpi(service_protocol, "WHIP") == 0;
	const bool is_srt_custom = service_protocol && astrcmpi(service_protocol, "SRT") == 0 &&
				   strcmp(obs_service_get_id(service), "rtmp_custom") == 0;
	// ERTMP (the custom RTMP service, i.e. an rtmp:// rtmp_custom publish)
	// carries the same multitrack ladder as WHIP/SRT: the ABR layers ride
	// the Enhanced RTMP multitrack messages, so a single ERTMP connection is
	// enough for the main track and its ladder. rtmp_common (Twitch and
	// friends) is deliberately excluded - it never gets a PPCDN ladder.
	const bool is_rtmp_custom = service_protocol && astrcmpi(service_protocol, "rtmp") == 0 &&
				    strcmp(obs_service_get_id(service), "rtmp_custom") == 0;
	const bool ladder_transport = is_whip || is_srt_custom || is_rtmp_custom;
	const int configured_layers = config_get_int(main->Config(), "Stream1", "WHIPSimulcastTotalLayers");
	if (ladder_transport && configured_layers > 1)
		whipSimulcastEncoders = make_unique<WHIPSimulcastEncoders>(4);

	// Same transport gate as the Simulcast encoders above. When both H264
	// and HEVC are enabled the two ladders can't share one publish - the
	// ingest rejects a video-codec mix on a single connection - so they go
	// out as two connections (the HEVC half on its own /hevc path), exactly
	// as for custom SRT. See BuildCodecServiceSettings/SetupCompanionStream.
	const bool hevc_multitrack = ladder_transport &&
				     config_get_bool(main->Config(), "Stream1", "WHIPHevcH264Multitrack");
	if (hevc_multitrack)
		whipHevcEncoders = make_unique<WHIPHevcEncoders>();

	// With multitrack off, a custom SRT publish whose Server URL pins a codec
	// (/hevc or /h264) uses that codec. A URL with no segment follows the
	// configured stream encoder instead, and SetupCompanionStream appends the
	// matching /h264 or /hevc to the publish path. Derived here (rather than
	// per publish) so every output handler picks the same encoder at
	// construction. See urlPublishCodec's declaration.
	if (is_srt_custom && !hevc_multitrack) {
		const char *server = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
		urlPublishCodec = PublishCodecFromURL(server ? server : "");
	}
}

std::string BasicOutputHandler::EffectiveStreamEncoderId(const std::string &configuredId) const
{
	if (urlPublishCodec.empty())
		return configuredId;

	const char *configuredCodec = obs_get_encoder_codec(configuredId.c_str());
	if (configuredCodec && urlPublishCodec == configuredCodec)
		return configuredId;

	std::string resolved = (urlPublishCodec == "hevc") ? ResolveWHIPHevcEncoderId(configuredId)
							  : ResolveWHIPH264EncoderId(configuredId);
	if (!resolved.empty())
		return resolved;

	blog(LOG_WARNING,
	     "[PPCDN] publish URL is pinned to %s but no matching encoder is available; publishing with '%s' instead",
	     urlPublishCodec.c_str(), configuredId.c_str());
	return configuredId;
}

extern void log_vcam_changed(const VCamConfig &config, bool starting);

bool BasicOutputHandler::StartVirtualCam()
{
	if (!main->vcamEnabled)
		return false;

	bool typeIsProgram = main->vcamConfig.type == VCamOutputType::ProgramView;

	if (!virtualCamView && !typeIsProgram)
		virtualCamView = obs_view_create();

	UpdateVirtualCamOutputSource();

	if (!virtualCamVideo) {
		virtualCamVideo = typeIsProgram ? obs_get_video() : obs_view_add(virtualCamView);

		if (!virtualCamVideo)
			return false;
	}

	obs_output_set_media(virtualCam, virtualCamVideo, obs_get_audio());
	if (!Active())
		SetupOutputs();

	bool success = obs_output_start(virtualCam);
	if (!success) {
		QString errorReason;

		const char *error = obs_output_get_last_error(virtualCam);
		if (error) {
			errorReason = QT_UTF8(error);
		} else {
			errorReason = QTStr("Output.StartFailedGeneric");
		}

		QMessageBox::critical(main, QTStr("Output.StartVirtualCamFailed"), errorReason);

		DestroyVirtualCamView();
	}

	log_vcam_changed(main->vcamConfig, true);

	return success;
}

void BasicOutputHandler::StopVirtualCam()
{
	if (main->vcamEnabled) {
		obs_output_stop(virtualCam);
	}
}

bool BasicOutputHandler::VirtualCamActive() const
{
	if (main->vcamEnabled) {
		return obs_output_active(virtualCam);
	}
	return false;
}

void BasicOutputHandler::UpdateVirtualCamOutputSource()
{
	if (!main->vcamEnabled || !virtualCamView)
		return;

	OBSSourceAutoRelease source;

	switch (main->vcamConfig.type) {
	case VCamOutputType::Invalid:
	case VCamOutputType::ProgramView:
		DestroyVirtualCameraScene();
		return;
	case VCamOutputType::PreviewOutput: {
		DestroyVirtualCameraScene();
		OBSSource s = main->GetCurrentSceneSource();
		obs_source_get_ref(s);
		source = s.Get();
		break;
	}
	case VCamOutputType::SceneOutput:
		DestroyVirtualCameraScene();
		source = obs_get_source_by_name(main->vcamConfig.scene.c_str());
		break;
	case VCamOutputType::SourceOutput:
		OBSSourceAutoRelease s = obs_get_source_by_name(main->vcamConfig.source.c_str());

		if (!vCamSourceScene)
			vCamSourceScene = obs_scene_create_private("vcam_source");
		source = obs_source_get_ref(obs_scene_get_source(vCamSourceScene));

		if (vCamSourceSceneItem && (obs_sceneitem_get_source(vCamSourceSceneItem) != s)) {
			obs_sceneitem_remove(vCamSourceSceneItem);
			vCamSourceSceneItem = nullptr;
		}

		if (!vCamSourceSceneItem) {
			vCamSourceSceneItem = obs_scene_add(vCamSourceScene, s);

			obs_sceneitem_set_bounds_type(vCamSourceSceneItem, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(vCamSourceSceneItem, OBS_ALIGN_CENTER);

			const struct vec2 size = {
				(float)obs_source_get_width(source),
				(float)obs_source_get_height(source),
			};
			obs_sceneitem_set_bounds(vCamSourceSceneItem, &size);
		}
		break;
	}

	OBSSourceAutoRelease current = obs_view_get_source(virtualCamView, 0);
	if (source != current)
		obs_view_set_source(virtualCamView, 0, source);
}

void BasicOutputHandler::DestroyVirtualCamView()
{
	if (main->vcamConfig.type == VCamOutputType::ProgramView) {
		virtualCamVideo = nullptr;
		return;
	}

	obs_view_remove(virtualCamView);
	obs_view_set_source(virtualCamView, 0, nullptr);
	virtualCamVideo = nullptr;

	obs_view_destroy(virtualCamView);
	virtualCamView = nullptr;

	DestroyVirtualCameraScene();
}

void BasicOutputHandler::DestroyVirtualCameraScene()
{
	if (!vCamSourceScene)
		return;

	obs_scene_release(vCamSourceScene);
	vCamSourceScene = nullptr;
	vCamSourceSceneItem = nullptr;
}

const char *FindAudioEncoderFromCodec(const char *type)
{
	const char *alt_enc_id = nullptr;
	size_t i = 0;

	while (obs_enum_encoder_types(i++, &alt_enc_id)) {
		const char *codec = obs_get_encoder_codec(alt_enc_id);
		if (strcmp(type, codec) == 0) {
			return alt_enc_id;
		}
	}

	return nullptr;
}

void clear_archive_encoder(obs_output_t *output, const char *expected_name)
{
	obs_encoder_t *last = obs_output_get_audio_encoder(output, 1);
	bool clear = false;

	/* ensures that we don't remove twitch's soundtrack encoder */
	if (last) {
		const char *name = obs_encoder_get_name(last);
		clear = name && strcmp(name, expected_name) == 0;
		obs_encoder_release(last);
	}

	if (clear)
		obs_output_set_audio_encoder(output, nullptr, 1);
}

void BasicOutputHandler::SetupAutoRemux(const char *&container)
{
	bool autoRemux = config_get_bool(main->Config(), "Video", "AutoRemux");
	if (autoRemux && strcmp(container, "mp4") == 0)
		container = "mkv";
}

std::string BasicOutputHandler::GetRecordingFilename(const char *path, const char *container, bool noSpace,
						     bool overwrite, const char *format, bool ffmpeg)
{
	if (!ffmpeg)
		SetupAutoRemux(container);

	string dst = GetOutputFilename(path, container, noSpace, overwrite, format);
	lastRecordingPath = dst;
	return dst;
}

extern std::string DeserializeConfigText(const char *text);

std::shared_future<void> BasicOutputHandler::SetupMultitrackVideo(obs_service_t *service, std::string audio_encoder_id,
								  size_t main_audio_mixer,
								  std::optional<size_t> vod_track_mixer,
								  std::function<void(std::optional<bool>)> continuation)
{
	auto start_streaming_guard = std::make_shared<StartMultitrackVideoStreamingGuard>();
	if (!multitrackVideo) {
		continuation(std::nullopt);
		return start_streaming_guard->GetFuture();
	}

	multitrackVideoActive = false;

	streamDelayStarting.Disconnect();
	streamStopping.Disconnect();
	startStreaming.Disconnect();
	stopStreaming.Disconnect();

	bool is_custom = strncmp("rtmp_custom", obs_service_get_type(service), 11) == 0;

	std::optional<std::string> custom_config = std::nullopt;
	if (config_get_bool(main->Config(), "Stream1", "MultitrackVideoConfigOverrideEnabled"))
		custom_config = DeserializeConfigText(
			config_get_string(main->Config(), "Stream1", "MultitrackVideoConfigOverride"));

	std::optional<QString> extraCanvasUUID;
	const char *uuid = config_get_string(main->Config(), "Stream1", "MultitrackExtraCanvas");
	if (uuid && *uuid) {
		extraCanvasUUID = uuid;
	}

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	QString key = obs_data_get_string(settings, "key");

	const char *service_name = "<unknown>";
	if (is_custom && obs_data_has_user_value(settings, "service_name")) {
		service_name = obs_data_get_string(settings, "service_name");
	} else if (!is_custom) {
		service_name = obs_data_get_string(settings, "service");
	}

	std::optional<std::string> custom_rtmp_url;
	std::optional<bool> use_rtmps;
	auto server = obs_data_get_string(settings, "server");
	if (strncmp(server, "auto", 4) != 0) {
		custom_rtmp_url = server;
	} else {
		QString server_ = server;
		use_rtmps = server_.contains("rtmps", Qt::CaseInsensitive);
	}

	auto service_custom_server = obs_data_get_bool(settings, "using_custom_server");
	if (custom_rtmp_url.has_value()) {
		blog(LOG_INFO, "Using %sserver '%s'", service_custom_server ? "custom " : "", custom_rtmp_url->c_str());
	}

	auto maximum_aggregate_bitrate =
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto")
			? std::nullopt
			: std::make_optional<uint32_t>(
				  config_get_int(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrate"));

	auto maximum_video_tracks = config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracksAuto")
					    ? std::nullopt
					    : std::make_optional<uint32_t>(config_get_int(
						      main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracks"));

	auto stream_dump_config = GenerateMultitrackVideoStreamDumpConfig();

	auto continue_on_main_thread = [&, start_streaming_guard, service = OBSService{service},
					continuation =
						std::move(continuation)](std::optional<MultitrackVideoError> error) {
		if (error) {
			OBSDataAutoRelease service_settings = obs_service_get_settings(service);
			auto multitrack_video_name = QTStr("Basic.Settings.Stream.MultitrackVideoLabel");
			if (obs_data_has_user_value(service_settings, "multitrack_video_name")) {
				multitrack_video_name = obs_data_get_string(service_settings, "multitrack_video_name");
			}

			multitrackVideoActive = false;
			if (!error->ShowDialog(main, multitrack_video_name))
				return continuation(false);
			return continuation(std::nullopt);
		}

		multitrackVideoActive = true;

		auto signal_handler = multitrackVideo->StreamingSignalHandler();

		streamDelayStarting.Connect(signal_handler, "starting", OBSStreamStarting, this);
		streamStopping.Connect(signal_handler, "stopping", OBSStreamStopping, this);

		startStreaming.Connect(signal_handler, "start", OBSStartStreaming, this);
		stopStreaming.Connect(signal_handler, "stop", OBSStopStreaming, this);
		return continuation(true);
	};

	QThreadPool::globalInstance()->start([=, main = main, multitrackVideo = multitrackVideo.get(),
					      service_name = std::string{service_name}, service = OBSService{service},
					      stream_dump_config = OBSData{stream_dump_config},
					      start_streaming_guard = start_streaming_guard]() mutable {
		std::optional<MultitrackVideoError> error;
		try {
			multitrackVideo->PrepareStreaming(main, service_name.c_str(), service, custom_rtmp_url, key,
							  audio_encoder_id.c_str(), maximum_aggregate_bitrate,
							  maximum_video_tracks, custom_config, stream_dump_config,
							  main_audio_mixer, vod_track_mixer, use_rtmps,
							  extraCanvasUUID);
		} catch (const MultitrackVideoError &error_) {
			error.emplace(error_);
		}

		QMetaObject::invokeMethod(main, [=] { continue_on_main_thread(error); });
	});

	return start_streaming_guard->GetFuture();
}

OBSDataAutoRelease BasicOutputHandler::GenerateMultitrackVideoStreamDumpConfig()
{
	auto stream_dump_enabled = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpEnabled");

	if (!stream_dump_enabled)
		return nullptr;

	const char *path = config_get_string(main->Config(), "SimpleOutput", "FilePath");
	bool noSpace = config_get_bool(main->Config(), "SimpleOutput", "FileNameWithoutSpace");
	const char *filenameFormat = config_get_string(main->Config(), "Output", "FilenameFormatting");
	bool overwriteIfExists = config_get_bool(main->Config(), "Output", "OverwriteIfExists");
	bool useMP4 = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpAsMP4");

	string f;

	OBSDataAutoRelease settings = obs_data_create();
	f = GetFormatString(filenameFormat, nullptr, nullptr);
	string strPath = GetRecordingFilename(path, useMP4 ? "mp4" : "flv", noSpace, overwriteIfExists, f.c_str(),
					      // never remux stream dump
					      false);
	obs_data_set_string(settings, "path", strPath.c_str());

	if (useMP4) {
		obs_data_set_bool(settings, "use_mp4", true);
		obs_data_set_string(settings, "muxer_settings", "write_encoder_info=1");
	}

	return settings;
}

BasicOutputHandler *CreateSimpleOutputHandler(OBSBasic *main)
{
	return new SimpleOutput(main);
}

BasicOutputHandler *CreateAdvancedOutputHandler(OBSBasic *main)
{
	return new AdvancedOutput(main);
}
