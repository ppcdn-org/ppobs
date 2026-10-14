#include "PPCDNPublishEndpoint.hpp"
#include "RemoteTextThread.hpp"

#include <qt-wrappers.hpp>

#include <QUrl>

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

	// The pathname may also carry the SRT publish token as a third
	// colon-separated segment ("<appId>/<streamName>:<token>", see
	// rtmp-services/rtmp-custom.c's inject_ppcdn_publish_token). The appId
	// and stream-name segments never contain ':', so the first remaining
	// ':' starts the token. Drop it - PPCenter wants only the stream name.
	int token = value.indexOf(':');
	if (token >= 0)
		value = value.left(token);

	// The pathname is "<appId>/<streamName>"; PPCenter wants the stream name
	// on its own (<appId> is sent separately). If the appId segment isn't
	// there, keep the whole value rather than guessing.
	if (!appId.isEmpty()) {
		const QString prefix = appId + "/";
		if (value.startsWith(prefix))
			value = value.mid(prefix.size());
	}

	// A publish path may end in a codec segment ("/h264" or "/hevc" - see
	// PublishCodecCheck.hpp) which selects the codec, not part of the stream
	// name. PPCenter knows the stream without it, so sending the suffixed
	// name (e.g. "B01-frontView/h264") is rejected with HTTP 400.
	for (const char *codec : {"h264", "hevc"}) {
		const QString suffix = QStringLiteral("/") + codec;
		if (value.endsWith(suffix))
			value.chop(suffix.size());
	}

	return value.trimmed();
}

// CustomLadderProtocol returns "srt"/"rtmp" for a custom rtmp_custom service
// whose configured Server URL scheme is one PPCenter can dynamically resolve
// an Origin address for (see ResolvePPCDNPublishEndpoint), or "" for
// anything else - WHIP, rtmp_common, or a custom service whose URL resolved
// to a protocol this feature doesn't cover (RIST/RTMPS). "rtmp" covers both
// the "ERTMP" service preset and a plain "自定义/Custom" rtmp:// URL alike:
// they're otherwise indistinguishable at this layer (see
// OBSBasicSettings_Stream.cpp's ListOpt::ERTMP comment), and both get the
// same dynamic-resolution behavior when they carry the placeholder.
std::string CustomLadderProtocol(obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	const char *id = obs_service_get_id(service);
	if (!protocol || !id || strcmp(id, "rtmp_custom") != 0)
		return "";
	if (astrcmpi(protocol, "srt") == 0)
		return "srt";
	if (astrcmpi(protocol, "rtmp") == 0)
		return "rtmp";
	return "";
}

// ParseRTMPStreamName extracts the stream name from an RTMP multitrack
// publish URL, whose path is "{appId}/{streamName}" with no trailing codec
// segment (see PPCDN_ERTMP_DEFAULT_PUBLISH_URL) - unlike
// ParseWHIPStreamNameFromServerUrl's path (.../whip), the stream name here
// is simply the last path segment.
QString ParseRTMPStreamName(const QString &url)
{
	QStringList segments = QUrl(url.trimmed()).path().split('/', Qt::SkipEmptyParts);
	if (segments.isEmpty())
		return QString();
	return segments.last();
}

// DegradeWsURLFromWhipURL turns a per-codec WHIP publish URL into its degrade
// WS control URL, identical to WsDegradeClient::whip_to_ws
// (plugins/obs-webrtc/degrade-client.cpp) - duplicated here rather than
// shared because obs-ffmpeg (which actually opens this connection, see
// srt-degrade-client.cpp) is a separate plugin module from obs-webrtc and
// this frontend code links against neither. Returns empty on an unexpected
// shape rather than guessing.
std::string DegradeWsURLFromWhipURL(std::string url)
{
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	if (url.rfind("https://", 0) == 0)
		url = "wss://" + url.substr(8);
	else if (url.rfind("http://", 0) == 0)
		url = "ws://" + url.substr(7);
	else
		return "";
	const size_t whipAt = url.rfind("/whip");
	if (whipAt == std::string::npos || whipAt + 5 != url.size())
		return "";
	return url.substr(0, whipAt) + "/ws/whip";
}

// ppobs's "ppcenter_url" setting may be the bare origin
// ("https://api.pp-cdn.org") or the full publish endpoint; this returns the
// publish endpoint either way so the SRT node request below POSTs to the right
// URL. Mirrors the plugin's DerivePpcenterPublishUrl
// (plugins/obs-webrtc/ppcenter-time-sync-utils.cpp).
std::string NormalizePpcenterPublishUrl(const std::string &url)
{
	const auto schemeEnd = url.find("://");
	if (schemeEnd == std::string::npos)
		return "";
	const std::string scheme = url.substr(0, schemeEnd);
	if (scheme != "http" && scheme != "https")
		return "";
	const size_t hostStart = schemeEnd + 3;
	const size_t pathStart = url.find('/', hostStart);
	const std::string host = (pathStart == std::string::npos) ? url.substr(hostStart)
								  : url.substr(hostStart, pathStart - hostStart);
	if (host.empty())
		return "";
	return scheme + "://" + host + "/v1/publish/requests";
}

} // namespace

bool ResolvePPCDNPublishEndpoint(obs_service_t *service, std::string &error)
{
	if (!service)
		return true;
	const std::string protocol = CustomLadderProtocol(service);
	if (protocol.empty())
		return true;

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	const char *rawServer = obs_data_get_string(settings, "server");
	if (!rawServer || !*rawServer)
		return true;

	// Only services configured with the placeholder need a PPCenter round
	// trip; a literal SRT/RTMP URL keeps working exactly as before.
	if (!strstr(rawServer, PPCDN_PUBLISH_ENDPOINT_PLACEHOLDER))
		return true;

	const std::string ppcenterURL = NormalizePpcenterPublishUrl(obs_data_get_string(settings, "ppcenter_url"));
	const std::string appID = obs_data_get_string(settings, "ppcenter_appid");
	const std::string appSecret = obs_data_get_string(settings, "ppcenter_secret");
	const std::string region = obs_data_get_string(settings, "ppcenter_region");
	if (ppcenterURL.empty() || appID.empty() || appSecret.empty()) {
		error = "PPCenter URL, app ID and app secret are required to resolve the " +
			QString(protocol.c_str()).toUpper().toStdString() + " publish node";
		return false;
	}

	// Resolve the stream name from the *expanded* URL (the app id segment is
	// already substituted there by rtmp_custom_update). SRT packs it into the
	// "?streamid=publish:<appId>/<stream>" query; RTMP's path is plainly
	// "<appId>/<stream>" with nothing to strip (see ParseRTMPStreamName).
	const char *expanded = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const QString expandedUrl = QT_UTF8(expanded && *expanded ? expanded : rawServer);
	const QString streamName = protocol == "srt" ? ParseSRTStreamName(expandedUrl, QT_UTF8(appID.c_str()))
						      : ParseRTMPStreamName(expandedUrl);
	if (streamName.isEmpty()) {
		error = "Cannot determine the stream name from the configured URL";
		return false;
	}

	nlohmann::json body = {
		{"appId", appID},
		{"appSecret", appSecret},
		{"streamName", streamName.toStdString()},
		{"protocol", protocol},
		// Always requested, same as every WHIP publish (see
		// ppcenter-client.cpp's ppcenter_build_publish_json): gets back a
		// per-codec WHIP-style bearer token this SRT/RTMP publish doesn't
		// otherwise have, used only to open the degrade WS control channel
		// (docs/design/publish-degrade-protocol.zh-CN.md §6.1) - it never
		// switches transport. Omitted from the response (whipTracks stays
		// unset) when the picked Origin has no WebRTC base URL configured;
		// handled as a plain "degrade unavailable" case below, not an error.
		{"capabilities", nlohmann::json::array({"whip-hevc-h264"})},
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
		error = "PPCenter rejected the " + QString(protocol.c_str()).toUpper().toStdString() +
			" publish-node request (HTTP " + std::to_string(status) + ")";
		return false;
	}

	std::string endpoint;
	std::string publishToken;
	std::string publishTokenH264;
	std::string publishTokenHEVC;
	std::string degradeWsURLH264, degradeWsTokenH264;
	std::string degradeWsURLHEVC, degradeWsTokenHEVC;
	try {
		const auto decoded = nlohmann::json::parse(response);
		endpoint = decoded.value("ppcdnPublishEndpoint", "");
		publishToken = decoded.value("publishToken", "");
		// Per-codec SRT tokens for an HEVC/H264 multitrack publish: each SRT
		// connection carries its own codec path ("<appId>/<stream>/h264" or
		// ".../hevc"), and mmx binds a token to the exact path (see
		// ppcenter's PublishTokens). The codec-less publishToken above stays
		// for a single-codec SRT publish.
		const auto tokens = decoded.find("publishTokens");
		if (tokens != decoded.end() && tokens->is_object()) {
			publishTokenH264 = tokens->value("h264", "");
			publishTokenHEVC = tokens->value("hevc", "");
		}
		// whipTracks (requested via "whip-hevc-h264" above): per-codec WHIP
		// URL + bearer token, absent when the Origin has no WebRTC base URL
		// configured. Turn the WHIP URL into its degrade WS URL here -
		// obs-ffmpeg's SRT output just needs a ready wss:// URL, it never
		// parses a WHIP URL itself.
		const auto whipTracks = decoded.find("whipTracks");
		if (whipTracks != decoded.end() && whipTracks->is_object()) {
			if (const auto h264 = whipTracks->find("h264"); h264 != whipTracks->end()) {
				degradeWsURLH264 = DegradeWsURLFromWhipURL(h264->value("url", ""));
				degradeWsTokenH264 = h264->value("bearerToken", "");
			}
			if (const auto hevc = whipTracks->find("hevc"); hevc != whipTracks->end()) {
				degradeWsURLHEVC = DegradeWsURLFromWhipURL(hevc->value("url", ""));
				degradeWsTokenHEVC = hevc->value("bearerToken", "");
			}
		}
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
	// The SRT publish token (mmx srtPublishTokenRequired) goes into its own
	// setting so rtmp_custom_update injects it as the streamid's third
	// segment. Absent for RTMP or an Origin without the check, so store the
	// value unconditionally (empty clears any stale token from a prior run).
	obs_data_set_string(settings, "ppcdn_publish_token", publishToken.c_str());
	// Per-codec SRT tokens, consumed by BasicOutputHandler's
	// BuildCodecServiceSettings when it derives the /h264 and /hevc publish
	// services of an HEVC/H264 multitrack publish.
	obs_data_set_string(settings, "ppcdn_publish_token_h264", publishTokenH264.c_str());
	obs_data_set_string(settings, "ppcdn_publish_token_hevc", publishTokenHEVC.c_str());
	// Degrade WS control-channel credentials, read by obs-ffmpeg-mpegts.c's
	// fetch_service_info() (see srt-degrade-client.cpp). Empty when the
	// Origin has no WebRTC base URL - that output just runs without the
	// degrade connection, same as today, rather than failing the publish.
	obs_data_set_string(settings, "ppcdn_degrade_ws_url_h264", degradeWsURLH264.c_str());
	obs_data_set_string(settings, "ppcdn_degrade_ws_token_h264", degradeWsTokenH264.c_str());
	obs_data_set_string(settings, "ppcdn_degrade_ws_url_hevc", degradeWsURLHEVC.c_str());
	obs_data_set_string(settings, "ppcdn_degrade_ws_token_hevc", degradeWsTokenHEVC.c_str());
	// Persist the resolved stream name for the P2P companion output. The
	// settings-save path (OBSBasicSettings_Stream.cpp) fills "ppcenter_stream"
	// with a WHIP URL-*path* parser (ParseWHIPStreamNameFromServerUrl, which
	// expects a trailing "/whip" segment) that gets the wrong answer for
	// both protocols here: "" for SRT's "?streamid=publish:<appId>/<stream>"
	// URL (the name lives in the query, not the path) and the *appId*
	// instead of the stream name for RTMP's plain two-segment path (no
	// "/whip" to offset against). Either wrong value would make the
	// companion's WHIPOutput::Setup() fail both the NAT probe and
	// ppcenter_resolve_publish ("stream name required"), leaving the
	// SRT/RTMP publisher out of the P2P mesh so every play decision falls
	// back to edge-only. streamName here is parsed from the *expanded* URL
	// above with the protocol-correct parser, so overwrite the wrong value
	// with it.
	obs_data_set_string(settings, "ppcenter_stream", streamName.toStdString().c_str());
	obs_service_update(service, settings);
	return true;
}
