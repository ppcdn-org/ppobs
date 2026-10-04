#include "ppcenter-time-sync-utils.h"

#include <cstdlib>

namespace {
// Splits an http(s) URL into its scheme and authority ("host[:port]"). The
// path, query and any userinfo are discarded - callers only need the origin.
bool ParseUrlOrigin(const std::string &url, std::string *scheme, std::string *host)
{
	const auto schemeEnd = url.find("://");
	if (schemeEnd == std::string::npos)
		return false;

	const std::string s = url.substr(0, schemeEnd);
	if (s != "http" && s != "https")
		return false;

	const size_t hostStart = schemeEnd + 3;
	const size_t pathStart = url.find('/', hostStart);
	std::string h = (pathStart == std::string::npos) ? url.substr(hostStart)
							 : url.substr(hostStart, pathStart - hostStart);
	// Drop a userinfo prefix ("user:pass@host") so the origin is a bare host.
	const size_t at = h.rfind('@');
	if (at != std::string::npos)
		h = h.substr(at + 1);
	if (h.empty())
		return false;

	*scheme = s;
	*host = h;
	return true;
}
} // namespace

std::string DerivePpcenterOrigin(const std::string &ppcenterUrl)
{
	std::string scheme, host;
	if (!ParseUrlOrigin(ppcenterUrl, &scheme, &host))
		return "";
	return scheme + "://" + host;
}

std::string DerivePpcenterPublishUrl(const std::string &ppcenterUrl)
{
	const std::string origin = DerivePpcenterOrigin(ppcenterUrl);
	if (origin.empty())
		return "";
	return origin + "/v1/publish/requests";
}

std::string DerivePpcenterTimeSyncUrl(const std::string &ppcenterUrl)
{
	std::string scheme, host;
	if (!ParseUrlOrigin(ppcenterUrl, &scheme, &host))
		return "";
	const std::string wsScheme = (scheme == "https") ? "wss" : "ws";
	return wsScheme + "://" + host + "/ws/play";
}

int64_t ComputePpcenterClockOffsetMs(int64_t t1, int64_t t2, int64_t t3, int64_t t4)
{
	return ((t2 - t1) + (t3 - t4)) / 2;
}

bool ExtractJsonIntField(const std::string &json, const char *field, int64_t *out)
{
	// The TIME_SYNC_ACK body is a flat object of integers, so a full JSON
	// parser is unnecessary: find "field", then the ':' after it, skip
	// whitespace and parse an optional sign plus digits.
	const std::string key = std::string("\"") + field + "\"";
	const size_t keyAt = json.find(key);
	if (keyAt == std::string::npos)
		return false;
	const size_t colon = json.find(':', keyAt + key.size());
	if (colon == std::string::npos)
		return false;

	size_t i = colon + 1;
	while (i < json.size() && (json[i] == ' ' || json[i] == '\t'))
		++i;
	const size_t start = i;
	if (i < json.size() && (json[i] == '-' || json[i] == '+'))
		++i;
	const size_t digitsStart = i;
	while (i < json.size() && json[i] >= '0' && json[i] <= '9')
		++i;
	if (i == digitsStart)
		return false;

	try {
		*out = std::stoll(json.substr(start, i - start));
	} catch (...) {
		return false;
	}
	return true;
}
