#pragma once

#include <algorithm>
#include <cstdint>

// PPCDN's fixed video-bitrate ladder: the output resolution decides the
// target bitrate, so users cannot accidentally pair 4K with a 1080p bitrate
// (or exceed the 10 Mbps publish cap). Returns 0 for resolutions outside the
// ladder, leaving the user's own value untouched.
inline int PPCDNVideoBitrateForResolution(uint32_t cx, uint32_t cy)
{
	const uint32_t shortSide = std::min(cx, cy);
	if (shortSide >= 2160)
		return 10000;
	if (shortSide >= 1440)
		return 5000;
	if (shortSide >= 1080)
		return 3000;
	if (shortSide >= 720)
		return 1000;
	return 0;
}
