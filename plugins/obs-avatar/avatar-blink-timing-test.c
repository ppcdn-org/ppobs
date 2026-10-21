/* The timing of the blink, without a GPU: how often, how long, how shut.
 *
 * A blink on a clock looks mechanical sooner than no blink looks wrong, so the
 * properties that matter are statistical: roughly every four seconds but never
 * the same gap twice, now and then a second blink right behind the first, the
 * lid closing faster than it opens, and never a value outside 0..1. Ten minutes
 * of video is sampled at 25 fps, the rate of the canvas this was tuned on.
 */

#include "avatar-blink.h"
#include "avatar-test-check.h"

#include <math.h>
#include <stdio.h>

#define FPS 25
#define MINUTES 10

int main(void)
{
	struct avatar_blink blink;
	const uint64_t start = 1000000000ULL;
	avatar_blink_init(&blink, start);

	const int frames = FPS * 60 * MINUTES;
	int blinks = 0;
	int doubles = 0;
	double max_amount = 0.0;
	double longest_blink_s = 0.0;
	double first_blink_at_s = -1.0;
	double last_blink_start_s = -100.0;

	bool in_blink = false;
	double blink_began_s = 0.0;
	double peak_at_s = 0.0;
	double peak = 0.0;
	double close_time_total = 0.0;
	double open_time_total = 0.0;
	double min_gap_s = 1e9;
	double max_gap_s = 0.0;

	for (int i = 0; i < frames; i++) {
		const double t = (double)i / FPS;
		const float a = avatar_blink_amount(&blink, start + (uint64_t)(t * 1e9));

		CHECK(a >= 0.0f && a <= 1.0f);
		CHECK(isfinite(a));

		if (a > max_amount)
			max_amount = a;

		if (a > 0.0f && !in_blink) {
			in_blink = true;
			blink_began_s = t;
			peak = a;
			peak_at_s = t;
			blinks++;
			if (first_blink_at_s < 0)
				first_blink_at_s = t;

			const double gap = t - last_blink_start_s;
			if (last_blink_start_s > 0.0) {
				if (gap < min_gap_s)
					min_gap_s = gap;
				if (gap > max_gap_s)
					max_gap_s = gap;
				if (gap < 1.0)
					doubles++;
			}
			last_blink_start_s = t;
		} else if (a > 0.0f && in_blink) {
			if (a > peak) {
				peak = a;
				peak_at_s = t;
			}
		} else if (a == 0.0f && in_blink) {
			in_blink = false;
			const double length = t - blink_began_s;
			if (length > longest_blink_s)
				longest_blink_s = length;
			close_time_total += peak_at_s - blink_began_s;
			open_time_total += t - peak_at_s;
		}
	}

	const double mean_gap = (double)MINUTES * 60.0 / (double)blinks;
	printf("%d blinks in %d minutes (%d of them doubles); mean gap %.1f s, gaps %.2f..%.1f s\n", blinks, MINUTES,
	       doubles, mean_gap, min_gap_s, max_gap_s);
	printf("longest blink %.2f s, closing %.0f ms on average, opening %.0f ms\n", longest_blink_s,
	       1000.0 * close_time_total / blinks, 1000.0 * open_time_total / blinks);

	/* Not before the first second or so: a source that has just appeared should not blink at once. */
	CHECK(first_blink_at_s >= 1.0);

	/* Fully shut at the peak. */
	CHECK(max_amount > 0.95);

	/* About every four seconds, with doubles pulling the mean a little under. */
	CHECK(mean_gap > 2.5 && mean_gap < 5.5);

	/* Neither a stuck lid nor a flutter. */
	CHECK(longest_blink_s > 0.15 && longest_blink_s < 0.35);

	/* Some doubles, not most. */
	CHECK(doubles > blinks / 25);
	CHECK(doubles < blinks / 3);

	/* The lid comes down faster than it goes up. */
	CHECK(close_time_total < open_time_total);

	/* The gaps are not all alike. */
	CHECK(max_gap_s - min_gap_s > 2.0);

	printf("PASS\n");
	return 0;
}
