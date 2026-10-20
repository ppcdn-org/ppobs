/******************************************************************************
    PPCDN ERTSP (Enhanced RTSP) publish output.

    obs-deps' bundled FFmpeg has the "rtsp" network PROTOCOL compiled out
    (confirmed empirically: avio_enum_protocols() never returns "rtsp",
    while "tcp"/"rtmp"/"srt"/"rist" etc. are all present) even though the
    "rtsp" MUXER format is present. That muxer is also useless on its own
    for this purpose - av_guess_format("rtsp",...) ->flags has AVFMT_NOFILE
    set, meaning it manages its own connection internally via the very same
    missing "rtsp" protocol, so swapping in a custom AVIOContext at the
    outer avio_open2() layer (the trick obs-ffmpeg-mpegts.c uses for SRT/
    RIST) can't intercept it either. Both facts were verified directly
    against obs-deps' avformat-61.dll with a small ctypes probe before
    writing this file - see the ERTSP design docs for the exact readout.

    So this is a minimal RTSP publish CLIENT, hand-written against RFC 2326
    (RTSP), RFC 4566 (SDP) and RFC 6184 (H.264/RTP), cross-checked against
    ppmmx's gortsplib server (the actual receiving implementation) for exact
    wire compatibility - particularly gortsplib's "FFmpeg format" relative
    control-URL convention (server_session.go's findMediaByURL/
    getPathAndQueryAndTrackID) and its H264/Opus format parameters
    (pkg/format/h264.go, opus.go). It only needs the "tcp" protocol (which
    IS present) as a byte pipe for the RTSP control connection; everything
    above that - ANNOUNCE/SETUP/RECORD, SDP, RTP packetization - is this
    file's own code.

    Scope: H264 video + Opus audio only, matching ppmmx's ERTSP codec
    whitelist (H264/HEVC + Opus/AAC, exactly one video + one audio track,
    see ppmmx's publish_codec.go) minus HEVC/AAC, which are not yet
    implemented here - see the design docs for why this was scoped down to
    what obs is actually configured to encode with today.
******************************************************************************/

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/base.h>

#include <libavformat/avio.h>
#include <libavutil/base64.h>
#include <libavutil/avstring.h>
#include <libavutil/error.h>

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "obs-ffmpeg-compat.h"

#define do_log(level, format, ...) blog(level, "[ertsp_output: '%s']: " format, obs_output_get_name(s->output), ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug_log(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

#define ERTSP_MAX_RESPONSE 8192
#define ERTSP_VIDEO_PT 96
#define ERTSP_AUDIO_PT 97
#define ERTSP_CH_VIDEO_RTP 0
#define ERTSP_CH_VIDEO_RTCP 1
#define ERTSP_CH_AUDIO_RTP 2
#define ERTSP_CH_AUDIO_RTCP 3

struct ertsp_output {
	obs_output_t *output;

	pthread_t start_thread;
	bool start_thread_active;

	pthread_mutex_t write_mutex;
	volatile bool active;
	volatile bool connected;
	volatile bool stopping;

	/* parsed from the service's resolved rtsp:// URL */
	struct dstr host;
	int port;
	struct dstr auth_user;
	struct dstr auth_pass;
	struct dstr path; /* no leading slash, e.g. "appId/streamName" */

	AVIOContext *pb; /* the one TCP connection: RTSP control + both
			   * interleaved RTP/RTCP channels */
	int cseq;
	struct dstr session_id;
	bool authed; /* true once a 401 challenge has been answered once -
		      * a second 401 after that is a real auth failure, not
		      * "needs a challenge", see ertsp_announce(). */

	/* H264 */
	uint8_t *sps;
	size_t sps_size;
	uint8_t *pps;
	size_t pps_size;
	uint32_t video_ssrc;
	uint16_t video_seq;

	/* Opus */
	int audio_channels;
	int audio_samplerate;
	uint32_t audio_ssrc;
	uint16_t audio_seq;

	char last_error[256];
};

static void ertsp_set_error(struct ertsp_output *s, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(s->last_error, sizeof(s->last_error), fmt, args);
	va_end(args);
	warn("%s", s->last_error);
}

/* ------------------------------------------------------------------------- */
/* URL parsing: "rtsp://[user[:pass]@]host[:port]/path" - the URL ppcenter's
 * GenerateERTSPPublishCredential-style resolution always hands back (see
 * ppcenter's services.GenerateERTSPPublishCredential / publish_v1.go). No
 * percent-decoding: every credential ppcenter mints here is plain hex/
 * base64url text with nothing that needs escaping. */
static bool ertsp_parse_url(struct ertsp_output *s, const char *url)
{
	const char *p = url;
	if (astrcmpi_n(p, "rtsp://", 7) != 0) {
		ertsp_set_error(s, "URL must start with rtsp://: %s", url);
		return false;
	}
	p += 7;

	const char *at = strchr(p, '@');
	const char *slash = strchr(p, '/');
	if (at && (!slash || at < slash)) {
		const char *colon = memchr(p, ':', (size_t)(at - p));
		if (colon) {
			dstr_ncopy(&s->auth_user, p, (size_t)(colon - p));
			dstr_ncopy(&s->auth_pass, colon + 1, (size_t)(at - colon - 1));
		} else {
			dstr_ncopy(&s->auth_user, p, (size_t)(at - p));
		}
		p = at + 1;
	}

	const char *host_end = slash ? slash : p + strlen(p);
	const char *port_colon = memchr(p, ':', (size_t)(host_end - p));
	if (port_colon) {
		dstr_ncopy(&s->host, p, (size_t)(port_colon - p));
		s->port = atoi(port_colon + 1);
	} else {
		dstr_ncopy(&s->host, p, (size_t)(host_end - p));
		s->port = 554;
	}
	if (s->host.len == 0) {
		ertsp_set_error(s, "URL has no host: %s", url);
		return false;
	}

	if (slash && *(slash + 1))
		dstr_copy(&s->path, slash + 1);
	else
		dstr_copy(&s->path, "");

	return true;
}

/* ------------------------------------------------------------------------- */
/* RTSP control connection: a plain TCP byte pipe opened via ffmpeg's "tcp"
 * protocol (confirmed present - see this file's header comment), used only
 * to carry hand-written RTSP request/response text and, once SETUP/RECORD
 * succeed, interleaved "$<channel><len>"-framed RTP packets. */
static bool ertsp_connect_tcp(struct ertsp_output *s)
{
	struct dstr tcp_url = {0};
	dstr_printf(&tcp_url, "tcp://%s:%d?timeout=10000000", s->host.array, s->port);

	int ret = avio_open2(&s->pb, tcp_url.array, AVIO_FLAG_READ_WRITE, NULL, NULL);
	dstr_free(&tcp_url);
	if (ret < 0) {
		char errbuf[128];
		av_strerror(ret, errbuf, sizeof(errbuf));
		ertsp_set_error(s, "TCP connect to %s:%d failed: %s", s->host.array, s->port, errbuf);
		return false;
	}
	return true;
}

struct rtsp_response {
	int status_code;
	struct dstr session_id;
	struct dstr www_authenticate;
	long content_length;
	char *body; /* NULL if no body; caller frees */
};

static void rtsp_response_free(struct rtsp_response *r)
{
	dstr_free(&r->session_id);
	dstr_free(&r->www_authenticate);
	if (r->body)
		bfree(r->body);
	memset(r, 0, sizeof(*r));
}

/* Reads one RTSP response: status line + headers (up to the blank line),
 * then Content-Length bytes of body if present. avio_read is used one
 * buffer at a time rather than byte-by-byte for the header scan, which is
 * simpler at the cost of potentially reading a little past "\r\n\r\n" into
 * whatever's next on the wire - fine here, since ANNOUNCE/SETUP/RECORD are
 * always answered before anything else arrives on this connection (no RTP
 * can be flowing yet - RECORD hasn't been sent). */
static bool rtsp_read_response(struct ertsp_output *s, struct rtsp_response *out)
{
	memset(out, 0, sizeof(*out));

	char buf[ERTSP_MAX_RESPONSE];
	size_t total = 0;
	const char *header_end = NULL;

	while (total < sizeof(buf) - 1) {
		int n = avio_read(s->pb, (uint8_t *)buf + total, 1);
		if (n <= 0) {
			ertsp_set_error(s, "connection closed while reading response");
			return false;
		}
		total += (size_t)n;
		buf[total] = '\0';
		if (total >= 4 && memcmp(buf + total - 4, "\r\n\r\n", 4) == 0) {
			header_end = buf + total;
			break;
		}
	}
	if (!header_end) {
		ertsp_set_error(s, "response headers too large or malformed");
		return false;
	}

	/* status line: "RTSP/1.0 200 OK\r\n" */
	if (strncmp(buf, "RTSP/1.0 ", 9) != 0 && strncmp(buf, "RTSP/1.1 ", 9) != 0) {
		ertsp_set_error(s, "not an RTSP response: %.32s", buf);
		return false;
	}
	out->status_code = atoi(buf + 9);

	/* headers, line by line */
	const char *line = strstr(buf, "\r\n") + 2;
	while (line < header_end - 2) {
		const char *line_end = strstr(line, "\r\n");
		if (!line_end)
			break;
		const char *colon = memchr(line, ':', (size_t)(line_end - line));
		if (colon) {
			const char *v = colon + 1;
			while (v < line_end && *v == ' ')
				v++;
			size_t vlen = (size_t)(line_end - v);
			if (astrcmpi_n(line, "Session", 7) == 0) {
				/* Session: <id>;timeout=60 - keep only the id */
				const char *semi = memchr(v, ';', vlen);
				dstr_ncopy(&out->session_id, v, semi ? (size_t)(semi - v) : vlen);
			} else if (astrcmpi_n(line, "WWW-Authenticate", 16) == 0) {
				dstr_ncopy(&out->www_authenticate, v, vlen);
			} else if (astrcmpi_n(line, "Content-Length", 14) == 0) {
				out->content_length = atol(v);
			}
		}
		line = line_end + 2;
	}

	if (out->content_length > 0) {
		out->body = bmalloc((size_t)out->content_length + 1);
		size_t got = 0;
		while (got < (size_t)out->content_length) {
			int n = avio_read(s->pb, (uint8_t *)out->body + got, (int)((size_t)out->content_length - got));
			if (n <= 0) {
				ertsp_set_error(s, "connection closed while reading response body");
				rtsp_response_free(out);
				return false;
			}
			got += (size_t)n;
		}
		out->body[out->content_length] = '\0';
	}

	return true;
}

/* Sends one RTSP request and returns the parsed response. uri is the full
 * "rtsp://host:port/path[...]" request-URI (the RTSP request line always
 * carries an absolute URI here, same as every real RTSP client/server -
 * ppmmx's gortsplib server parses out just the path component from it, see
 * session.go's onAnnounce/onSetup). extra_headers, if non-NULL, is appended
 * verbatim (each line already "\r\n"-terminated) before the final blank
 * line. body/body_len, if body is non-NULL, are sent after the headers with
 * a Content-Length header added automatically. */
static bool rtsp_request(struct ertsp_output *s, const char *method, const char *uri, const char *extra_headers,
			  const char *body, size_t body_len, struct rtsp_response *out)
{
	struct dstr req = {0};
	dstr_printf(&req, "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: ppobs-ertsp\r\n", method, uri, ++s->cseq);
	if (s->session_id.len)
		dstr_catf(&req, "Session: %s\r\n", s->session_id.array);
	if (extra_headers)
		dstr_cat(&req, extra_headers);
	if (body && body_len > 0)
		dstr_catf(&req, "Content-Length: %zu\r\n\r\n", body_len);
	else
		dstr_cat(&req, "\r\n");

	/* avio_write() returns void in this FFmpeg version - errors surface
	 * later, as a read failure/timeout on the response. */
	avio_write(s->pb, (const uint8_t *)req.array, (int)req.len);
	if (body && body_len > 0)
		avio_write(s->pb, (const uint8_t *)body, (int)body_len);
	dstr_free(&req);
	avio_flush(s->pb);

	return rtsp_read_response(s, out);
}

/* ANNOUNCE with one retry on 401: the first attempt carries no Authorization
 * header (matching every other ERTSP client - ppmmx's own
 * verifyERTSPPublishToken, same as the playback side, expects to issue a
 * WWW-Authenticate challenge rather than assume credentials are sent
 * up-front); a second 401 after answering the challenge is a genuine auth
 * failure, not retried again. */
static bool ertsp_announce(struct ertsp_output *s, const char *sdp, size_t sdp_len)
{
	struct dstr uri = {0};
	dstr_printf(&uri, "rtsp://%s:%d/%s", s->host.array, s->port, s->path.array);

	struct dstr headers = {0};
	dstr_copy(&headers, "Content-Type: application/sdp\r\n");

	struct rtsp_response resp = {0};
	bool ok = rtsp_request(s, "ANNOUNCE", uri.array, headers.array, sdp, sdp_len, &resp);
	dstr_free(&headers);

	if (ok && resp.status_code == 401) {
		rtsp_response_free(&resp);

		char userpass[512];
		snprintf(userpass, sizeof(userpass), "%s:%s", s->auth_user.array, s->auth_pass.array);
		char b64[AV_BASE64_SIZE(sizeof(userpass))];
		av_base64_encode(b64, sizeof(b64), (const uint8_t *)userpass, (int)strlen(userpass));

		dstr_printf(&headers, "Content-Type: application/sdp\r\nAuthorization: Basic %s\r\n", b64);
		ok = rtsp_request(s, "ANNOUNCE", uri.array, headers.array, sdp, sdp_len, &resp);
		dstr_free(&headers);
		s->authed = true;
	}

	dstr_free(&uri);

	if (!ok)
		return false;
	if (resp.status_code != 200) {
		ertsp_set_error(s, "ANNOUNCE rejected: RTSP %d", resp.status_code);
		rtsp_response_free(&resp);
		return false;
	}
	rtsp_response_free(&resp);
	return true;
}

/* SETUP for one track. control is the SDP "a=control" value for that media
 * (e.g. "streamid=0"); interleaved is "0-1" or "2-3". The resolved URL is
 * "<announce-uri>/<control>" - gortsplib's "FFmpeg format" convention for a
 * RECORD session with no query string on the base URL (server_session.go's
 * findMediaByURL: a non-"rtsp://" control value is appended to the path
 * with "/" when the base URL carries no query - this file's ANNOUNCE URI
 * never has one, so this is the branch that always applies here). The
 * first successful SETUP's Session header is kept for every request after
 * it, including this function's own second call for the other track. */
static bool ertsp_setup(struct ertsp_output *s, const char *control, const char *interleaved)
{
	struct dstr uri = {0};
	dstr_printf(&uri, "rtsp://%s:%d/%s/%s", s->host.array, s->port, s->path.array, control);

	struct dstr headers = {0};
	dstr_printf(&headers, "Transport: RTP/AVP/TCP;unicast;interleaved=%s\r\n", interleaved);

	struct rtsp_response resp = {0};
	bool ok = rtsp_request(s, "SETUP", uri.array, headers.array, NULL, 0, &resp);
	dstr_free(&uri);
	dstr_free(&headers);
	if (!ok)
		return false;
	if (resp.status_code != 200) {
		ertsp_set_error(s, "SETUP %s rejected: RTSP %d", control, resp.status_code);
		rtsp_response_free(&resp);
		return false;
	}
	if (!s->session_id.len && resp.session_id.len)
		dstr_copy_dstr(&s->session_id, &resp.session_id);
	rtsp_response_free(&resp);
	return true;
}

static bool ertsp_record(struct ertsp_output *s)
{
	struct dstr uri = {0};
	dstr_printf(&uri, "rtsp://%s:%d/%s", s->host.array, s->port, s->path.array);

	struct rtsp_response resp = {0};
	bool ok = rtsp_request(s, "RECORD", uri.array, "Range: npt=0.000-\r\n", NULL, 0, &resp);
	dstr_free(&uri);
	if (!ok)
		return false;
	if (resp.status_code != 200) {
		ertsp_set_error(s, "RECORD rejected: RTSP %d", resp.status_code);
		rtsp_response_free(&resp);
		return false;
	}
	rtsp_response_free(&resp);
	return true;
}

static void ertsp_teardown(struct ertsp_output *s)
{
	if (!s->pb || !s->session_id.len)
		return;
	struct dstr uri = {0};
	dstr_printf(&uri, "rtsp://%s:%d/%s", s->host.array, s->port, s->path.array);
	struct rtsp_response resp = {0};
	/* best-effort: ignore the result, the connection is closing anyway */
	rtsp_request(s, "TEARDOWN", uri.array, NULL, 0, 0, &resp);
	rtsp_response_free(&resp);
	dstr_free(&uri);
}

/* ------------------------------------------------------------------------- */
/* SDP (RFC 4566) describing exactly the one H264 video + one Opus audio
 * track ppmmx's codec whitelist requires (publish_codec.go). rtpmap/fmtp
 * values match gortsplib's own format.H264/format.Opus FMTP()/RTPMap()
 * exactly (pkg/format/h264.go, opus.go) - this is what the *server*
 * (ppmmx) will parse this SDP into, so it has to agree byte-for-byte on
 * what each parameter means, not just be RFC-valid in isolation. */
static size_t ertsp_build_sdp(struct ertsp_output *s, char *buf, size_t buf_size)
{
	char sps_b64[2048] = "";
	char pps_b64[512] = "";
	char profile_level_id[16] = "";
	if (s->sps && s->sps_size > 0)
		av_base64_encode(sps_b64, sizeof(sps_b64), s->sps, (int)s->sps_size);
	if (s->pps && s->pps_size > 0)
		av_base64_encode(pps_b64, sizeof(pps_b64), s->pps, (int)s->pps_size);
	if (s->sps_size >= 4)
		snprintf(profile_level_id, sizeof(profile_level_id), "%02X%02X%02X", s->sps[1], s->sps[2], s->sps[3]);

	/* RFC7587: the RTP clock/channel in "a=rtpmap" MUST read 48000/2
	 * regardless of the actual channel count, which is instead signalled
	 * via "sprop-stereo" in fmtp (see format.Opus.unmarshal/RTPMap -
	 * gortsplib only accepts "opus/48000/2" literally in the rtpmap
	 * line). */
	int n = snprintf(buf, buf_size,
			  "v=0\r\n"
			  "o=- 0 0 IN IP4 0.0.0.0\r\n"
			  "s=ppobs ERTSP publish\r\n"
			  "c=IN IP4 0.0.0.0\r\n"
			  "t=0 0\r\n"
			  "m=video 0 RTP/AVP %d\r\n"
			  "a=rtpmap:%d H264/90000\r\n"
			  "a=fmtp:%d packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s,%s\r\n"
			  "a=control:streamid=0\r\n"
			  "m=audio 0 RTP/AVP %d\r\n"
			  "a=rtpmap:%d opus/48000/2\r\n"
			  "a=fmtp:%d sprop-stereo=%d\r\n"
			  "a=control:streamid=1\r\n",
			  ERTSP_VIDEO_PT, ERTSP_VIDEO_PT, ERTSP_VIDEO_PT, profile_level_id, sps_b64, pps_b64,
			  ERTSP_AUDIO_PT, ERTSP_AUDIO_PT, ERTSP_AUDIO_PT, (s->audio_channels >= 2) ? 1 : 0);
	return (n > 0) ? (size_t)n : 0;
}

/* ------------------------------------------------------------------------- */
/* RTP (RFC 3550) + interleaved framing (RFC 2326 §10.12: "$" + 1-byte
 * channel + 2-byte big-endian length + the RTP/RTCP packet itself, sent
 * over the same TCP connection as the RTSP control text). Both channels
 * share s->pb and must be serialized - callers hold s->write_mutex. */
static bool ertsp_send_interleaved(struct ertsp_output *s, uint8_t channel, const uint8_t *data, size_t len)
{
	uint8_t hdr[4] = {'$', channel, (uint8_t)(len >> 8), (uint8_t)(len & 0xFF)};
	avio_write(s->pb, hdr, 4);
	avio_write(s->pb, data, (int)len);
	avio_flush(s->pb);
	return true;
}

static void rtp_write_header(uint8_t *buf, bool marker, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc)
{
	buf[0] = 0x80; /* version 2, no padding/extension/CSRC */
	buf[1] = (uint8_t)((marker ? 0x80 : 0) | (pt & 0x7F));
	buf[2] = (uint8_t)(seq >> 8);
	buf[3] = (uint8_t)(seq & 0xFF);
	buf[4] = (uint8_t)(ts >> 24);
	buf[5] = (uint8_t)(ts >> 16);
	buf[6] = (uint8_t)(ts >> 8);
	buf[7] = (uint8_t)(ts & 0xFF);
	buf[8] = (uint8_t)(ssrc >> 24);
	buf[9] = (uint8_t)(ssrc >> 16);
	buf[10] = (uint8_t)(ssrc >> 8);
	buf[11] = (uint8_t)(ssrc & 0xFF);
}

/* FU-A fragmentation threshold. There's no real network MTU to respect -
 * everything rides the same TCP connection as the RTSP control channel,
 * framed with a 2-byte (max 65535) length - so this exists only as a safety
 * net for a pathological oversized NAL (e.g. a very high-bitrate keyframe),
 * not a normal code path; see rtph264's defaultPayloadMaxSize (1450, a real
 * UDP MTU budget) for the contrast this mirrors structurally but not in
 * magnitude. */
#define ERTSP_H264_FRAG_THRESHOLD 60000

/* Sends one H264 NAL unit (including its own 1-byte header, no Annex-B
 * start code) as one or more RTP packets, single-NAL-unit or FU-A per
 * RFC6184 - mirrors gortsplib's own rtph264.Encoder.writeSingle/
 * writeFragmented exactly (same NRI/type bit layout), so a NAL this file
 * can emit is one gortsplib's decoder can reassemble. marker is set on the
 * packet(s) carrying the LAST NAL of an access unit (one encoded video
 * frame may be several NALs - e.g. SPS+PPS+IDR-slice on a keyframe - all
 * sharing one RTP timestamp, with the marker only on the final one, per
 * RFC6184 §5.1). */
static bool ertsp_send_h264_nal(struct ertsp_output *s, const uint8_t *nal, size_t nal_len, uint32_t rtp_ts,
				 bool marker)
{
	if (nal_len == 0)
		return true;

	if (nal_len <= ERTSP_H264_FRAG_THRESHOLD) {
		uint8_t pkt[12 + ERTSP_H264_FRAG_THRESHOLD];
		rtp_write_header(pkt, marker, ERTSP_VIDEO_PT, s->video_seq++, rtp_ts, s->video_ssrc);
		memcpy(pkt + 12, nal, nal_len);
		return ertsp_send_interleaved(s, ERTSP_CH_VIDEO_RTP, pkt, 12 + nal_len);
	}

	/* FU-A: fragment nal[1:] (the payload after the 1-byte NAL header)
	 * into ERTSP_H264_FRAG_THRESHOLD-sized chunks, each prefixed with a
	 * 1-byte FU indicator (NRI bits + type 28) and a 1-byte FU header
	 * (start/end bits + the original NAL type). */
	uint8_t nri = (nal[0] >> 5) & 0x03;
	uint8_t orig_type = nal[0] & 0x1F;
	const uint8_t *payload = nal + 1;
	size_t remaining = nal_len - 1;
	bool first = true;

	while (remaining > 0) {
		size_t chunk = remaining;
		if (chunk > (size_t)(ERTSP_H264_FRAG_THRESHOLD - 2))
			chunk = (size_t)(ERTSP_H264_FRAG_THRESHOLD - 2);
		bool last = (chunk == remaining);

		uint8_t pkt[12 + ERTSP_H264_FRAG_THRESHOLD];
		pkt[12] = (uint8_t)((nri << 5) | 28);
		pkt[13] = (uint8_t)(((first ? 1 : 0) << 7) | ((last ? 1 : 0) << 6) | orig_type);
		memcpy(pkt + 14, payload, chunk);
		rtp_write_header(pkt, marker && last, ERTSP_VIDEO_PT, s->video_seq++, rtp_ts, s->video_ssrc);
		if (!ertsp_send_interleaved(s, ERTSP_CH_VIDEO_RTP, pkt, 14 + chunk))
			return false;

		payload += chunk;
		remaining -= chunk;
		first = false;
	}
	return true;
}

/* Splits an Annex-B byte stream (one or more "00 00 01" / "00 00 00 01"
 * start-code-prefixed NALs, the format OBS's H264 encoders emit - see
 * obs-x264's load_headers for confirmation that even "extra data" comes out
 * this way) and sends every NAL via ertsp_send_h264_nal, with the marker
 * bit only on the last one. SPS/PPS are cached once at connect time, from
 * the video encoder's own extra data - see ertsp_cache_sps_pps and
 * ertsp_start_thread - not from a live packet, since OBS never calls
 * encoded_packet() until after obs_output_begin_data_capture(), which is
 * what this file is connecting in order to be able to call; waiting here
 * for a keyframe before that point would deadlock forever (found in the
 * field: every ERTSP publish attempt hung until this was fixed). */
static size_t annexb_split(const uint8_t *data, size_t size, const uint8_t **nal_starts, size_t *nal_lens,
			    size_t max_nals)
{
	const uint8_t *p = data;
	const uint8_t *end = data + size;
	size_t nal_count = 0;

	while (p < end && nal_count < max_nals) {
		const uint8_t *sc = NULL;
		for (const uint8_t *q = p; q + 3 <= end; q++) {
			if (q[0] == 0 && q[1] == 0 && q[2] == 1) {
				sc = q;
				break;
			}
		}
		if (!sc)
			break;
		const uint8_t *nal_start = sc + 3;

		const uint8_t *nal_end = end;
		for (const uint8_t *q = nal_start; q + 3 <= end; q++) {
			if (q[0] == 0 && q[1] == 0 && q[2] == 1) {
				nal_end = (q > nal_start && q[-1] == 0) ? q - 1 : q;
				break;
			}
		}
		if (nal_end > nal_start) {
			nal_starts[nal_count] = nal_start;
			nal_lens[nal_count] = (size_t)(nal_end - nal_start);
			nal_count++;
		}
		p = nal_end;
	}
	return nal_count;
}

/* Caches the first SPS (type 7) / PPS (type 8) NAL found in an Annex-B
 * buffer - the video encoder's own extra data at connect time (see
 * ertsp_start_thread; obs-x264's load_headers confirms OBS's H264 encoders
 * emit extra data in this same start-code-prefixed form, not AVCC). */
static void ertsp_cache_sps_pps(struct ertsp_output *s, const uint8_t *data, size_t size)
{
	const uint8_t *nal_starts[64];
	size_t nal_lens[64];
	size_t nal_count = annexb_split(data, size, nal_starts, nal_lens, 64);

	for (size_t i = 0; i < nal_count; i++) {
		uint8_t nal_type = nal_starts[i][0] & 0x1F;
		if (nal_type == 7 && !s->sps) { /* SPS */
			s->sps = bmemdup(nal_starts[i], nal_lens[i]);
			s->sps_size = nal_lens[i];
		} else if (nal_type == 8 && !s->pps) { /* PPS */
			s->pps = bmemdup(nal_starts[i], nal_lens[i]);
			s->pps_size = nal_lens[i];
		}
	}
}

static void ertsp_send_h264_access_unit(struct ertsp_output *s, const uint8_t *data, size_t size, uint32_t rtp_ts)
{
	if (!s->connected)
		return;

	const uint8_t *nal_starts[64];
	size_t nal_lens[64];
	size_t nal_count = annexb_split(data, size, nal_starts, nal_lens, 64);

	for (size_t i = 0; i < nal_count; i++)
		ertsp_send_h264_nal(s, nal_starts[i], nal_lens[i], rtp_ts, i == nal_count - 1);
}

/* Opus (RFC7587): one encoded Opus packet is sent verbatim as one RTP
 * packet's payload - no sub-framing, matching gortsplib's rtpsimpleaudio
 * encoder/decoder exactly (pkg/format/rtpsimpleaudio). */
static void ertsp_send_opus_frame(struct ertsp_output *s, const uint8_t *data, size_t size, uint32_t rtp_ts)
{
	if (!s->connected || size == 0 || size > 1400)
		return;
	uint8_t pkt[12 + 1400];
	rtp_write_header(pkt, false, ERTSP_AUDIO_PT, s->audio_seq++, rtp_ts, s->audio_ssrc);
	memcpy(pkt + 12, data, size);
	ertsp_send_interleaved(s, ERTSP_CH_AUDIO_RTP, pkt, 12 + size);
}

/* ------------------------------------------------------------------------- */

static const char *ertsp_output_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "ERTSP (PPCDN)";
}

static void *ertsp_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct ertsp_output *s = bzalloc(sizeof(struct ertsp_output));
	s->output = output;
	pthread_mutex_init(&s->write_mutex, NULL);
	return s;
}

static void ertsp_close_connection(struct ertsp_output *s)
{
	if (s->pb) {
		if (s->connected)
			ertsp_teardown(s);
		avio_close(s->pb);
		s->pb = NULL;
	}
	s->connected = false;
	dstr_free(&s->session_id);
	s->cseq = 0;
	s->authed = false;
}

static void ertsp_output_destroy(void *data)
{
	struct ertsp_output *s = data;
	if (s->start_thread_active)
		pthread_join(s->start_thread, NULL);
	ertsp_close_connection(s);
	dstr_free(&s->host);
	dstr_free(&s->auth_user);
	dstr_free(&s->auth_pass);
	dstr_free(&s->path);
	if (s->sps)
		bfree(s->sps);
	if (s->pps)
		bfree(s->pps);
	pthread_mutex_destroy(&s->write_mutex);
	bfree(s);
}

static void *ertsp_start_thread(void *data)
{
	struct ertsp_output *s = data;
	int code = OBS_OUTPUT_CONNECT_FAILED;

	obs_service_t *service = obs_output_get_service(s->output);
	const char *url = service ? obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : NULL;
	if (!url || !*url) {
		ertsp_set_error(s, "no service URL configured");
		goto fail;
	}
	if (!ertsp_parse_url(s, url))
		goto fail;

	obs_encoder_t *aencoder = obs_output_get_audio_encoder(s->output, 0);
	if (aencoder) {
		s->audio_samplerate = (int)obs_encoder_get_sample_rate(aencoder);
		struct obs_audio_info aoi;
		obs_get_audio_info(&aoi);
		s->audio_channels = (int)get_audio_channels(aoi.speakers);
	}

	/* ANNOUNCE needs SPS/PPS in its SDP - read them from the video
	 * encoder's own extra data now, *before* connecting (matching
	 * rtmp-stream.c's connect_thread, which reads the same kind of header
	 * info before its own obs_output_begin_data_capture() call for
	 * exactly the same reason: no encoded_packet() callback - and so no
	 * live NAL to parse - happens before that call). */
	obs_encoder_t *vencoder = obs_output_get_video_encoder2(s->output, 0);
	uint8_t *extra_data = NULL;
	size_t extra_size = 0;
	if (vencoder && obs_encoder_get_extra_data(vencoder, &extra_data, &extra_size) && extra_data && extra_size)
		ertsp_cache_sps_pps(s, extra_data, extra_size);
	if (!s->sps || !s->pps) {
		ertsp_set_error(s, "no SPS/PPS available from the video encoder's extra data");
		goto fail;
	}

	if (!ertsp_connect_tcp(s))
		goto fail;

	char sdp[4096];
	size_t sdp_len = ertsp_build_sdp(s, sdp, sizeof(sdp));

	if (!ertsp_announce(s, sdp, sdp_len)) {
		code = OBS_OUTPUT_INVALID_STREAM;
		goto fail;
	}
	if (!ertsp_setup(s, "streamid=0", "0-1")) {
		code = OBS_OUTPUT_INVALID_STREAM;
		goto fail;
	}
	if (!ertsp_setup(s, "streamid=1", "2-3")) {
		code = OBS_OUTPUT_INVALID_STREAM;
		goto fail;
	}
	if (!ertsp_record(s)) {
		code = OBS_OUTPUT_INVALID_STREAM;
		goto fail;
	}

	s->video_ssrc = (uint32_t)os_gettime_ns();
	s->audio_ssrc = s->video_ssrc ^ 0x5a5a5a5a;
	s->video_seq = (uint16_t)os_gettime_ns();
	s->audio_seq = (uint16_t)(os_gettime_ns() >> 16);

	s->connected = true;
	s->active = true;
	info("connected and recording to rtsp://%s:%d/%s", s->host.array, s->port, s->path.array);
	obs_output_begin_data_capture(s->output, 0);
	s->start_thread_active = false;
	return NULL;

fail:
	ertsp_close_connection(s);
	obs_output_signal_stop(s->output, code);
	s->start_thread_active = false;
	return NULL;
}

static bool ertsp_output_start(void *data)
{
	struct ertsp_output *s = data;
	if (!obs_output_can_begin_data_capture(s->output, 0))
		return false;
	if (!obs_output_initialize_encoders(s->output, 0))
		return false;

	s->stopping = false;
	if (pthread_create(&s->start_thread, NULL, ertsp_start_thread, s) != 0)
		return false;
	s->start_thread_active = true;
	return true;
}

static void ertsp_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct ertsp_output *s = data;
	s->stopping = true;
	if (s->start_thread_active)
		pthread_join(s->start_thread, NULL);

	pthread_mutex_lock(&s->write_mutex);
	bool was_active = s->active;
	s->active = false;
	ertsp_close_connection(s);
	pthread_mutex_unlock(&s->write_mutex);

	if (was_active)
		obs_output_end_data_capture(s->output);
}

static void ertsp_output_encoded_packet(void *data, struct encoder_packet *packet)
{
	struct ertsp_output *s = data;
	if (!packet)
		return;

	pthread_mutex_lock(&s->write_mutex);
	if (!s->active) {
		pthread_mutex_unlock(&s->write_mutex);
		return;
	}

	if (packet->type == OBS_ENCODER_VIDEO) {
		uint32_t rtp_ts = (uint32_t)((int64_t)packet->pts * 90000 * packet->timebase_num / packet->timebase_den);
		ertsp_send_h264_access_unit(s, packet->data, packet->size, rtp_ts);
	} else if (packet->type == OBS_ENCODER_AUDIO) {
		int clock = (s->audio_samplerate > 0) ? s->audio_samplerate : 48000;
		uint32_t rtp_ts = (uint32_t)((int64_t)packet->pts * clock * packet->timebase_num / packet->timebase_den);
		if (s->connected)
			ertsp_send_opus_frame(s, packet->data, packet->size, rtp_ts);
	}

	pthread_mutex_unlock(&s->write_mutex);
}

static uint64_t ertsp_output_total_bytes(void *data)
{
	UNUSED_PARAMETER(data);
	return 0;
}

struct obs_output_info ertsp_output_info = {
	.id = "ertsp_output",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
	.encoded_video_codecs = "h264",
	.encoded_audio_codecs = "opus",
	.protocols = "RTSP",
	.get_name = ertsp_output_getname,
	.create = ertsp_output_create,
	.destroy = ertsp_output_destroy,
	.start = ertsp_output_start,
	.stop = ertsp_output_stop,
	.encoded_packet = ertsp_output_encoded_packet,
	.get_total_bytes = ertsp_output_total_bytes,
};
