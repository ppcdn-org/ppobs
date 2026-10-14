#pragma once

#include <obs.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

/*
 * Protocol-independent full-reference quality score for the outgoing
 * stream.
 *
 * Answers "how much picture quality does the encode cost?" for ANY output
 * (SRT, RTMP, WHIP) by decoding the output's top layer's packets right
 * back (FFmpeg avcodec) and comparing each decoded frame against the exact
 * program frame that produced it - the same composited feed the encoders
 * see, tapped via obs_add_raw_video_callback2. The program feed is by
 * definition the 100-point reference; the score is mean Y-SSIM x 100, so a
 * lossless encode scores 100 and heavier compression scores progressively
 * lower. PSNR is logged alongside. One log line every 10 seconds.
 *
 * This used to live in the obs-webrtc plugin and only scored WHIP because
 * it was fed from WHIPOutput::Data(). It is now a standalone component: it
 * registers its OWN obs_output_add_packet_callback on the output passed to
 * Start(), so it works for every output type (the frontend attaches it to
 * whichever output is publishing). The measurement itself was always
 * transport-independent - it compares the encoder's input to the encoder's
 * output - only the wiring used to be WHIP-specific.
 *
 * Frame matching: encoder_packet.dts_usec is in the output's own
 * interleaving-relative clock (see obs-output.c
 * apply_interleaved_packet_offset - it zeroes dts/pts against this
 * particular output's start, not the raw-frame clock the reference tap
 * uses), so it's translated to the reference tap's clock via a
 * self-calibrating offset (see clock_offset_us below). That offset is
 * estimated from the minimum observed one-way delay, which is only
 * unbiased if the minimum encode pipeline latency is ~0; encoders that
 * buffer (e.g. NVENC lookahead) add a constant delay, so the corrected
 * content time is systematically ahead of the true frame. To stay correct
 * regardless, the comparison searches a bounded window of references for
 * the best-matching one (see CompareFrame) rather than taking the nearest
 * by timestamp.
 *
 * Both sides are downscaled to the same size (the SSIM-standard factor
 * round(min(w,h)/256), with the same fast-bilinear kernel libobs uses
 * for the reference tap) before comparison, so the per-sample SSIM is
 * trivial; the real cost is the continuous decode of the stream's own
 * top layer, roughly 5-10% of one core for 1080p H.264.
 */
class QualityScorer {
public:
	// Per-sample score sink, invoked on the scorer's decode thread. Kept as
	// a plain function pointer + param (not std::function) so SetSink
	// before Start is race-free against the running thread.
	using ScoreSink = void (*)(void *param, float score, float psnr);

	~QualityScorer();

	// Install the per-sample sink. Call before Start().
	void SetSink(ScoreSink cb, void *param)
	{
		sink = cb;
		sink_param = param;
	}

	// Call after the output's encoders are initialized (e.g. from the
	// output's "start" signal). Reads layer 0's codec and dimensions,
	// registers the packet tap and the reference tap. No-op if already
	// started or the codec has no decoder available.
	void Start(obs_output_t *output);
	void Stop();

private:
	static void PacketTap(obs_output_t *output, struct encoder_packet *packet,
			      struct encoder_packet_time *pkt_time, void *param);
	// Called from the packet tap for every video packet; filters to the
	// scored (top) layer internally. Copies the packet and returns.
	void OnPacket(struct encoder_packet *packet);

	static void RawVideo(void *param, struct video_data *frame);
	void DecodeLoop();
	void DrainDecoder();
	void CompareFrame(AVFrame *frame);

	struct RefFrame {
		int64_t ts_us;
		std::vector<uint8_t> luma; // packed det_width x det_height
	};

	ScoreSink sink = nullptr;
	void *sink_param = nullptr;

	// Reference ring: written by the raw video callback (video thread),
	// consumed by the decode thread.
	std::mutex ref_mutex;
	std::deque<RefFrame> refs;

	// Packet queue: written by PacketTap (encoder thread), drained by the
	// decode thread. wait_keyframe lives under the same lock because an
	// overflow resync (clear queue, wait for next IDR) touches both.
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::deque<AVPacket *> queue;
	bool wait_keyframe = true;
	bool running = false;

	std::atomic<bool> started{false};
	std::atomic<bool> dead{false}; // decoder gave up (persistent errors)
	std::thread decode_thread;

	const obs_encoder_t *target_encoder = nullptr;
	obs_output_t *score_output = nullptr; // valid while started; for the packet tap + raw callback

	// packet->dts_usec is NOT the same clock as the reference tap's
	// frame->timestamp: outputs with both audio and video encoders go
	// through libobs's interleaving path (obs-output.c
	// apply_interleaved_packet_offset), which rewrites dts_usec to be
	// relative to this output's own start rather than the absolute
	// os_gettime_ns()-based clock the raw video callback uses. Both
	// clocks tick at the same real rate, just offset by an unknown
	// constant, so it's recovered by comparing each packet's dts_usec
	// against os_gettime_ns() at arrival time (PacketTap, encoder
	// thread) and keeping the running minimum - the same
	// minimum-filter trick NTP uses to estimate one-way delay: arrival
	// time is (true content time + encode/delivery latency), and
	// latency has a hard lower bound but only ever adds jitter above
	// it, so the minimum observed gap converges on the true constant
	// offset. Written from PacketTap(), read from CompareFrame()
	// (decode thread) - atomic for cross-thread visibility.
	std::atomic<int64_t> clock_offset_us{INT64_MAX};
	uint32_t det_width = 0;
	uint32_t det_height = 0;
	int64_t frame_dur_us = 16667;

	AVCodecContext *decoder = nullptr;
	SwsContext *sws = nullptr;
	AVFrame *dec_frame = nullptr;

	// Downscale destination planes (decode thread only). U/V are scaled
	// too because sws wants complete YUV420P output, but only Y is
	// compared.
	std::vector<uint8_t> det_y, det_u, det_v;

	// Scoring window (decode thread only, except resyncs).
	int64_t next_sample_us = 0;
	int64_t next_report_us = 0;
	uint32_t win_samples = 0;
	double win_ssim_sum = 0.0;
	double win_ssim_min = 1.0;
	double win_psnr_sum = 0.0;
	// Running sum of (corrected content time - matched reference time) in ms,
	// i.e. the observed encoder pipeline delay (the clock offset's bias).
	// Logged so a drift in frame alignment is visible.
	double win_align_sum_ms = 0.0;
	uint32_t win_misses = 0;
	uint32_t win_decode_errors = 0;
	std::atomic<uint32_t> resyncs{0};
	uint32_t reported_resyncs = 0;
	uint32_t consecutive_errors = 0;
};
