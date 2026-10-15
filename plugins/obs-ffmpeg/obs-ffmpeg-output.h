#pragma once

#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#ifdef NEW_MPEGTS_OUTPUT
#include "obs-ffmpeg-url.h"
#include "publish-loss-reconnect.h"
#endif

struct ffmpeg_cfg {
	const char *url;
	const char *format_name;
	const char *format_mime_type;
	const char *muxer_settings;
	const char *protocol_settings; // not used yet for SRT nor RIST
	int gop_size;
	int video_bitrate;
	int audio_bitrate;
	const char *video_encoder;
	int video_encoder_id;
	const char *video_encoders[MAX_OUTPUT_VIDEO_ENCODERS];
	int video_bitrates[MAX_OUTPUT_VIDEO_ENCODERS];
	int video_gop_sizes[MAX_OUTPUT_VIDEO_ENCODERS];
	const char *audio_encoder;
	int audio_encoder_id;
	int audio_bitrates[MAX_AUDIO_MIXES]; // multi-track
	const char *video_settings;
	const char *audio_settings;
	int audio_mix_count;
	int audio_tracks;
	const char *audio_stream_names[MAX_AUDIO_MIXES];
	enum AVPixelFormat format;
	enum AVColorRange color_range;
	enum AVColorPrimaries color_primaries;
	enum AVColorTransferCharacteristic color_trc;
	enum AVColorSpace colorspace;
	int max_luminance;
	int scale_width;
	int scale_height;
	int width;
	int height;
	int frame_size; // audio frame size
	const char *username;
	const char *password;
	const char *stream_id;
	const char *encrypt_passphrase;
	bool is_srt;
	bool is_rist;
	// ERTSP (Enhanced RTSP) publish - unlike is_srt/is_rist, this never
	// triggers the custom URLContext/AVIOContext path (connect_mpegts_url,
	// loss monitoring, degrade WS companion): it goes through the same
	// generic avio_open2() path as plain udp/tcp/http (see
	// proto_is_allowed), letting avformat's own "rtsp" protocol/muxer
	// handle the ANNOUNCE/RECORD handshake and RTP packetization, same as
	// a plain "ffmpeg -f rtsp rtsp://user:pass@host/path" publish. Only
	// used to pick the output format ("rtsp" instead of "mpegts" - see
	// ffmpeg_mpegts_data_init) and to admit the URL in proto_is_allowed.
	bool is_rtsp;
	int srt_pkt_size;
#ifdef NEW_MPEGTS_OUTPUT
	/* Degrade WS control-channel credentials for this publish's codec, read
	 * from service settings ("ppcdn_degrade_ws_url_h264" etc., written by
	 * ResolvePPCDNPublishEndpoint - see srt-degrade-client.h). Only one pair
	 * is actually used per connection (picked by video_encoder once it's
	 * known), but both are fetched up front in fetch_service_info() since
	 * setup_video_settings() runs after it and resolves the codec. Empty
	 * when the Origin has no WebRTC base URL configured - that connection
	 * just runs without a degrade channel, same as before this existed. */
	const char *degrade_ws_url_h264;
	const char *degrade_ws_token_h264;
	const char *degrade_ws_url_hevc;
	const char *degrade_ws_token_hevc;
#endif
};

struct ffmpeg_audio_info {
	AVStream *stream;
	AVCodecContext *ctx;
};

struct ffmpeg_video_info {
	AVStream *stream;
	AVCodecContext *ctx;
};

struct ffmpeg_data {
	/* Used by the generic FFmpeg output. */
	AVStream *video;
	AVCodecContext *video_ctx;
	/* Used by the native MPEG-TS output. */
	struct ffmpeg_video_info video_infos[MAX_OUTPUT_VIDEO_ENCODERS];
	bool video_started[MAX_OUTPUT_VIDEO_ENCODERS];
	struct ffmpeg_audio_info *audio_infos;
	const AVCodec *acodec;
	const AVCodec *vcodec;
	AVFormatContext *output;
	struct SwsContext *swscale;

	int64_t total_frames;
	AVFrame *vframe;
	int frame_size;

	uint64_t start_timestamp;

	int64_t total_samples[MAX_AUDIO_MIXES];
	uint32_t audio_samplerate;
	enum audio_format audio_format;
	size_t audio_planes;
	size_t audio_size;
	int num_audio_streams;

	/* audio_tracks is a bitmask storing the indices of the mixes */
	int audio_tracks;
	struct deque excess_frames[MAX_AUDIO_MIXES][MAX_AV_PLANES];
	uint8_t *samples[MAX_AUDIO_MIXES][MAX_AV_PLANES];
	AVFrame *aframe[MAX_AUDIO_MIXES];

	struct ffmpeg_cfg config;

	bool initialized;

	char *last_error;
};

struct ffmpeg_output {
	obs_output_t *output;
	volatile bool active;
	struct ffmpeg_data ff_data;

	bool connecting;
	pthread_t start_thread;

	uint64_t total_bytes;

	uint64_t audio_start_ts;
	uint64_t video_start_ts;
	uint64_t stop_ts;
	volatile bool stopping;

	bool write_thread_active;
	pthread_mutex_t write_mutex;
	pthread_t write_thread;
	os_sem_t *write_sem;
	os_event_t *stop_event;

	DARRAY(AVPacket *) packets;
#ifdef NEW_MPEGTS_OUTPUT
	/* used for SRT & RIST */
	URLContext *h;
	AVIOContext *s;
	bool got_headers;

	volatile bool running;

	pthread_t start_stop_thread;
	pthread_mutex_t start_stop_mutex;
	volatile bool start_stop_thread_active;
	bool has_connected;

	/* SRT publish loss-reconnect: samples srt_bstats every second and feeds
	 * the shared window policy; see obs-ffmpeg-mpegts.c's
	 * mpegts_sample_srt_loss(). */
	struct publish_loss_monitor loss_monitor;
	int64_t loss_next_read_ms;
	int64_t loss_prev_sent;
	int64_t loss_prev_snd_loss;
	int64_t loss_prev_retrans;
	bool loss_stats_primed;
#endif
};

#ifdef NEW_MPEGTS_OUTPUT
enum mpegts_cmd_type { MPEGTS_CMD_START, MPEGTS_CMD_STOP };

struct mpegts_cmd {
	enum mpegts_cmd_type type;
	bool signal_stop;
	struct ffmpeg_output *stream;
	uint64_t ts;
};
#endif
bool ffmpeg_data_init(struct ffmpeg_data *data, struct ffmpeg_cfg *config);
void ffmpeg_data_free(struct ffmpeg_data *data);
