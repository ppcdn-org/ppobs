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
// encoders can't satisfy the codec the URL pins, or an empty string when the
// combination is publishable.
//
// streamCodec is the main stream encoder's codec ("h264"/"hevc"/...);
// multitrack says whether HEVC/H264 multitrack is enabled, which adds a
// second codec's ladder to the same output.
inline std::string CheckPublishCodecMatchesURL(const std::string &url, const char *streamCodec, bool multitrack)
{
	const std::string urlCodec = PublishCodecFromURL(url);
	if (urlCodec.empty())
		return std::string();

	const std::string codec = streamCodec ? streamCodec : "";

	// Multitrack puts both codecs' ladders on one output, which a
	// codec-pinned path rejects whichever way it's pinned: it carries the
	// wrong codec's layers either way, and the layer count doubles.
	// Publishing both codecs needs one output per codec path.
	if (multitrack) {
		return "The stream URL is pinned to " + urlCodec +
		       " but HEVC/H264 multitrack is enabled, which publishes both codecs on one connection. "
		       "Turn multitrack off, or remove the /" +
		       urlCodec + " segment from the URL.";
	}

	if (codec != urlCodec) {
		return "The stream URL is pinned to " + urlCodec + " but the stream encoder is " +
		       (codec.empty() ? std::string("not set") : codec) + ". Pick a " + urlCodec +
		       " encoder, or change the /" + urlCodec + " segment in the URL.";
	}

	return std::string();
}
