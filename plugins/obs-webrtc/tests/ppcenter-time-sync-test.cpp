#include "ppcenter-time-sync-utils.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void Require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << "\n";
		std::exit(1);
	}
}
} // namespace

int main()
{
	// Origin derivation must accept both the full publish URL (the historical
	// config value) and the bare origin (the new one), keeping any port.
	Require(DerivePpcenterOrigin("https://api.pp-cdn.org/v1/publish/requests") == "https://api.pp-cdn.org",
		"origin from full publish URL");
	Require(DerivePpcenterOrigin("https://api.pp-cdn.org") == "https://api.pp-cdn.org",
		"origin from bare origin");
	Require(DerivePpcenterOrigin("https://api.pp-cdn.org/") == "https://api.pp-cdn.org",
		"origin drops trailing slash");
	Require(DerivePpcenterOrigin("http://127.0.0.1:18000/v1/publish/requests") == "http://127.0.0.1:18000",
		"origin keeps port");
	Require(DerivePpcenterOrigin("ftp://host/x").empty(), "non-http scheme rejected");
	Require(DerivePpcenterOrigin("api.pp-cdn.org/v1").empty(), "missing scheme rejected");

	// The publish URL is rebuilt from the origin regardless of the input form.
	Require(DerivePpcenterPublishUrl("https://api.pp-cdn.org") == "https://api.pp-cdn.org/v1/publish/requests",
		"publish URL from bare origin");
	Require(DerivePpcenterPublishUrl("https://api.pp-cdn.org/v1/publish/requests") ==
			"https://api.pp-cdn.org/v1/publish/requests",
		"publish URL from full URL");

	// Time-sync endpoint: https->wss, http->ws, fixed path /ws/play.
	Require(DerivePpcenterTimeSyncUrl("https://api.pp-cdn.org/v1/publish/requests") == "wss://api.pp-cdn.org/ws/play",
		"wss from https");
	Require(DerivePpcenterTimeSyncUrl("http://127.0.0.1:18000/v1/publish/requests") == "ws://127.0.0.1:18000/ws/play",
		"ws from http");
	Require(DerivePpcenterTimeSyncUrl("https://api.pp-cdn.org") == "wss://api.pp-cdn.org/ws/play",
		"time-sync URL from bare origin");

	// Four-timestamp offset ((t2-t1)+(t3-t4))/2. Example: ppcenter 500ms ahead,
	// symmetric 20ms RTT (t1=0, t2=510, t3=520, t4=40) -> 495ms.
	Require(ComputePpcenterClockOffsetMs(0, 510, 520, 40) == 495, "offset with skew");
	Require(ComputePpcenterClockOffsetMs(1000, 1010, 1020, 1030) == 0, "offset with no skew");
	Require(ComputePpcenterClockOffsetMs(0, 0, 0, 0) == 0, "offset zero");
	Require(ComputePpcenterClockOffsetMs(5000, 5000, 5000, 5000) == 0, "offset zero equal stamps");

	// Flat JSON integer extraction from a TIME_SYNC_ACK body.
	int64_t v = 0;
	Require(ExtractJsonIntField("{\"type\":\"TIME_SYNC_ACK\",\"t1\":123,\"t2\":456,\"t3\":789}", "t2", &v) &&
			v == 456,
		"json t2");
	Require(ExtractJsonIntField("{\"t1\":-5}", "t1", &v) && v == -5, "json negative");
	Require(ExtractJsonIntField("{\"t1\": 42 ,\"t2\":1}", "t1", &v) && v == 42, "json whitespace");
	Require(!ExtractJsonIntField("{\"t1\":\"x\"}", "t1", &v), "json non-integer rejected");
	Require(!ExtractJsonIntField("{\"t2\":1}", "t1", &v), "json missing rejected");

	return 0;
}
