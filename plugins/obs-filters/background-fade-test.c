/* Covers the staleness ramp applied to the matte.
 *
 * Mirrors the arithmetic in background_sync_mask_texture; the real function
 * needs a graphics context and a running worker, but the ramp is where the
 * user-visible behaviour lives.  Kept in sync by hand.
 *
 * The filter used to switch straight from "matte applied" to "source
 * untouched" the moment the matte aged out, which on screen reads as the real
 * background appearing all at once.  Fading instead turns that into the matte
 * softening, which is far less obvious.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define FADE_START_NS 400000000ULL
#define TIMEOUT_NS 1000000000ULL

static float strength_for_age(uint64_t age_ns, bool published)
{
	if (!published)
		return 0.0f;

	if (age_ns <= FADE_START_NS)
		return 1.0f;
	if (age_ns >= TIMEOUT_NS)
		return 0.0f;

	const uint64_t span = TIMEOUT_NS - FADE_START_NS;
	return 1.0f - (float)(age_ns - FADE_START_NS) / (float)span;
}

static bool nearly(float a, float b)
{
	const float d = a - b;
	return (d < 0 ? -d : d) < 0.001f;
}

static void test_fresh_matte_fully_applied(void)
{
	/* Anything inside the fade-start window is the normal case and must be
	 * applied at full strength - no partial transparency while the worker
	 * is keeping up. */
	assert(nearly(strength_for_age(0, true), 1.0f));
	assert(nearly(strength_for_age(100000000ULL, true), 1.0f));
	assert(nearly(strength_for_age(FADE_START_NS, true), 1.0f));
}

static void test_ramp_is_monotonic(void)
{
	/* Strength must only ever decrease with age; a non-monotonic ramp would
	 * show up as the background flickering in and out. */
	float previous = 1.0f;

	for (uint64_t age = FADE_START_NS; age <= TIMEOUT_NS; age += 25000000ULL) {
		const float current = strength_for_age(age, true);
		assert(current <= previous + 0.001f);
		assert(current >= 0.0f && current <= 1.0f);
		previous = current;
	}
}

static void test_midpoint(void)
{
	/* Halfway through the fade window should be about half strength. */
	const uint64_t middle = FADE_START_NS + (TIMEOUT_NS - FADE_START_NS) / 2;
	assert(nearly(strength_for_age(middle, true), 0.5f));
}

static void test_expiry(void)
{
	/* At and past the timeout the matte is gone, which makes the caller
	 * fall back to the untouched source. */
	assert(nearly(strength_for_age(TIMEOUT_NS, true), 0.0f));
	assert(nearly(strength_for_age(TIMEOUT_NS + 1, true), 0.0f));
	assert(nearly(strength_for_age(TIMEOUT_NS * 10, true), 0.0f));
}

static void test_no_matte(void)
{
	/* Before the first inference completes there is nothing to apply, at
	 * any age. */
	assert(nearly(strength_for_age(0, false), 0.0f));
	assert(nearly(strength_for_age(FADE_START_NS, false), 0.0f));
}

static void test_covers_realistic_inference_rates(void)
{
	/* At the rates the UI offers, a matte produced one interval ago must
	 * still be at full strength - otherwise normal operation would show
	 * constant partial fading rather than only degrading when the worker
	 * genuinely falls behind. */
	const int rates[] = {5, 10, 15, 20, 30};

	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		const uint64_t interval = 1000000000ULL / (uint64_t)rates[i];
		const float strength = strength_for_age(interval, true);

		if (rates[i] >= 10) {
			assert(nearly(strength, 1.0f));
		} else {
			/* 5 FPS has a 200ms interval, still inside the window. */
			assert(strength > 0.0f);
		}
	}
}

int main(void)
{
	test_fresh_matte_fully_applied();
	test_ramp_is_monotonic();
	test_midpoint();
	test_expiry();
	test_no_matte();
	test_covers_realistic_inference_rates();

	printf("PASSED\n");
	return 0;
}
