#pragma once

#include <string>

#include <obs.hpp>

// The SRT publish address is chosen by PPCenter, not the user: PPCDN returns
// the selected Origin's ingest address (ppcdn_publish_endpoint) from
// POST /v1/publish/requests with protocol="srt". ppobs keeps the configured
// Server URL as a template carrying the "{ppcdn_publish_endpoint}" placeholder
// and substitutes the resolved address at stream start. See ppcenter's
// docs/api/streaming.md.
#define PPCDN_PUBLISH_ENDPOINT_PLACEHOLDER "{ppcdn_publish_endpoint}"

// Default SRT Server URL used when the field is left empty. "{ppcenter_appid}"
// is expanded by the service itself (rtmp_custom_update), the endpoint
// placeholder by ResolvePPCDNPublishEndpoint() below.
#define PPCDN_SRT_DEFAULT_PUBLISH_URL \
	"srt://" PPCDN_PUBLISH_ENDPOINT_PLACEHOLDER "?streamid=publish:{ppcenter_appid}/B01-frontView"

// ResolvePPCDNPublishEndpoint is a no-op returning true for anything that
// isn't a custom SRT service still carrying the placeholder. For an SRT
// template it requests the Origin's ingest address from PPCenter and writes it
// back into the service settings as "ppcdn_publish_endpoint" (which
// rtmp_custom_update then expands), so the subsequent publish uses the real
// address. Returns false with `error` set on failure.
bool ResolvePPCDNPublishEndpoint(obs_service_t *service, std::string &error);
