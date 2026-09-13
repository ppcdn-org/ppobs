#pragma once

#include <string>

#include <obs.hpp>

// A SRT publish URL may pin the stream to one codec by ending its streamid
// pathname in /h264 or /hevc (see the HEVC/H264 multitrack design's §3.1).
// The server can only discover the actual codec once the MPEG-TS PMT
// arrives, so a mismatch is rejected *after* the connection is established -
// which OBS sees as a broken connection and retries forever, with nothing in
// its own log explaining why. These helpers let the frontend catch the same
// mismatches before starting, and say so once.

// PublishCodecFromURL returns the codec a publish URL pins itself to
// ("h264"/"hevc"), or an empty string when the URL carries no codec segment
// and is therefore unconstrained. Anything that isn't a known codec is a
// plain stream name, matching the server's own parsing.
inline std::string PublishCodecFromURL(const std::string &url)
{
	// The codec segment is the last path component of the streamid, but a
	// SRT URL carries query parameters after it ("?streamid=..." plus
	// whatever else), so scan the whole string for the suffix rather than
	// trying to parse out the streamid.
	auto endsWithSegment = [&url](const char *segment) {
		const std::string needle = std::string("/") + segment;
		const size_t at = url.rfind(needle);
		if (at == std::string::npos)
			return false;

		// Must be the end of the pathname: either the end of the URL
		// or immediately followed by a query/parameter separator.
		const size_t after = at + needle.size();
		return after == url.size() || url[after] == '?' || url[after] == '&' || url[after] == ',';
	};

	if (endsWithSegment("h264"))
		return "h264";
	if (endsWithSegment("hevc"))
		return "hevc";
	return std::string();
}

// WithPublishCodec returns url with its publish path ending in /<codec>,
// replacing an existing codec segment or appending one when there is none.
//
// The publish path isn't always the URL's own path: a SRT URL carries it
// inside the streamid parameter ("srt://host:port?streamid=publish:app/name"),
// where the surrounding query syntax has to be respected or the segment ends
// up in the host part instead. The streamid value runs to the next '&', and
// may itself end in ",token=..." or ":query" fields that must stay after the
// codec segment - so the insertion point is the end of the *pathname*, not
// the end of the value.
inline std::string WithPublishCodec(const std::string &url, const std::string &codec)
{
	if (codec.empty())
		return url;

	// Replace an existing segment in place, keeping whatever follows it.
	const std::string existing = PublishCodecFromURL(url);
	if (!existing.empty()) {
		const size_t at = url.rfind("/" + existing);
		return url.substr(0, at) + "/" + codec + url.substr(at + 1 + existing.size());
	}

	// Locate the publish path: the streamid value when there is one,
	// otherwise the URL's own path.
	size_t pathStart, pathEnd;
	const size_t sid = url.find("streamid=");
	if (sid != std::string::npos) {
		pathStart = sid + strlen("streamid=");
		pathEnd = url.find('&', pathStart);
		if (pathEnd == std::string::npos)
			pathEnd = url.size();

		// A streamid may append extra fields after the pathname,
		// separated by ',' (the "#!::" form's key=value list) or ':'
		// (the "action:path:query" form). Stop at whichever comes
		// first so the codec stays part of the pathname - but skip the
		// leading "publish:"/"read:" action, whose ':' is not a field
		// separator.
		size_t scan = pathStart;
		const size_t action = url.find(':', pathStart);
		if (action != std::string::npos && action < pathEnd)
			scan = action + 1;

		const size_t field = url.find_first_of(",:", scan);
		if (field != std::string::npos && field < pathEnd)
			pathEnd = field;
	} else {
		pathStart = url.find("://");
		pathStart = (pathStart == std::string::npos) ? 0 : pathStart + 3;
		pathStart = url.find('/', pathStart);
		if (pathStart == std::string::npos)
			return url; // no path to extend

		pathEnd = url.find_first_of("?#", pathStart);
		if (pathEnd == std::string::npos)
			pathEnd = url.size();
	}

	return url.substr(0, pathEnd) + "/" + codec + url.substr(pathEnd);
}

// StreamTransportIsSingleCodec reports whether a service's transport can
// only carry one video codec per connection. MPEG-TS (SRT/RIST/UDP) muxes
// every video encoder into one program and rejects a mix (see
// obs-ffmpeg-mpegts.c), as does RTMP; WHIP negotiates codecs per session and
// can carry both. Publishing two codecs over a single-codec transport
// therefore needs two connections.
inline bool StreamTransportIsSingleCodec(obs_service_t *service)
{
	const char *protocol = service ? obs_service_get_protocol(service) : nullptr;
	return !protocol || astrcmpi(protocol, "WHIP") != 0;
}

// CheckPublishCodecMatchesURL returns an error message when the configured
// encoders can't produce a publishable stream for this URL, or an empty
// string when the combination is fine.
//
// streamCodec is the main stream encoder's codec ("h264"/"hevc"/...);
// multitrack says whether HEVC/H264 multitrack is enabled, which adds a
// second codec's ladder alongside the first; singleCodecTransport says
// whether the transport can only carry one video codec per connection
// (MPEG-TS over SRT can't - see obs-ffmpeg-mpegts.c's encoder check).
inline std::string CheckPublishCodecMatchesURL(const std::string &url, const char *streamCodec, bool multitrack,
					       bool singleCodecTransport)
{
	const std::string urlCodec = PublishCodecFromURL(url);
	const std::string codec = streamCodec ? streamCodec : "";

	// Multitrack builds an H264 ladder and a HEVC ladder. On a single-codec
	// transport they go out as two connections, each publishing to its own
	// /h264 or /hevc path derived from this URL (see WithPublishCodec), so
	// the configured URL needs no codec segment of its own. One pinned to
	// a codec is still accepted - it just fixes which path the pair is
	// derived from - but pinning to hevc would be misleading, since it's
	// the H264 half that this URL's own connection carries.
	if (multitrack && singleCodecTransport && urlCodec == "hevc") {
		return "The stream URL is pinned to hevc, but with HEVC/H264 multitrack each codec is published "
		       "on its own connection - the /h264 and /hevc paths are derived automatically. Drop the "
		       "/hevc segment from the URL.";
	}

	// The main encoder feeds the H264 ladder whenever multitrack is on
	// (the ladder is pinned to H264 in that case), so its own codec is not
	// what decides the URL match here.
	if (!multitrack && !urlCodec.empty() && codec != urlCodec) {
		return "The stream URL is pinned to " + urlCodec + " but the stream encoder is " +
		       (codec.empty() ? std::string("not set") : codec) + ". Pick a " + urlCodec +
		       " encoder, or change the /" + urlCodec + " segment in the URL.";
	}

	return std::string();
}
