#pragma once

#include <cstdint>
#include <string>

// ppobs stores its ppcenter address in one setting ("ppcenter_url"), which may
// be either the full publish endpoint ("https://api.pp-cdn.org/v1/publish/
// requests", the historical value) or the bare origin
// ("https://api.pp-cdn.org"). Every consumer derives the endpoint it needs
// from this single value, so both forms keep working and no second config item
// is required. DerivePpcenterPublishUrl rebuilds the publish URL and
// DerivePpcenterTimeSyncUrl maps the same origin onto pplayer's /ws/play clock
// probe - the shared time base introduced for ppobs<->pplayer delay alignment.

// Returns "scheme://host[:port]" for an http(s) URL, or "" when the URL has no
// usable scheme/host.
std::string DerivePpcenterOrigin(const std::string &ppcenterUrl);

// The /v1/publish/requests endpoint on the configured ppcenter origin.
std::string DerivePpcenterPublishUrl(const std::string &ppcenterUrl);

// pplayer's clock-sync WebSocket: ws(s)://host/ws/play on the same origin.
std::string DerivePpcenterTimeSyncUrl(const std::string &ppcenterUrl);

// pplayer's four-timestamp offset: offset = ((t2 - t1) + (t3 - t4)) / 2, i.e.
// (ppcenterClock - localClock). Add it to the local wall clock reading.
int64_t ComputePpcenterClockOffsetMs(int64_t t1, int64_t t2, int64_t t3, int64_t t4);

// Extracts a flat integer JSON field (e.g. "t2") from ppcenter's TIME_SYNC_ACK
// body. Returns false when the field is absent or not an integer.
bool ExtractJsonIntField(const std::string &json, const char *field, int64_t *out);
