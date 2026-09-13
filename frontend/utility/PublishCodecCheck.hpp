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

	// Multitrack builds an H264 ladder and a HEVC ladder. Both end up on
	// one connection here, which a single-codec transport rejects outright
	// and a codec-pinned path can only half-satisfy - either way the
	// publish can't succeed. Publishing both codecs needs one output per
	// codec path, which this output model doesn't do yet.
	if (multitrack && singleCodecTransport) {
		return "HEVC/H264 multitrack publishes two codecs, but this stream's transport carries only one "
		       "video codec per connection. Turn multitrack off, or publish over WHIP.";
	}

	if (multitrack && !urlCodec.empty()) {
		return "The stream URL is pinned to " + urlCodec +
		       " but HEVC/H264 multitrack is enabled, which publishes both codecs on one connection. "
		       "Turn multitrack off, or remove the /" +
		       urlCodec + " segment from the URL.";
	}

	if (!urlCodec.empty() && codec != urlCodec) {
		return "The stream URL is pinned to " + urlCodec + " but the stream encoder is " +
		       (codec.empty() ? std::string("not set") : codec) + ". Pick a " + urlCodec +
		       " encoder, or change the /" + urlCodec + " segment in the URL.";
	}

	return std::string();
}
