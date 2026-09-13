#pragma once

#include <string>

// Advanced-output encoder settings are persisted per encoder type (e.g.
// "streamEncoder_obs_x264.json" vs "streamEncoder_jim_nvenc.json"), so
// switching the encoder combo brings back that encoder's own last-used
// settings instead of another encoder's - a shared file would otherwise
// let one encoder's values (x264's "preset" strings, say) bleed into a
// property with the same name but different meaning on another encoder
// (NVENC's "preset" list).
//
// Both the settings dialog (which reads and writes these files) and
// AdvancedOutput (which reads them when building the encoders that
// actually stream/record) must agree on the name, or the UI ends up
// editing one file while the output reads another - so this lives here
// rather than in either one of them.
inline std::string EncoderJsonFileName(const char *base, const char *encoderId)
{
	std::string name = base;
	name += '_';
	for (const char *c = encoderId; *c; c++) {
		bool safe = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
			    *c == '_' || *c == '-';
		name += safe ? *c : '_';
	}
	name += ".json";
	return name;
}

// The pre-per-encoder shared file name for an output's encoder settings
// ("streamEncoder.json" / "recordEncoder.json"). Profiles written before
// per-encoder storage existed only have this one, so readers fall back to
// it when the per-encoder file is absent - see LegacyEncoderJsonFileName's
// callers. Writers must never use it: writing there would resurrect the
// very cross-encoder mixing the per-encoder split avoids.
inline std::string LegacyEncoderJsonFileName(const char *base)
{
	return std::string(base) + ".json";
}
