/* A check that survives the configuration the tests are actually built in.
 *
 * assert() expands to nothing when NDEBUG is defined, and MSVC's
 * RelWithDebInfo flags define it - which is the configuration this repo
 * builds. A test written with assert() therefore compiles down to a program
 * that runs, exits 0, and verifies nothing at all, while looking exactly like
 * a passing test. (It also tends to announce itself first as an
 * unused-variable warning, for any variable only read inside an assert.)
 *
 * CHECK is a real branch, so it holds in every configuration.
 */

#pragma once

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static inline void avatar_check(bool ok, const char *expr, const char *file, int line)
{
	if (ok)
		return;

	fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
	fflush(stderr);
	exit(1);
}

#define CHECK(expr) avatar_check((expr), #expr, __FILE__, __LINE__)
