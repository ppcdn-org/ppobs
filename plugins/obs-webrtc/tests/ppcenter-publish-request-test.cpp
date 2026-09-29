#include "ppcenter-client.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>

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
	PPCenterPublishRequest request{"https://center.example/v1/publish/requests", "app", "secret", "live", "auto"};
	request.device_id = "device-1";

	auto withoutProbe = nlohmann::json::parse(ppcenter_build_publish_json(request));
	Require(withoutProbe.value("deviceId", "") == "device-1", "deviceId should be included");
	Require(!withoutProbe.contains("natProbeId"), "empty natProbeId should be omitted");
	Require(withoutProbe["capabilities"] == nlohmann::json::array({"whip-hevc-h264"}),
		"capabilities should always request whip-hevc-h264");

	request.nat_probe_id = "probe-1";
	auto withProbe = nlohmann::json::parse(ppcenter_build_publish_json(request));
	Require(withProbe.value("natProbeId", "") == "probe-1", "successful probeId should be included");

	// ppcenter_publish_token_expiry_unix reads the "exp" claim of ppcenter's
	// "<base64url(JSON)>.<base64url(HMAC)>" publisher signal token without the
	// signing key - WHIPOutput uses it to know when a long stream must refresh
	// the signal token before it stops being accepted.
	Require(ppcenter_publish_token_expiry_unix(
			"eyJ2IjoxLCJyb2xlIjoicHVibGlzaGVyIiwicGFydGljaXBhbnRJZCI6InB1Ymxpc2hlci1hYmMiLCJzdHJlYW1QYXRoIjoiYXBwL2xpdmUiLCJleHAiOjE3MDAwMDAwMDB9.sig") ==
			1700000000,
		"exp should be decoded from a real-shaped token payload");
	Require(ppcenter_publish_token_expiry_unix("eyJ2IjoxLCJyb2xlIjoicHVibGlzaGVyIn0.sig") == 0,
		"a token without an exp claim should decode to 0");
	Require(ppcenter_publish_token_expiry_unix("eyJleHAiOiJzb29uIn0.sig") == 0,
		"a non-numeric exp claim should decode to 0");
	Require(ppcenter_publish_token_expiry_unix("") == 0, "an empty token should decode to 0");
	Require(ppcenter_publish_token_expiry_unix("no-separator") == 0,
		"a token without a payload separator should decode to 0");
	Require(ppcenter_publish_token_expiry_unix("!!!.sig") == 0,
		"an un-decodable payload should decode to 0");
	return 0;
}
