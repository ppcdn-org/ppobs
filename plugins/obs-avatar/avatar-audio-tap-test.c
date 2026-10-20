#include "avatar-audio-tap.h"
#include "avatar-test-check.h"

#include <math.h>
#include <stdint.h>

static bool close_enough(float a, float b)
{
	return fabsf(a - b) < 0.01f;
}

static void test_ring_round_trip(void)
{
	struct avatar_pcm_ring ring;
	avatar_pcm_ring_init(&ring, 8);

	const float in[4] = {0.1f, 0.2f, 0.3f, 0.4f};
	avatar_pcm_ring_push(&ring, in, 4);
	CHECK(avatar_pcm_ring_available(&ring) == 4);

	float out[4] = {0};
	CHECK(avatar_pcm_ring_read(&ring, out, 4) == 4);
	for (size_t i = 0; i < 4; i++)
		CHECK(out[i] == in[i]);

	CHECK(avatar_pcm_ring_available(&ring) == 0);
	CHECK(ring.dropped == 0);

	/* Reading an empty ring yields nothing rather than stale samples. */
	CHECK(avatar_pcm_ring_read(&ring, out, 4) == 0);

	avatar_pcm_ring_free(&ring);
}

static void test_ring_partial_read(void)
{
	struct avatar_pcm_ring ring;
	avatar_pcm_ring_init(&ring, 8);

	const float in[4] = {1.0f, 2.0f, 3.0f, 4.0f};
	avatar_pcm_ring_push(&ring, in, 4);

	/* Asking for more than is buffered returns what there is, in order. */
	float out[6] = {0};
	CHECK(avatar_pcm_ring_read(&ring, out, 6) == 4);
	CHECK(out[0] == 1.0f && out[3] == 4.0f);

	avatar_pcm_ring_push(&ring, in, 4);
	CHECK(avatar_pcm_ring_read(&ring, out, 2) == 2);
	CHECK(out[0] == 1.0f && out[1] == 2.0f);
	/* The rest stays queued; a short read is not a flush. */
	CHECK(avatar_pcm_ring_available(&ring) == 2);

	avatar_pcm_ring_free(&ring);
}

static void test_ring_overflow_drops_oldest(void)
{
	struct avatar_pcm_ring ring;
	avatar_pcm_ring_init(&ring, 4);

	const float first[4] = {1.0f, 2.0f, 3.0f, 4.0f};
	const float second[2] = {5.0f, 6.0f};

	avatar_pcm_ring_push(&ring, first, 4);
	avatar_pcm_ring_push(&ring, second, 2);

	/* The newest audio has to survive: lip sync that falls behind the voice
	 * never catches up, so the stale end is what goes. */
	CHECK(avatar_pcm_ring_available(&ring) == 4);
	CHECK(ring.dropped == 2);

	float out[4] = {0};
	CHECK(avatar_pcm_ring_read(&ring, out, 4) == 4);
	CHECK(out[0] == 3.0f);
	CHECK(out[1] == 4.0f);
	CHECK(out[2] == 5.0f);
	CHECK(out[3] == 6.0f);

	avatar_pcm_ring_free(&ring);
}

static void test_ring_push_larger_than_capacity(void)
{
	struct avatar_pcm_ring ring;
	avatar_pcm_ring_init(&ring, 3);

	const float priming[2] = {-1.0f, -2.0f};
	avatar_pcm_ring_push(&ring, priming, 2);

	const float flood[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
	avatar_pcm_ring_push(&ring, flood, 5);

	/* Only the tail of the oversized block is kept, and everything that was
	 * already queued counts as dropped: 2 primed + 2 from the head. */
	CHECK(avatar_pcm_ring_available(&ring) == 3);
	CHECK(ring.dropped == 4);

	float out[3] = {0};
	CHECK(avatar_pcm_ring_read(&ring, out, 3) == 3);
	CHECK(out[0] == 3.0f && out[1] == 4.0f && out[2] == 5.0f);

	avatar_pcm_ring_free(&ring);
}

static void test_ring_reset(void)
{
	struct avatar_pcm_ring ring;
	avatar_pcm_ring_init(&ring, 8);

	const float in[4] = {0.5f, 0.5f, 0.5f, 0.5f};
	avatar_pcm_ring_push(&ring, in, 4);
	avatar_pcm_ring_reset(&ring);

	/* Switching driving source resets the ring; what is left belongs to the
	 * previous source and must not drive the mouth. */
	CHECK(avatar_pcm_ring_available(&ring) == 0);

	/* Reset on an already-empty ring is a no-op, not an underflow. */
	avatar_pcm_ring_reset(&ring);
	CHECK(avatar_pcm_ring_available(&ring) == 0);

	avatar_pcm_ring_free(&ring);
}

static void test_ring_zero_capacity(void)
{
	/* A degenerate capacity must not corrupt anything: pushes are refused
	 * outright rather than dividing by zero or growing without bound. */
	struct avatar_pcm_ring ring;
	avatar_pcm_ring_init(&ring, 0);

	const float in[2] = {1.0f, 2.0f};
	avatar_pcm_ring_push(&ring, in, 2);
	CHECK(avatar_pcm_ring_available(&ring) == 0);

	avatar_pcm_ring_free(&ring);
}

static void test_block_levels(void)
{
	const float block[4] = {0.5f, -0.75f, 0.25f, 0.0f};

	/* Peak is magnitude, so the negative sample wins. */
	CHECK(close_enough(avatar_block_peak(block, 4), 0.75f));

	const float expected_rms = sqrtf((0.25f + 0.5625f + 0.0625f) / 4.0f);
	CHECK(close_enough(avatar_block_rms(block, 4), expected_rms));

	/* Empty blocks are silent rather than undefined. */
	CHECK(avatar_block_peak(block, 0) == 0.0f);
	CHECK(avatar_block_rms(block, 0) == 0.0f);
}

static void test_amplitude_to_db(void)
{
	CHECK(close_enough(avatar_amplitude_to_db(1.0f), 0.0f));
	CHECK(close_enough(avatar_amplitude_to_db(0.5f), -6.02f));
	CHECK(close_enough(avatar_amplitude_to_db(0.1f), -20.0f));

	/* Digital silence reports the floor, not -inf: the latter formats as
	 * "-1.#J" and reads in the log like a different failure. */
	CHECK(avatar_amplitude_to_db(0.0f) == AVATAR_TAP_SILENCE_FLOOR_DB);

	/* The silence threshold has to sit above the floor, or nothing would
	 * ever be classed as silent. */
	CHECK(AVATAR_TAP_SILENCE_DB > AVATAR_TAP_SILENCE_FLOOR_DB);
	CHECK(avatar_amplitude_to_db(0.0f) < AVATAR_TAP_SILENCE_DB);
}

static void test_buffer_capacity_constant(void)
{
	/* The buffer has to hold more than one whisper window, otherwise mode A
	 * would be starved by design rather than by load. */
	const size_t capacity = AVATAR_TAP_SAMPLE_RATE * AVATAR_TAP_BUFFER_MS / 1000;
	CHECK(capacity == 16000);
}

int main(void)
{
	test_ring_round_trip();
	test_ring_partial_read();
	test_ring_overflow_drops_oldest();
	test_ring_push_larger_than_capacity();
	test_ring_reset();
	test_ring_zero_capacity();
	test_block_levels();
	test_amplitude_to_db();
	test_buffer_capacity_constant();
	return 0;
}
