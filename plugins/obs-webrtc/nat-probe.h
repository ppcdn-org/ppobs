#pragma once

#include <string>

// Publisher-side NAT observation, submitted to ppcenter's POST /v1/nat/probe
// before every WHIP publish attempt - see
// docs/design/ppobs-p2p-nat-probe-gap.zh-CN.md R1. Without this, ppcenter's
// P2P admission gate (Coordinator.AllocateEligible) can never find a fresh
// publisher observation and always rejects P2P for streams published by
// ppobs, regardless of the player side's own probe.
struct NATProbeResult {
	std::string probe_id;
	bool succeeded = false;
};

// Stable per-installation identifier reused across probes (and across
// restarts) so repeated probes from the same machine land on the same
// ppcenter-side probeId/observation slot instead of piling up one-off
// records - see the design doc's "注意" in R1 §2.3. Persisted under this
// module's config directory; generated once on first use.
std::string GetOrCreateP2PClientId();

// Gathers local ICE candidates via a short-lived, track-less
// rtc::PeerConnection (mirrors ppplayer's nat-probe.mjs: an empty
// DataChannel is enough to trigger ICE gathering) and submits the best
// candidate found to ppcenter's POST /v1/nat/probe. `ppcenterUrl` is the
// same base URL already configured for /v1/publish/requests. Returns a
// result with `succeeded == false` (and an empty probe_id) on any failure
// or timeout - by design this must never block or abort the publish
// attempt itself (see R1 §2.3 "不需要因为探测失败就中止推流").
//
// `clientId` must be the exact same value passed as `deviceId` to the
// subsequent /v1/publish/requests call. ppcenter's eligibility check
// (p2p.EvaluateDirectEligibility) compares the NAT observation's
// ParticipantID (= whatever this probe submits as `clientId`) against the
// publisher's claims.ObservationID (= /v1/publish/requests's `deviceId`,
// see IssuePublisherWithProbe's call site in ppcenter/internal/apis
// publish_v1.go) and rejects the pair as identity_mismatch on any
// difference. Before 2026-09-22 this function generated its own identifier
// via GetOrCreateP2PClientId() independently of whatever deviceId the
// caller ended up using for publish - the two essentially never matched,
// so P2P eligibility failed shut on identity_mismatch for every ppobs
// publisher, unconditionally, even once every other precondition (fresh
// probes on both sides, matching stream path, compatible NAT types) was
// satisfied. See docs/test/ppcdn-debug-log.md's 2026-09-22 entry.
NATProbeResult ProbePublisherNAT(const std::string &ppcenterUrl, const std::string &appId,
				 const std::string &appSecret, const std::string &streamName,
				 const std::string &clientId);
