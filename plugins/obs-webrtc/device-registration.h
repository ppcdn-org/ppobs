#pragma once

#include <string>

// Collects this machine's hardware identity (disk serial or LAN MAC) and
// registers it with ppcenter via POST /v1/ppobs-devices/register. The
// returned deviceId is a stable hash of the hardware info and is cached
// in memory for the lifetime of the process.
//
// Called before every publish attempt so that ppcenter can enforce the
// per-user device binding limit and reject publishes from unregistered
// devices.
//
// Returns the deviceId on success, or an empty string on failure.
// Registration failure is non-fatal: the caller may still attempt to
// publish (the publish handler itself enforces the binding requirement).
std::string RegisterPpobsDevice(const std::string &ppcenterUrl,
				const std::string &appId,
				const std::string &appSecret);
