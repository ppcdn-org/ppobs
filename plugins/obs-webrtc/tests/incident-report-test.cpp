#include "incident-report.h"
#include "encoder-report.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void Require(bool condition, const char *message)
{
	if (!condition) {
		std::cerr << message << "\n";
		std::exit(1);
	}
}

const char *kSampleCrash =
	"Unhandled exception: c0000005\r\n"
	"Date/Time: 2026-10-22, 8:30:00 AM\r\n"
	"Fault address: 7FFA12341234 (obs-ffmpeg.dll)\r\n"
	"libobs version: 32.0.0 (64-bit)\r\n"
	"Windows version: 10.0 build 26200 (release: 25H2; revision: 1; 64-bit)\r\n"
	"CPU: Test CPU\r\n\r\n"
	"Thread 1A2B: (Crashed)\r\n"
	"Stack            EIP              Arg0             Arg1             Arg2             Arg3             Address\r\n"
	"000000AABBCC0010 00007FFA12341234 0000000000000000 0000000000000001 0000000000000002 0000000000000003 obs-ffmpeg.dll!write_thread+0x34\r\n"
	"000000AABBCC0020 00007FFA12345678 0000000000000000 0000000000000001 0000000000000002 0000000000000003 obs-ffmpeg.dll!0x7ffa12345678\r\n"
	"\r\n"
	"Thread 0F00:\r\n"
	"Stack            EIP              Arg0             Arg1             Arg2             Arg3             Address\r\n"
	"000000AABBCC0030 00007FFA99999999 0000000000000000 0000000000000001 0000000000000002 0000000000000003 other.dll!other_thread+0x1\r\n"
	"\r\nLoaded modules:\r\n";

void WriteFile(const std::filesystem::path &path, const std::string &content)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << content;
}

PostFn MakePost(std::vector<std::string> *bodies, PostOutcome outcome)
{
	return [bodies, outcome](const std::string &body) {
		bodies->push_back(body);
		PostResult r;
		r.outcome = outcome;
		r.note = "test";
		return r;
	};
}
} // namespace

int main()
{
	// URL derivation sits next to DeriveEncoderReportUrl.
	Require(DeriveCrashReportUrl("https://center.example/v1/publish/requests") ==
			"https://center.example/v1/ppobs/crashes",
		"crash URL should derive from the publish URL");
	Require(DeriveReconnectReportUrl("https://center.example/v1/publish/requests") ==
			"https://center.example/v1/ppobs/reconnects",
		"reconnect URL should derive from the publish URL");
	Require(DeriveCrashReportUrl("https://center.example/other").empty(), "no /v1/ means no URL");

	// Reasons are a closed set.
	Require(std::string(ReconnectReasonName(ReconnectReason::PpmmxReconnectCommand)) == "ppmmx_reconnect_command",
		"reason name");
	Require(ReconnectReasonFromName("high_packet_loss") == ReconnectReason::HighPacketLoss, "reason parse");
	Require(ReconnectReasonFromName("garbage") == ReconnectReason::Unknown, "unknown reason");

	// Reconnect body.
	IncidentTarget target;
	target.publish_url = "https://center.example/v1/publish/requests";
	target.app_id = "app";
	target.app_secret = "secret";
	target.stream_name = "live";
	target.device_id = "dev-1";
	target.protocol = "srt";
	ReconnectEvent ev;
	ev.reason = ReconnectReason::HighPacketLoss;
	ev.detail = std::string(600, 'x');
	ev.codec = "hevc";
	ev.loss_pct = 3.4;
	ev.bitrate_kbps = 6000;
	ev.latency_ms = 120;
	auto rc = nlohmann::json::parse(BuildReconnectReportJson(target, ev, "2026-10-22T08:30:00Z", "32.0.0"));
	Require(rc.value("reason", "") == "high_packet_loss", "reconnect reason");
	Require(rc.value("detail", std::string()).size() == 512, "detail truncated to 512");
	Require(rc.value("protocol", "") == "srt" && rc.value("codec", "") == "hevc", "protocol/codec");
	Require(rc.value("lossPct", 0.0) == 3.4 && rc.value("bitrateKbps", 0) == 6000 && rc.value("latencyMs", 0) == 120,
		"snapshots");
	Require(!rc.contains("reconnectMs") && !rc.contains("success"), "unset optionals are omitted");
	Require(rc.value("deviceId", "") == "dev-1" && rc.value("appSecret", "") == "secret", "auth fields");

	// Dedup: second report for the same stream inside 3s is dropped.
	ReconnectDedup dedup;
	Require(dedup.ShouldReport("app/live", ReconnectReason::HighPacketLoss, 1000), "first report passes");
	Require(!dedup.ShouldReport("app/live", ReconnectReason::SrtConnectionLost, 2500), "dup within 3s dropped");
	Require(dedup.ShouldReport("app/other", ReconnectReason::SrtConnectionLost, 2500), "other stream passes");
	Require(dedup.ShouldReport("app/live", ReconnectReason::SrtConnectionLost, 4500), "after window passes");
	Require(dedup.ShouldReport("app/live", ReconnectReason::PpcenterTokenRefreshFailed, 10000), "token first");
	Require(!dedup.ShouldReport("app/live", ReconnectReason::PpcenterTokenRefreshFailed, 30000),
		"token refresh failures throttled hard");
	Require(dedup.ShouldReport("app/live", ReconnectReason::PpcenterTokenRefreshFailed, 71000),
		"token failure allowed again after a minute");

	// Stream name from URLs.
	Require(DeriveStreamNameFromServerUrl("srt://h:1234?streamid=publish:app/B01-frontView&latency=200", "app") ==
			"B01-frontView",
		"srt stream name");
	Require(DeriveStreamNameFromServerUrl("srt://h:1234?streamid=publish%3Aapp%2Fcam1", "app") == "cam1",
		"percent-encoded srt stream name");
	Require(DeriveStreamNameFromServerUrl("rtmp://h/app/stream1", "app") == "stream1", "rtmp stream name");
	Require(DeriveStreamNameFromServerUrl("garbage", "app").empty(), "no stream name");

	// Crash log parsing.
	const ParsedCrashLog parsed = ParseCrashLogText(kSampleCrash);
	Require(parsed.exc_code == "0xC0000005", "exception code");
	Require(parsed.exception_name == "ACCESS_VIOLATION", "exception name");
	Require(parsed.fault_module == "obs-ffmpeg.dll", "fault module");
	Require(parsed.os == "Windows 11 10.0.26200 x64", "os");
	Require(parsed.top_frame == "obs-ffmpeg.dll!write_thread+0x34", "top frame");
	Require(parsed.stack.find("other.dll") == std::string::npos, "only the crashed thread is in the stack");
	Require(parsed.stack.find("obs-ffmpeg.dll!0x7ffa12345678") != std::string::npos, "second frame kept");
	Require(parsed.summary ==
			"Unhandled exception: ACCESS_VIOLATION (0xC0000005) at obs-ffmpeg.dll!write_thread+0x34",
		"summary");

	// A system-DLL top frame is followed by the first symbolized frame.
	const ParsedCrashLog sys_top = ParseCrashLogText(
		"Unhandled exception: c0000005\r\n"
		"Fault address: 1 (c:/windows/system32/msvcp140.dll)\r\n\r\n"
		"Thread 1: worker (Crashed)\r\n"
		"Stack EIP Arg0 Arg1 Arg2 Arg3 Address\r\n"
		"0000000000000001 0000000000000002 0 0 0 0 msvcp140.dll!0x7ffe1\r\n"
		"0000000000000001 0000000000000002 0 0 0 0 obs-webrtc.dll!Foo::Bar+0x47\r\n");
	Require(sys_top.summary == "Unhandled exception: ACCESS_VIOLATION (0xC0000005) at msvcp140.dll!0x7ffe1 via "
				   "obs-webrtc.dll!Foo::Bar+0x47",
		"summary points at the first symbolized frame");

	// Malformed logs never throw and still give a summary.
	Require(!ParseCrashLogText("").summary.empty(), "empty log summary");
	Require(!ParseCrashLogText(std::string("\xff\xfe\x00\x01 junk\n\x80", 12)).summary.empty(), "binary log summary");
	Require(ParseCrashLogText(std::string(100000, 'a')).stack.size() <= 8000, "stack capped");

	// File-name time + fingerprint stability.
	int64_t t = 0;
	Require(ParseCrashFileNameTime("Crash 2026-10-22 08-30-00.txt", &t) && t > 0, "file name parses");
	Require(!ParseCrashFileNameTime("Crash garbage.txt", &t), "bad file name rejected");
	const std::string fp1 = CrashFingerprint("dev-1", "Crash 2026-10-22 08-30-00.txt", t);
	Require(fp1.size() == 32, "fingerprint is 32 hex chars");
	Require(fp1 == CrashFingerprint("dev-1", "Crash 2026-10-22 08-30-00.txt", t), "fingerprint stable");
	Require(fp1 != CrashFingerprint("dev-2", "Crash 2026-10-22 08-30-00.txt", t), "fingerprint depends on device");
	Require(FormatRfc3339Utc(0) == "1970-01-01T00:00:00Z", "rfc3339");

	// Scan + report-once state machine on a temp dir.
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "ppobs-incident-test-crashes";
	fs::remove_all(dir);
	fs::create_directories(dir);
	WriteFile(dir / "Crash 2026-10-22 08-30-00.txt", kSampleCrash);
	WriteFile(dir / "Crash 2026-10-21 08-30-00.txt", "Unhandled exception: c00000fd\r\n");
	WriteFile(dir / "Crash 2020-01-01 00-00-00.txt", "too old\r\n");
	WriteFile(dir / "notes.txt", "not a crash\r\n");

	CrashScanOptions options;
	options.crash_dir = dir.string();
	options.now_unix = t + 86400; // one day after the newest crash
	int device_calls = 0;
	std::vector<std::string> bodies;
	CrashScanContext context;
	context.app_id = "app";
	context.app_secret = "secret";
	context.client_version = "32.0.0";
	context.device_id = [&]() {
		device_calls++;
		return std::string("dev-1");
	};

	// Server down: nothing marked done, scan stops after the first failure.
	context.post = MakePost(&bodies, PostOutcome::Retry);
	Require(ProcessCrashLogs(options, context) == 0, "nothing sent while ppcenter fails");
	Require(bodies.size() == 1, "stops at first retryable failure");

	// Server up: both recent crashes are sent (old one skipped), newest first.
	bodies.clear();
	context.post = MakePost(&bodies, PostOutcome::Ok);
	Require(ProcessCrashLogs(options, context) == 2, "both recent crashes sent");
	Require(bodies.size() == 2, "two posts");
	auto first = nlohmann::json::parse(bodies[0]);
	Require(first.value("exceptionCode", "") == "0xC0000005", "newest crash first");
	Require(first.value("fingerprint", "").size() == 32, "fingerprint in body");
	Require(first.value("deviceId", "") == "dev-1" && first.value("appId", "") == "app", "ids in body");
	Require(first.value("crashedAt", "").size() == 20, "crashedAt RFC3339");

	// Second start: reported crashes are never re-sent.
	bodies.clear();
	Require(ProcessCrashLogs(options, context) == 0 && bodies.empty(), "no re-send after success");

	// Attempt cap: a crash that keeps failing is given up after max_attempts.
	WriteFile(dir / "Crash 2026-10-22 09-00-00.txt", "Unhandled exception: c0000005\r\n");
	options.max_attempts = 2;
	context.post = MakePost(&bodies, PostOutcome::Retry);
	bodies.clear();
	ProcessCrashLogs(options, context);
	ProcessCrashLogs(options, context);
	Require(bodies.size() == 2, "two attempts made");
	ProcessCrashLogs(options, context);
	Require(bodies.size() == 2, "no third attempt after giving up");

	// 400 is permanent.
	WriteFile(dir / "Crash 2026-10-22 10-00-00.txt", "Unhandled exception: c0000005\r\n");
	bodies.clear();
	context.post = MakePost(&bodies, PostOutcome::Rejected);
	ProcessCrashLogs(options, context);
	ProcessCrashLogs(options, context);
	Require(bodies.size() == 1, "rejected crash is not retried");

	// Last publish target is attached only when the publish was live before the crash.
	fs::remove(dir / kCrashReportedStateFile);
	for (const auto &e : fs::directory_iterator(dir)) {
		if (e.path().filename().string().rfind("Crash ", 0) == 0)
			fs::remove(e.path());
	}
	WriteFile(dir / "Crash 2026-10-22 08-30-00.txt", kSampleCrash);
	RecordLastPublish(dir.string(), "app", "live", "srt", true, t - 600);
	bodies.clear();
	context.post = MakePost(&bodies, PostOutcome::Ok);
	ProcessCrashLogs(options, context);
	Require(bodies.size() == 1, "one crash");
	auto withTarget = nlohmann::json::parse(bodies[0]);
	Require(withTarget.value("lastStream", "") == "live" && withTarget.value("lastProtocol", "") == "srt",
		"last publish attached");

	fs::remove_all(dir);
	std::cout << "ok\n";
	return 0;
}
