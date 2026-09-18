#include "PPCDNPublishEndpoint.hpp"
#include "RemoteTextThread.hpp"

#include <qt-wrappers.hpp>

#include <util/dstr.h>

#include <nlohmann/json.hpp>

#include <cstring>
#include <vector>

namespace {

// The streamid pathname inside an SRT publish URL, e.g.
// "srt://host:7890?streamid=publish:<appId>/B01-frontView". Returns the
// PPCenter stream name ("B01-frontView") or an empty string when the URL
// carries no usable streamid. Mirrors the server-side parsing in
// PublishCodecCheck.hpp / mmx: the value runs to the next '&', starts with an
// action ("publish:"/"read:"), and may append a ",key=value" field list after
// the pathname.
QString ParseSRTStreamName(const QString &url, const QString &appId)
{
	const QString marker = "streamid=";
	int at = url.indexOf(marker);
	if (at < 0)
		return QString();

	int start = at + marker.size();
	int end = url.indexOf('&', start);
	QString value = (end < 0) ? url.mid(start) : url.mid(start, end - start);

	// Drop the action prefix ("publish:" / "read:").
	int colon = value.indexOf(':');
	if (colon >= 0)
		value = value.mid(colon + 1);

	// A pathname may carry a trailing SRT field list.
	int comma = value.indexOf(',');
	if (comma >= 0)
		value = value.left(comma);

	// The pathname is "<appId>/<streamName>"; PPCenter wants the stream name
	// on its own (<appId> is sent separately). If the appId segment isn't
	// there, keep the whole value rather than guessing.
	if (!appId.isEmpty()) {
		const QString prefix = appId + "/";
		if (value.startsWith(prefix))
			value = value.mid(prefix.size());
	}

	return value.trimmed();
}

bool IsCustomSRTService(obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	const char *id = obs_service_get_id(service);
	return protocol && astrcmpi(protocol, "srt") == 0 && id && strcmp(id, "rtmp_custom") == 0;
}

} // namespace

bool ResolvePPCDNPublishEndpoint(obs_service_t *service, std::string &error)
{
	if (!service || !IsCustomSRTService(service))
		return true;

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	const char *rawServer = obs_data_get_string(settings, "server");
	if (!rawServer || !*rawServer)
		return true;

	// Only services configured with the placeholder need a PPCenter round
	// trip; a literal SRT URL keeps working exactly as before.
	if (!strstr(rawServer, PPCDN_PUBLISH_ENDPOINT_PLACEHOLDER))
		return true;

	const std::string ppcenterURL = obs_data_get_string(settings, "ppcenter_url");
	const std::string appID = obs_data_get_string(settings, "ppcenter_appid");
	const std::string appSecret = obs_data_get_string(settings, "ppcenter_secret");
	const std::string region = obs_data_get_string(settings, "ppcenter_region");
	if (ppcenterURL.empty() || appID.empty() || appSecret.empty()) {
		error = "PPCenter URL, app ID and app secret are required to resolve the SRT publish node";
		return false;
	}

	// Resolve the stream name from the *expanded* URL (the app id segment is
	// already substituted there by rtmp_custom_update).
	const char *expanded = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const QString streamName = ParseSRTStreamName(QT_UTF8(expanded && *expanded ? expanded : rawServer),
						      QT_UTF8(appID.c_str()));
	if (streamName.isEmpty()) {
		error = "Cannot determine the SRT stream name from the configured URL";
		return false;
	}

	nlohmann::json body = {
		{"appId", appID},
		{"appSecret", appSecret},
		{"streamName", streamName.toStdString()},
		{"protocol", "srt"},
	};
	if (!region.empty())
		body["requestRegion"] = region;

	const std::string payload = body.dump();
	std::string response;
	std::string libraryError;
	long status = 0;
	std::vector<std::string> extraHeaders;
	const bool ok = GetRemoteFile(ppcenterURL.c_str(), response, libraryError, &status, "application/json", "POST",
				      payload.c_str(), extraHeaders, nullptr, 8, false,
				      static_cast<int>(payload.size()));
	if (!ok) {
		error = "PPCenter publish-node request failed: " +
			(libraryError.empty() ? std::string("network error") : libraryError);
		return false;
	}
	if (status != 200) {
		error = "PPCenter rejected the SRT publish-node request (HTTP " + std::to_string(status) + ")";
		return false;
	}

	std::string endpoint;
	try {
		const auto decoded = nlohmann::json::parse(response);
		endpoint = decoded.value("ppcdnPublishEndpoint", "");
	} catch (const std::exception &) {
		error = "PPCenter returned an invalid publish-node response";
		return false;
	}
	if (endpoint.empty()) {
		error = "PPCenter response is missing ppcdnPublishEndpoint";
		return false;
	}

	// Write it back into the service settings so rtmp_custom_update expands
	// the placeholder into the address the output actually connects to.
	obs_data_set_string(settings, "ppcdn_publish_endpoint", endpoint.c_str());
	obs_service_update(service, settings);
	return true;
}
