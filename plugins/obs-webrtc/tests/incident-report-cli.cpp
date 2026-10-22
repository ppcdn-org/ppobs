// Manual end-to-end driver for the crash-report scan: runs the exact same
// ProcessCrashLogs + HTTP POST path ppobs uses at startup, without OBS.
//
//   obs-webrtc-incident-report-cli <crash_dir> <ppcenter_publish_url> <appId> <appSecret> <deviceId>
//
// e.g. against a local mock server:
//   ... "D:/tmp/crashes" "http://127.0.0.1:18080/v1/publish/requests" app secret dev-1
// Run it twice: the first run reports, the second must report nothing
// (state in <crash_dir>/.ppcenter-reported.json).
#include "incident-report.h"
#include "encoder-report.h"

#include <obs.h>

#include <ctime>
#include <iostream>

int main(int argc, char **argv)
{
	if (argc < 6) {
		std::cerr << "usage: " << argv[0] << " <crash_dir> <publish_url> <appId> <appSecret> <deviceId>\n";
		return 2;
	}
	const std::string url = DeriveCrashReportUrl(argv[2]);
	if (url.empty()) {
		std::cerr << "publish_url must contain /v1/\n";
		return 2;
	}

	// The HTTP user-agent helper reads libobs' locale/version, so libobs must be up.
	if (!obs_startup("en-US", nullptr, nullptr)) {
		std::cerr << "obs_startup failed\n";
		return 3;
	}

	CrashScanOptions options;
	options.crash_dir = argv[1];
	options.now_unix = static_cast<int64_t>(std::time(nullptr));

	CrashScanContext context;
	context.app_id = argv[3];
	context.app_secret = argv[4];
	context.client_version = "cli-test";
	const std::string device = argv[5];
	context.device_id = [device]() { return device; };
	context.post = [url](const std::string &body) {
		return ClassifyCrashPost(PostJsonBlocking(url, body, 5, 10));
	};

	const size_t sent = ProcessCrashLogs(options, context);
	std::cout << "sent=" << sent << "\n";
	obs_shutdown();
	return 0;
}
