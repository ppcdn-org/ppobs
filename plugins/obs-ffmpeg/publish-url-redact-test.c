#include "publish-url-redact.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Self-contained rather than reaching into another plugin's test header: this
 * test belongs to obs-ffmpeg, and a cross-plugin include would tie the two
 * together for four lines of code.
 *
 * Not assert(): NDEBUG is defined in RelWithDebInfo, which is what this repo
 * builds, and assert() expands to nothing there - the test would run, exit 0
 * and verify nothing. */
static void check(bool ok, const char *expr, int line)
{
	if (ok)
		return;

	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expr);
	fflush(stderr);
	exit(1);
}

#define CHECK(expr) check((expr), #expr, __LINE__)

/* The shapes come from rtmp-services/rtmp-custom.c's
 * inject_ppcdn_publish_token, which is what produces them. */

static void test_redacts_the_token(void)
{
	struct dstr out;
	const char *url = "srt://ingest.example:9000?streamid=publish:app1/B01-frontView:abc123TOKEN";
	const char *safe = publish_url_redact(&out, url);

	/* The token must be gone, and the stream name must not be: the whole
	 * point of logging the URL is knowing which publish failed. */
	CHECK(strstr(safe, "abc123TOKEN") == NULL);
	CHECK(strstr(safe, "B01-frontView") != NULL);
	CHECK(strstr(safe, "publish:app1/") != NULL);
	CHECK(strstr(safe, PUBLISH_URL_REDACTED) != NULL);
	dstr_free(&out);
}

static void test_keeps_what_follows(void)
{
	struct dstr out;

	/* An SRT field list follows the pathname after a ',' - it is not part of
	 * the token and has to survive. */
	const char *fields = "srt://h:9000?streamid=publish:app1/s1:TOK,latency=200&foo=bar";
	const char *safe = publish_url_redact(&out, fields);
	CHECK(strstr(safe, "TOK,") == NULL);
	CHECK(strstr(safe, ",latency=200") != NULL);
	CHECK(strstr(safe, "&foo=bar") != NULL);
	dstr_free(&out);

	/* Same for a following query parameter when there is no field list. */
	const char *query = "srt://h:9000?streamid=publish:app1/s1:TOK&passphrase=x";
	safe = publish_url_redact(&out, query);
	CHECK(strstr(safe, ":TOK") == NULL);
	CHECK(strstr(safe, "&passphrase=x") != NULL);
	dstr_free(&out);
}

static void test_leaves_untokened_urls_alone(void)
{
	struct dstr out;

	/* Only the action prefix's ':' - no token to remove. Returned as-is and
	 * without allocating, which is the common case. */
	const char *plain = "srt://h:9000?streamid=publish:app1/s1";
	CHECK(publish_url_redact(&out, plain) == plain);
	dstr_free(&out);

	/* No streamid at all (RTMP, WHIP, a recording path). */
	const char *rtmp = "rtmp://h/live/streamkey";
	CHECK(publish_url_redact(&out, rtmp) == rtmp);
	dstr_free(&out);

	/* A streamid with no colon whatsoever. */
	const char *bare = "srt://h:9000?streamid=app1";
	CHECK(publish_url_redact(&out, bare) == bare);
	dstr_free(&out);
}

static void test_degenerate_input(void)
{
	struct dstr out;

	CHECK(publish_url_redact(&out, NULL) == NULL);
	dstr_free(&out);

	const char *empty = "";
	CHECK(publish_url_redact(&out, empty) == empty);
	dstr_free(&out);

	/* A truncated streamid must not read past the end. */
	const char *cut = "srt://h:9000?streamid=";
	CHECK(publish_url_redact(&out, cut) == cut);
	dstr_free(&out);
}

int main(void)
{
	test_redacts_the_token();
	test_keeps_what_follows();
	test_leaves_untokened_urls_alone();
	test_degenerate_input();
	return 0;
}
