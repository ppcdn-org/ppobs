/* Strips the SRT publish token out of a URL before it is logged.
 *
 * mmx's srtPublishTokenRequired puts the token in the streamid pathname -
 * "streamid=publish:<appId>/<stream>:<token>", injected by
 * rtmp-services/rtmp-custom.c's inject_ppcdn_publish_token - so the publish
 * URL is a credential-bearing string. Several log lines print that URL on
 * error paths (a failed connection, a close error), and obs.log is the first
 * thing a user pastes into a support thread, so the token would travel with
 * it.
 *
 * The stream name is left in: it is what makes those log lines useful, and it
 * is not a secret. Only the token segment goes.
 *
 * The bounds logic mirrors the injection deliberately - same marker, same
 * "pathname ends at '&' or an SRT field-list ','", same "the action prefix is
 * the only expected ':'" rule. If one side's idea of the shape drifts, the
 * other should be changed with it.
 */

#pragma once

#include <util/dstr.h>

#include <string.h>

#define PUBLISH_URL_TOKEN_MARKER "streamid="
#define PUBLISH_URL_REDACTED ":<token redacted>"

/* Returns a string safe to log. When there is nothing to redact, that is `url`
 * itself and `out` is left empty; otherwise it is `out->array`. Either way the
 * caller owns `out` and must dstr_free it.
 *
 * Returning the original on the common path keeps this allocation-free for
 * every URL that carries no token, which is most of them. */
static inline const char *publish_url_redact(struct dstr *out, const char *url)
{
	dstr_init(out);

	if (!url || !*url)
		return url;

	const char *value = strstr(url, PUBLISH_URL_TOKEN_MARKER);
	if (!value)
		return url;
	value += sizeof(PUBLISH_URL_TOKEN_MARKER) - 1;

	/* The streamid value ends at the next '&'; its pathname ends earlier if
	 * an SRT field list follows. */
	const char *limit = strchr(value, '&');
	if (!limit)
		limit = value + strlen(value);

	const char *comma = strchr(value, ',');
	if (comma && comma < limit)
		limit = comma;

	/* One ':' is the "publish:"/"read:" action prefix. A second one is the
	 * token, and everything from it to the limit is the secret.
	 *
	 * The casts are not decoration: memchr yields void* in C but is
	 * overloaded to const void* in C++, which will not convert implicitly.
	 * The other header in this directory is already included from C++
	 * (srt-degrade-client.cpp), so this one has to compile there too. */
	const char *first_colon = (const char *)memchr(value, ':', (size_t)(limit - value));
	if (!first_colon)
		return url;

	const char *token = (const char *)memchr(first_colon + 1, ':', (size_t)(limit - first_colon - 1));
	if (!token)
		return url;

	dstr_ncopy(out, url, (size_t)(token - url));
	dstr_cat(out, PUBLISH_URL_REDACTED);
	dstr_cat(out, limit);

	return out->array ? out->array : url;
}
