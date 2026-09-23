#include "BasicOutputHandler.hpp"
#include "AdvancedOutput.hpp"
#include "SimpleOutput.hpp"

#include <abs-ts.h>
#include <utility/MultitrackVideoError.hpp>
#include <utility/PublishCodecCheck.hpp>
#include <utility/StartMultiTrackVideoStreamingGuard.hpp>
#include <utility/VCamConfig.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QThreadPool>

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
	return settings;
}

void BasicOutputHandler::SetupCompanionStream(const char *outputType)
{
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

bool BasicOutputHandler::EnforceNoBFrames(obs_encoder_t *videoEncoder)
{
	if (!videoEncoder)
		return true;

	// WebRTC (the P2P leg) cannot consume B-frames. whip_custom forces this
	// via apply_encoder_settings, but the SRT rtmp_custom publish path does
	// not, and Advanced mode can skip service settings entirely - so force
	// bf=0 here for every PPCDN publish regardless of transport. Merged into
	// the encoder's existing settings (obs_encoder_update applies on top).
	OBSDataAutoRelease force = obs_data_create();
	obs_data_set_int(force, "bf", 0);
	obs_encoder_update(videoEncoder, force);

	OBSDataAutoRelease applied = obs_encoder_get_settings(videoEncoder);
	if (obs_data_get_int(applied, "bf") == 0)
		return true;

	// The encoder clamped or rejected bf=0 (e.g. a preset that mandates
	// B-frames, or a custom option overriding it). The main SRT/WHIP publish
	// still works; only P2P will not - tell the user to disable B-frames.
	blog(LOG_WARNING, "[PPCDN] streaming video encoder did not accept bf=0; P2P needs H.264 without B-frames");
	OBSBasic *m = main;
	QMetaObject::invokeMethod(
		m,
		[m]() {
			OBSMessageBox::warning(
				m, QStringLiteral("P2P 未启用：编码器存在 B 帧"),
				QStringLiteral(
					"当前视频编码器无法设置为 0 个 B 帧（bf=0）。P2P 需要无 B 帧的 H.264 编码。\n\n"
					"请到 设置 → 输出 把该编码器的 “B 帧 / bf” 数量改为 0，否则本次推流不会启用 P2P（仅走 edge）。"));
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

	p2pOutput = obs_output_create("ppcenter_p2p_output", "ppcenter_p2p", nullptr, nullptr);
	if (!p2pOutput) {
		blog(LOG_WARNING, "[PPCDN] P2P companion: failed to create ppcenter_p2p_output");
		return;
	}
	// Shares the primary's service (carries the ppcenter creds for both WHIP
	// and SRT) and the same H264/audio encoders the main publish uses - the
	// P2P output only reads encoded packets, it opens no WHIP media session.
	obs_output_set_service(p2pOutput, primary);
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
	}
}

void BasicOutputHandler::StopP2PCompanion()
{
	if (!p2pOutput)
		return;
	obs_output_stop(p2pOutput);
	p2pOutput = nullptr;
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
	return total;
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
	const int configured_layers = config_get_int(main->Config(), "Stream1", "WHIPSimulcastTotalLayers");
	if ((is_whip || is_srt_custom) && configured_layers > 1)
		whipSimulcastEncoders = make_unique<WHIPSimulcastEncoders>(4);

	// Same protocol gate as the Simulcast encoders above: only WHIP and
	// custom SRT can carry a second codec's ladder. Without it the HEVC
	// encoders were created for every service and attached to the output,
	// which an RTMP/MPEG-TS muxer then rejected outright.
	if ((is_whip || is_srt_custom) && config_get_bool(main->Config(), "Stream1", "WHIPHevcH264Multitrack"))
		whipHevcEncoders = make_unique<WHIPHevcEncoders>();
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
