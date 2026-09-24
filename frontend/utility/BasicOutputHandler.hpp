#pragma once

#include <utility/MultitrackVideoOutput.hpp>
#include <utility/WHIPSimulcastEncoders.hpp>
#include <utility/WHIPHevcEncoders.hpp>

#include <obs.hpp>
#include <util/dstr.hpp>

#include <future>

#define RTMP_PROTOCOL "rtmp"
#define SRT_PROTOCOL "srt"
#define RIST_PROTOCOL "rist"

class OBSBasic;

using SetupStreamingContinuation_t = std::function<void(bool)>;

struct BasicOutputHandler {
	OBSOutputAutoRelease fileOutput;
	OBSOutputAutoRelease streamOutput;
	OBSOutputAutoRelease replayBuffer;
	OBSOutputAutoRelease virtualCam;
	bool streamingActive = false;
	bool recordingActive = false;
	bool delayActive = false;
	bool replayBufferActive = false;
	bool virtualCamActive = false;
	OBSBasic *main;

	std::unique_ptr<MultitrackVideoOutput> multitrackVideo;
	bool multitrackVideoActive = false;

	OBSOutputAutoRelease StreamingOutput() const
	{
		return (multitrackVideo && multitrackVideoActive)
			       ? multitrackVideo->StreamingOutput()
			       : OBSOutputAutoRelease{obs_output_get_ref(streamOutput)};
	}

	obs_view_t *virtualCamView = nullptr;
	video_t *virtualCamVideo = nullptr;
	obs_scene_t *vCamSourceScene = nullptr;
	obs_sceneitem_t *vCamSourceSceneItem = nullptr;

	std::unique_ptr<WHIPSimulcastEncoders> whipSimulcastEncoders;
	// Only constructed when the WHIP service has HEVC/H264 multitrack
	// enabled (Stream1.WHIPHevcH264Multitrack) - see
	// docs/design/whip-hevc-h264-multitrack-simulcast-design.zh-CN.md
	// §4.1 and this member's Create() call sites in
	// AdvancedOutput.cpp/SimpleOutput.cpp.
	std::unique_ptr<WHIPHevcEncoders> whipHevcEncoders;

	// Second publish of the same scene carrying the HEVC ladder, used when
	// multitrack is on and the transport can only carry one video codec per
	// connection (SRT/MPEG-TS). streamOutput keeps the H264 ladder and
	// stays the "primary" one that the status bar, frontend API and stats
	// all follow, so none of them need to learn about a second output; this
	// one is started and stopped alongside it.
	//
	// It needs its own service because the mpegts output reads its URL from
	// whatever service is attached (see fetch_service_info in
	// obs-ffmpeg-mpegts.c), and the two publishes differ precisely in that
	// URL's codec segment.
	OBSOutputAutoRelease hevcStreamOutput;
	OBSServiceAutoRelease hevcStreamService;
	// The primary output's service while multitrack is on: the configured
	// service with /h264 appended, so both halves publish to their own
	// codec path without the user having to spell either one out.
	OBSServiceAutoRelease h264StreamService;
	OBSSignal hevcStopStreaming;

	// P2P companion output (ppcenter_p2p_output): started alongside a non-WHIP
	// (e.g. SRT) primary publish so that publish can still join the P2P mesh -
	// a WHIP publish runs P2P inside its own whip_output and needs none of
	// this. Shares the primary's H264/audio encoders (its own service, see
	// p2pService below). See
	// docs/design/ppobs-p2p-srt-publish-support.zh-CN.md.
	OBSOutputAutoRelease p2pOutput;
	// The P2P companion's own service. A service carries a single output
	// back-pointer (obs_output_set_service() unhooks whichever output owned
	// it before, see obs-output.c), so pointing the companion at the primary's
	// service would steal it from the primary output. The SRT/MPEG-TS output
	// reads its publish URL from exactly that back-pointer (observe
	// obs-ffmpeg-mpegts.c's fetch_service_info()), so sharing it makes the
	// primary start fail with OBS_OUTPUT_ERROR. Create an independent
	// instance from a copy of the settings instead. obs_output_set_service()
	// takes no reference, so this member must outlive p2pOutput.
	OBSServiceAutoRelease p2pService;
	// Set while StopStreaming is tearing the pair down on purpose, so the
	// companion's own "stop" signal isn't mistaken for a failure that
	// should stop the primary too.
	bool stoppingStreamPair = false;

	// Creates (or tears down) the companion publish to match the current
	// configuration. Safe to call repeatedly; outputType is the output id
	// the primary publish uses, which the companion shares.
	void SetupCompanionStream(const char *outputType);

	// Starts the companion publish, if one is configured. Returns false
	// only when a companion was wanted but couldn't be started - the caller
	// must then abandon the whole publish rather than go on air with half
	// of it (design §4.1's strong-consistency start).
	bool StartCompanionStream();
	void StopCompanionStream(bool force);

	// Forces videoEncoder to bf=0 (no B-frames) for a PPCDN publish. The P2P
	// WebRTC leg cannot consume B-frames; whip_custom forces this via
	// apply_encoder_settings but the SRT rtmp_custom path does not, so it is
	// enforced here for every transport. Returns false and warns the user (a
	// dialog on the GUI thread) if the encoder will not accept bf=0 - it does
	// NOT abort the main publish, which works with B-frames; only P2P needs
	// them off. See docs/design/ppobs-p2p-srt-publish-support.zh-CN.md.
	bool EnforceNoBFrames(obs_encoder_t *videoEncoder);

	// Starts/stops the P2P companion output (ppcenter_p2p_output) for a
	// non-WHIP primary publish, sharing videoEnc (must be H264) and audioEnc.
	// Best-effort: a P2P failure never affects the main publish. See p2pOutput
	// and docs/design/ppobs-p2p-srt-publish-support.zh-CN.md.
	void StartP2PCompanion(obs_encoder_t *videoEnc, obs_encoder_t *audioEnc);
	void StopP2PCompanion();

	// Bytes actually sent, combined across the primary streaming output
	// and - when HEVC/H264 multitrack is publishing over SRT/MPEG-TS,
	// where each codec is a separate obs_output_t - its hevcStreamOutput
	// companion. Despite the comment above on hevcStreamOutput ("none of
	// them need to learn about a second output"), callers that report
	// "how much has this stream sent" (the status bar's live kbps, the
	// Stats window) do need to: querying only StreamingOutput() undercounts
	// by exactly the HEVC ladder's bytes whenever the companion is active.
	// WHIP multitrack doesn't have this problem - WHIPOutput is a single
	// obs_output_t that already sums both codec sessions internally (see
	// WHIPOutput::GetTotalBytes in plugins/obs-webrtc/whip-output.h) - so
	// this only ever adds anything for the SRT/MPEG-TS companion case.
	uint64_t TotalStreamingBytes() const;

	std::string outputType;
	std::string lastError;

	// For a custom SRT publish with HEVC/H264 multitrack off, the Server
	// URL's codec segment decides the published codec: "/hevc" -> HEVC,
	// "/h264" or no segment -> H264 (PPCENTER treats a bare stream name as
	// the H264 stream). "h264"/"hevc", or empty when the URL doesn't apply
	// (WHIP, or multitrack is on and builds both ladders itself). See
	// EffectiveStreamEncoderId().
	std::string urlPublishCodec;

	// Returns the encoder id to publish with: the configured one when the
	// publish URL doesn't pin a codec (urlPublishCodec empty) or already
	// matches, otherwise the configured encoder family's counterpart for the
	// URL's codec (ResolveWHIPH264/HevcEncoderId). Falls back to the
	// configured id - with a warning - when no encoder for the URL's codec
	// is available.
	std::string EffectiveStreamEncoderId(const std::string &configuredId) const;

	std::string lastRecordingPath;

	OBSSignal startRecording;
	OBSSignal stopRecording;
	OBSSignal startReplayBuffer;
	OBSSignal stopReplayBuffer;
	OBSSignal startStreaming;
	OBSSignal stopStreaming;
	OBSSignal startVirtualCam;
	OBSSignal stopVirtualCam;
	OBSSignal deactivateVirtualCam;
	OBSSignal streamDelayStarting;
	OBSSignal streamStopping;
	OBSSignal recordStopping;
	OBSSignal recordFileChanged;
	OBSSignal replayBufferStopping;
	OBSSignal replayBufferSaved;

	BasicOutputHandler(OBSBasic *main_);

	virtual ~BasicOutputHandler() {};

	virtual std::shared_future<void> SetupStreaming(obs_service_t *service,
							SetupStreamingContinuation_t continuation) = 0;
	virtual bool StartStreaming(obs_service_t *service) = 0;
	virtual bool StartRecording() = 0;
	virtual bool StartReplayBuffer() { return false; }
	virtual bool StartVirtualCam();
	virtual void StopStreaming(bool force = false) = 0;
	virtual void StopRecording(bool force = false) = 0;
	virtual void StopReplayBuffer(bool force = false) { (void)force; }
	virtual void StopVirtualCam();
	virtual bool StreamingActive() const = 0;
	virtual bool RecordingActive() const = 0;
	virtual bool ReplayBufferActive() const { return false; }
	virtual bool VirtualCamActive() const;

	virtual void Update() = 0;
	virtual void SetupOutputs() = 0;

	virtual void UpdateVirtualCamOutputSource();
	virtual void DestroyVirtualCamView();
	virtual void DestroyVirtualCameraScene();

	inline bool Active() const
	{
		return streamingActive || recordingActive || delayActive || replayBufferActive || virtualCamActive ||
		       multitrackVideoActive;
	}

protected:
	void SetupAutoRemux(const char *&container);
	std::string GetRecordingFilename(const char *path, const char *container, bool noSpace, bool overwrite,
					 const char *format, bool ffmpeg);

	std::shared_future<void> SetupMultitrackVideo(obs_service_t *service, std::string audio_encoder_id,
						      size_t main_audio_mixer, std::optional<size_t> vod_track_mixer,
						      std::function<void(std::optional<bool>)> continuation);
	OBSDataAutoRelease GenerateMultitrackVideoStreamDumpConfig();
};

BasicOutputHandler *CreateSimpleOutputHandler(OBSBasic *main);
BasicOutputHandler *CreateAdvancedOutputHandler(OBSBasic *main);

void OBSStreamStarting(void *data, calldata_t *params);
void OBSStreamStopping(void *data, calldata_t *params);
void OBSStartStreaming(void *data, calldata_t *params);
void OBSStopStreaming(void *data, calldata_t *params);
void OBSStartRecording(void *data, calldata_t *params);
void OBSStopRecording(void *data, calldata_t *params);
void OBSRecordStopping(void *data, calldata_t *params);
void OBSRecordFileChanged(void *data, calldata_t *params);
void OBSStartReplayBuffer(void *data, calldata_t *params);
void OBSStopReplayBuffer(void *data, calldata_t *params);
void OBSReplayBufferStopping(void *data, calldata_t *params);
void OBSReplayBufferSaved(void *data, calldata_t *params);

inline bool can_use_output(const char *prot, const char *output, const char *prot_test1,
			   const char *prot_test2 = nullptr)
{
	return (strcmp(prot, prot_test1) == 0 || (prot_test2 && strcmp(prot, prot_test2) == 0)) &&
	       (obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0;
}

const char *GetStreamOutputType(const obs_service_t *service);

inline bool ServiceSupportsVodTrack(const char *service)
{
	static const char *vodTrackServices[] = {"Twitch"};

	for (const char *vodTrackService : vodTrackServices) {
		if (astrcmpi(vodTrackService, service) == 0)
			return true;
	}

	return false;
}

void clear_archive_encoder(obs_output_t *output, const char *expected_name);
