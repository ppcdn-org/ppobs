/* Wire format between the plugin and the MuseTalk worker process.
 *
 * The plugin owns the named pipe (it is the server) and the worker connects to
 * it, so the worker side needs nothing more than open() on \\.\pipe\<name>.
 * That is deliberate: it keeps the Python side free of pywin32, and it means
 * the OS controls who may connect rather than a localhost port that any
 * process on the machine could reach.
 *
 * The pipe is a byte stream (PIPE_TYPE_BYTE) with our own length framing
 * rather than a message-mode pipe, because message mode has no equivalent on
 * the Python side of a plain open().
 *
 * Every integer is little-endian and written byte by byte through the helpers
 * below. Nothing here memcpy's a struct onto the wire: the two ends are
 * different languages and different compilers, so struct padding and host
 * endianness must not enter into it. The Python side speaks the same format
 * through struct.pack("<...") - see tools/musetalk-sidecar/.
 *
 * avatar-protocol-test.c pins the exact bytes of a few messages, and the
 * worker's --selftest checks the same fixtures from Python. A wire-format
 * disagreement between the two languages is the failure this protocol is most
 * likely to suffer, so it is the one thing tested from both sides.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change. The worker reports its own version in
 * READY and the plugin refuses a mismatch outright: a half-understood frame
 * stream is worse than no avatar, because it would show the viewer garbage. */
#define AVATAR_PROTOCOL_VERSION 1

#define AVATAR_MSG_HEADER_SIZE 8

/* Bounds the largest frame a worker may send, and with it the largest
 * allocation an untrusted length field can provoke. 4 MiB covers 1024x1024
 * BGRA with room for the payload header; MuseTalk's own output is 256x256. */
#define AVATAR_FRAME_MAX_EDGE 1024
#define AVATAR_MSG_MAX_PAYLOAD (4u * 1024u * 1024u + 64u)

enum avatar_msg_type {
	AVATAR_MSG_HELLO = 1, /* plugin -> worker: negotiate */
	AVATAR_MSG_READY = 2, /* worker -> plugin: accepted, here is what I am */
	AVATAR_MSG_AUDIO = 3, /* plugin -> worker: 16 kHz mono float samples */
	AVATAR_MSG_FRAME = 4, /* worker -> plugin: BGRA frame + its audio pts */
	AVATAR_MSG_STAT = 5,  /* worker -> plugin: per-stage timings */
	AVATAR_MSG_LOG = 6,   /* worker -> plugin: a line for OBS's log */
	AVATAR_MSG_ERROR = 7, /* worker -> plugin: fatal, with a reason */
	AVATAR_MSG_BYE = 8,   /* plugin -> worker: shut down cleanly */
};

/* READY flags */
#define AVATAR_READY_REAL_MODEL (1u << 0) /* clear means a stub engine */

/* Payload header sizes, i.e. everything before the variable-length tail. */
#define AVATAR_AUDIO_HEADER_SIZE 12 /* int64 pts_ns + uint32 sample_count */
#define AVATAR_FRAME_HEADER_SIZE 16 /* int64 pts_ns + uint32 w + uint32 h */

/* STAT: whisper_us, unet_us, vae_us, queue_depth, dropped. */
#define AVATAR_STAT_PAYLOAD_SIZE 20

/* LOG carries OBS's own log level numbers, so neither side needs a mapping
 * table and the worker's lines land in obs.log at the severity it chose.
 * Anything outside this set is treated as info rather than dropped. */
#define AVATAR_LOG_ERROR 100
#define AVATAR_LOG_WARNING 200
#define AVATAR_LOG_INFO 300
#define AVATAR_LOG_DEBUG 400

/* ---------------------------------------------------------------------------
 * Little-endian scalar access
 * ------------------------------------------------------------------------- */

static inline void avatar_put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline uint16_t avatar_get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void avatar_put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline uint32_t avatar_get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void avatar_put_i64(uint8_t *p, int64_t v)
{
	const uint64_t u = (uint64_t)v;
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)((u >> (i * 8)) & 0xff);
}

static inline int64_t avatar_get_i64(const uint8_t *p)
{
	uint64_t u = 0;
	for (int i = 0; i < 8; i++)
		u |= ((uint64_t)p[i]) << (i * 8);
	return (int64_t)u;
}

/* Floats travel as their IEEE-754 bit pattern in little-endian order, which is
 * what Python's struct "<f" produces. memcpy rather than a pointer cast: the
 * cast is a strict-aliasing violation that MSVC tolerates and other compilers
 * do not have to. */
static inline void avatar_put_f32(uint8_t *p, float v)
{
	uint32_t bits;
	memcpy(&bits, &v, sizeof(bits));
	avatar_put_u32(p, bits);
}

static inline float avatar_get_f32(const uint8_t *p)
{
	const uint32_t bits = avatar_get_u32(p);
	float v;
	memcpy(&v, &bits, sizeof(v));
	return v;
}

/* ---------------------------------------------------------------------------
 * Framing
 * ------------------------------------------------------------------------- */

struct avatar_msg_header {
	uint32_t payload_len;
	uint16_t type;
	uint16_t reserved;
};

static inline void avatar_msg_header_write(uint8_t out[AVATAR_MSG_HEADER_SIZE], uint16_t type, uint32_t payload_len)
{
	avatar_put_u32(out, payload_len);
	avatar_put_u16(out + 4, type);
	avatar_put_u16(out + 6, 0);
}

/* Parses and sanity-checks a header. Returns false when the stream cannot be
 * trusted to continue - the caller must then drop the connection rather than
 * resynchronise, since a byte stream offers nothing to resynchronise against.
 *
 * The type is deliberately NOT validated here. A newer worker may send a type
 * this build has never heard of, and the right response is to read its payload
 * and discard it, not to tear down a working pipe. Only the length and the
 * reserved field are checked, because those are what the reader must act on. */
static inline bool avatar_msg_header_read(const uint8_t in[AVATAR_MSG_HEADER_SIZE], struct avatar_msg_header *out)
{
	const uint32_t payload_len = avatar_get_u32(in);
	const uint16_t reserved = avatar_get_u16(in + 6);

	/* Reserved must be zero so that giving it a meaning later makes old
	 * readers reject the message instead of misreading it. */
	if (reserved != 0)
		return false;
	if (payload_len > AVATAR_MSG_MAX_PAYLOAD)
		return false;

	out->payload_len = payload_len;
	out->type = avatar_get_u16(in + 4);
	out->reserved = reserved;
	return true;
}

/* ---------------------------------------------------------------------------
 * Payload sizing and validation
 * ------------------------------------------------------------------------- */

static inline uint32_t avatar_audio_payload_size(uint32_t sample_count)
{
	return AVATAR_AUDIO_HEADER_SIZE + sample_count * 4u;
}

/* How many samples an AUDIO payload may carry before it exceeds the cap. */
static inline uint32_t avatar_audio_max_samples(void)
{
	return (AVATAR_MSG_MAX_PAYLOAD - AVATAR_AUDIO_HEADER_SIZE) / 4u;
}

static inline bool avatar_audio_payload_valid(uint32_t payload_len, uint32_t sample_count)
{
	if (payload_len < AVATAR_AUDIO_HEADER_SIZE)
		return false;
	if (sample_count > avatar_audio_max_samples())
		return false;
	return payload_len == avatar_audio_payload_size(sample_count);
}

/* Checked in 64-bit arithmetic on purpose: width * height * 4 overflows 32
 * bits well inside the range a uint32 field can state, and an overflowed
 * product that happens to match payload_len would hand the caller a buffer
 * far smaller than the dimensions claim. */
static inline bool avatar_frame_payload_valid(uint32_t payload_len, uint32_t width, uint32_t height)
{
	if (payload_len < AVATAR_FRAME_HEADER_SIZE)
		return false;
	if (!width || !height)
		return false;
	if (width > AVATAR_FRAME_MAX_EDGE || height > AVATAR_FRAME_MAX_EDGE)
		return false;

	const uint64_t pixels = (uint64_t)width * (uint64_t)height * 4ull;
	return (uint64_t)payload_len == (uint64_t)AVATAR_FRAME_HEADER_SIZE + pixels;
}

#ifdef __cplusplus
}
#endif
