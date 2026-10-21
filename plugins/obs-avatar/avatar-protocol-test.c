#include "avatar-sidecar-protocol.h"
#include "avatar-test-check.h"

#include <stdint.h>
#include <string.h>

/* The fixtures below are the point of this file. The plugin and the worker are
 * written in different languages, so the likeliest way this protocol breaks is
 * the two ends disagreeing on the bytes while both "work" in isolation. These
 * exact byte sequences are also asserted by the worker's --selftest, so a
 * disagreement fails on one side or the other instead of showing up as a
 * garbled frame at runtime. Changing a fixture means changing both. */

static void test_header_fixture(void)
{
	/* FRAME (type 4) with a 16-byte payload. */
	static const uint8_t expected[AVATAR_MSG_HEADER_SIZE] = {0x10, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};

	uint8_t buf[AVATAR_MSG_HEADER_SIZE];
	avatar_msg_header_write(buf, AVATAR_MSG_FRAME, 16);
	CHECK(memcmp(buf, expected, sizeof(buf)) == 0);

	struct avatar_msg_header hdr;
	CHECK(avatar_msg_header_read(expected, &hdr));
	CHECK(hdr.payload_len == 16);
	CHECK(hdr.type == AVATAR_MSG_FRAME);
	CHECK(hdr.reserved == 0);
}

static void test_audio_payload_fixture(void)
{
	/* pts_ns = 1000000000, two samples: 0.5 and -0.25. */
	static const uint8_t expected[AVATAR_AUDIO_HEADER_SIZE + 8] = {
		0x00, 0xCA, 0x9A, 0x3B, 0x00, 0x00, 0x00, 0x00, /* pts_ns */
		0x02, 0x00, 0x00, 0x00,                         /* sample_count */
		0x00, 0x00, 0x00, 0x3F,                         /* 0.5f */
		0x00, 0x00, 0x80, 0xBE,                         /* -0.25f */
	};

	uint8_t buf[sizeof(expected)];
	avatar_put_i64(buf, 1000000000);
	avatar_put_u32(buf + 8, 2);
	avatar_put_f32(buf + 12, 0.5f);
	avatar_put_f32(buf + 16, -0.25f);
	CHECK(memcmp(buf, expected, sizeof(buf)) == 0);

	CHECK(avatar_get_i64(expected) == 1000000000);
	CHECK(avatar_get_u32(expected + 8) == 2);
	CHECK(avatar_get_f32(expected + 12) == 0.5f);
	CHECK(avatar_get_f32(expected + 16) == -0.25f);
}

static void test_mouth_gain_fixture(void)
{
	/* MOUTH_GAIN (type 10), a single float32: 2.0f. */
	static const uint8_t expected_header[AVATAR_MSG_HEADER_SIZE] = {0x04, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00};
	static const uint8_t expected_payload[AVATAR_MOUTH_GAIN_PAYLOAD_SIZE] = {0x00, 0x00, 0x00, 0x40};

	uint8_t header[AVATAR_MSG_HEADER_SIZE];
	avatar_msg_header_write(header, AVATAR_MSG_MOUTH_GAIN, AVATAR_MOUTH_GAIN_PAYLOAD_SIZE);
	CHECK(memcmp(header, expected_header, sizeof(header)) == 0);

	uint8_t payload[AVATAR_MOUTH_GAIN_PAYLOAD_SIZE];
	avatar_put_f32(payload, 2.0f);
	CHECK(memcmp(payload, expected_payload, sizeof(payload)) == 0);
	CHECK(avatar_get_f32(expected_payload) == 2.0f);
}

static void test_set_fps_fixture(void)
{
	/* SET_FPS (type 11), a single uint32: 20. */
	static const uint8_t expected_header[AVATAR_MSG_HEADER_SIZE] = {0x04, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00};
	static const uint8_t expected_payload[AVATAR_SET_FPS_PAYLOAD_SIZE] = {0x14, 0x00, 0x00, 0x00};

	uint8_t header[AVATAR_MSG_HEADER_SIZE];
	avatar_msg_header_write(header, AVATAR_MSG_SET_FPS, AVATAR_SET_FPS_PAYLOAD_SIZE);
	CHECK(memcmp(header, expected_header, sizeof(header)) == 0);

	uint8_t payload[AVATAR_SET_FPS_PAYLOAD_SIZE];
	avatar_put_u32(payload, 20);
	CHECK(memcmp(payload, expected_payload, sizeof(payload)) == 0);
	CHECK(avatar_get_u32(expected_payload) == 20);
}

static void test_scalar_round_trip(void)
{
	uint8_t buf[8];

	avatar_put_u16(buf, 0xBEEF);
	CHECK(avatar_get_u16(buf) == 0xBEEF);
	CHECK(buf[0] == 0xEF && buf[1] == 0xBE);

	avatar_put_u32(buf, 0xDEADBEEFu);
	CHECK(avatar_get_u32(buf) == 0xDEADBEEFu);
	CHECK(buf[0] == 0xEF && buf[3] == 0xDE);

	/* Timestamps are signed, and the extremes have to survive the trip - a
	 * worker that misreads a pts cannot be told apart from one that is
	 * simply late. */
	avatar_put_i64(buf, INT64_MIN);
	CHECK(avatar_get_i64(buf) == INT64_MIN);
	avatar_put_i64(buf, INT64_MAX);
	CHECK(avatar_get_i64(buf) == INT64_MAX);
	avatar_put_i64(buf, -1);
	CHECK(avatar_get_i64(buf) == -1);

	avatar_put_f32(buf, 0.0f);
	CHECK(avatar_get_f32(buf) == 0.0f);
	avatar_put_f32(buf, -1.0f);
	CHECK(avatar_get_f32(buf) == -1.0f);
}

static void test_header_rejects_bad_input(void)
{
	uint8_t buf[AVATAR_MSG_HEADER_SIZE];
	struct avatar_msg_header hdr;

	/* Reserved must be zero: giving it a meaning later has to make this
	 * build reject the message rather than misread it. */
	avatar_msg_header_write(buf, AVATAR_MSG_AUDIO, 4);
	avatar_put_u16(buf + 6, 1);
	CHECK(!avatar_msg_header_read(buf, &hdr));

	/* An oversized length is refused before it can become an allocation. */
	avatar_msg_header_write(buf, AVATAR_MSG_FRAME, AVATAR_MSG_MAX_PAYLOAD + 1);
	CHECK(!avatar_msg_header_read(buf, &hdr));

	/* Exactly at the cap is still legal. */
	avatar_msg_header_write(buf, AVATAR_MSG_FRAME, AVATAR_MSG_MAX_PAYLOAD);
	CHECK(avatar_msg_header_read(buf, &hdr));
	CHECK(hdr.payload_len == AVATAR_MSG_MAX_PAYLOAD);

	/* An empty payload is legal - BYE carries none. */
	avatar_msg_header_write(buf, AVATAR_MSG_BYE, 0);
	CHECK(avatar_msg_header_read(buf, &hdr));
	CHECK(hdr.payload_len == 0);
}

static void test_header_accepts_unknown_type(void)
{
	/* Forward compatibility: a newer worker's message type must parse so the
	 * reader can skip its payload. Rejecting it would tear down a pipe that
	 * is otherwise working perfectly. */
	uint8_t buf[AVATAR_MSG_HEADER_SIZE];
	struct avatar_msg_header hdr;

	avatar_msg_header_write(buf, 9999, 12);
	CHECK(avatar_msg_header_read(buf, &hdr));
	CHECK(hdr.type == 9999);
	CHECK(hdr.payload_len == 12);
}

static void test_audio_payload_validation(void)
{
	CHECK(avatar_audio_payload_size(0) == AVATAR_AUDIO_HEADER_SIZE);
	CHECK(avatar_audio_payload_size(160) == AVATAR_AUDIO_HEADER_SIZE + 640);

	CHECK(avatar_audio_payload_valid(AVATAR_AUDIO_HEADER_SIZE + 640, 160));

	/* A count that disagrees with the stated length is the whole point of
	 * the check: trusting the count would read past the buffer. */
	CHECK(!avatar_audio_payload_valid(AVATAR_AUDIO_HEADER_SIZE + 640, 161));
	CHECK(!avatar_audio_payload_valid(AVATAR_AUDIO_HEADER_SIZE + 640, 159));

	/* Truncated below its own header. */
	CHECK(!avatar_audio_payload_valid(4, 0));

	/* A sample count whose byte size would exceed the cap, including one
	 * chosen so that count * 4 overflows 32 bits back into a small value. */
	CHECK(!avatar_audio_payload_valid(AVATAR_AUDIO_HEADER_SIZE, avatar_audio_max_samples() + 1));
	CHECK(!avatar_audio_payload_valid(AVATAR_AUDIO_HEADER_SIZE + 16, 0x40000004u));
}

static void test_frame_payload_validation(void)
{
	/* MuseTalk's own output size. */
	CHECK(avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE + 256 * 256 * 4, 256, 256));

	/* The largest edge allowed, which is also the cap the payload limit was
	 * chosen around. */
	CHECK(avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE + 1024 * 1024 * 4, 1024, 1024));
	CHECK(!avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE + 1025 * 1025 * 4, 1025, 1025));

	/* Dimensions that disagree with the length. */
	CHECK(!avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE + 256 * 256 * 4, 256, 255));
	CHECK(!avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE, 256, 256));

	/* Zero dimensions would make an empty texture that renders as nothing. */
	CHECK(!avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE, 0, 0));
	CHECK(!avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE, 256, 0));

	/* The overflow case this validator exists for: 65536 * 65536 * 4 is 2^34,
	 * which truncates to 0 in 32-bit arithmetic and would make a 16-byte
	 * payload look like it holds a 65536-square frame. */
	CHECK(!avatar_frame_payload_valid(AVATAR_FRAME_HEADER_SIZE, 65536, 65536));
}

static void test_crop_payload_validation(void)
{
	const uint32_t frame_w = 768;
	const uint32_t frame_h = 1024;

	/* A plausible repainted region: a face bbox expanded by MuseTalk's own
	 * crop factor, somewhere in the upper middle of a portrait. */
	CHECK(avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE + 300 * 360 * 4, 230, 180, 300, 360, frame_w,
					frame_h));

	/* Dimensions disagreeing with the length, as for frames. */
	CHECK(!avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE + 300 * 360 * 4, 230, 180, 300, 359, frame_w,
					 frame_h));
	CHECK(!avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE, 230, 180, 300, 360, frame_w, frame_h));

	/* The region has to sit inside the reference frame. An x/y taken on
	 * trust would place a texture outside the artwork - or, added to a
	 * width, wrap and appear to fit. */
	CHECK(!avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE + 300 * 360 * 4, 600, 180, 300, 360, frame_w,
					 frame_h));
	CHECK(!avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE + 300 * 360 * 4, 230, 800, 300, 360, frame_w,
					 frame_h));
	CHECK(!avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE + 16 * 16 * 4, 0xFFFFFFF0u, 0, 16, 16, frame_w,
					 frame_h));

	/* Flush against the far edge is legal. */
	CHECK(avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE + 300 * 360 * 4, frame_w - 300, frame_h - 360, 300,
					360, frame_w, frame_h));

	/* Zero dimensions, and an edge beyond what a payload may carry. */
	CHECK(!avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE, 0, 0, 0, 0, frame_w, frame_h));
	CHECK(!avatar_crop_payload_valid(AVATAR_CROP_HEADER_SIZE + 2000 * 16 * 4, 0, 0, 2000, 16, 4096, 4096));
}

static void test_protocol_version(void)
{
	/* CROP arrived in 2. The version is checked rather than relying on the
	 * unknown-type skip: a plugin that ignored every CROP would show a
	 * still avatar and look like a worker that had stopped. */
	CHECK(AVATAR_PROTOCOL_VERSION == 2);
	CHECK(AVATAR_CROP_HEADER_SIZE == 24);
}

int main(void)
{
	test_header_fixture();
	test_audio_payload_fixture();
	test_mouth_gain_fixture();
	test_set_fps_fixture();
	test_scalar_round_trip();
	test_header_rejects_bad_input();
	test_header_accepts_unknown_type();
	test_audio_payload_validation();
	test_frame_payload_validation();
	test_crop_payload_validation();
	test_protocol_version();
	return 0;
}
