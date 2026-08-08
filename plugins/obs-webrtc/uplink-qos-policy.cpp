#include "uplink-qos-policy.h"
#include <algorithm>

UplinkQosPolicy::UplinkQosPolicy(const UplinkQosConfig &config) : config(config) {}

UplinkQosDecision UplinkQosPolicy::Evaluate(const UplinkQosSample &sample,
					    const std::vector<std::string> &p2pSessionIdsNewestFirst)
{
	UplinkQosDecision decision;

	if (config.maxRttMs <= 0 || sample.nowMs <= 0 || p2pSessionIdsNewestFirst.empty()) {
		return decision;
	}

	if (sample.rttMs < config.maxRttMs) {
		return decision;
	}

	int64_t elapsed = sample.nowMs - lastDropMs;
	if (lastDropMs > 0 && elapsed < config.cooldownMs) {
		decision.cooldownActive = true;
		return decision;
	}

	decision.congested = true;
	int toDrop = config.maxPeersToDropAtOnce;
	if (toDrop <= 0 || toDrop > (int)p2pSessionIdsNewestFirst.size()) {
		toDrop = (int)p2pSessionIdsNewestFirst.size();
	}

	for (int i = 0; i < toDrop; i++) {
		decision.sessionsToClose.push_back(p2pSessionIdsNewestFirst[i]);
	}

	decision.reason = "publisher_uplink_congestion";
	lastDropMs = sample.nowMs;
	dropCount += toDrop;

	return decision;
}

void UplinkQosPolicy::RecordDrop(int count)
{
	dropCount += count;
}
