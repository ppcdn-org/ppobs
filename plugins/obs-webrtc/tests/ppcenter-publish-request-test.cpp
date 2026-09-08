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
	return 0;
}
