#pragma once

#include <cstdint>
#include <vector>
#include <string>

struct UplinkQosSample {
	double rttMs = 0;
	int64_t nowMs = 0;
};

struct UplinkQosConfig {
	double maxRttMs = 300;
	int maxPeersToDropAtOnce = 1;
	int64_t cooldownMs = 10000;
};

struct UplinkQosDecision {
	bool congested = false;
	std::vector<std::string> sessionsToClose;
	std::string reason;
	bool cooldownActive = false;
};

class UplinkQosPolicy {
public:
	explicit UplinkQosPolicy(const UplinkQosConfig &config);

	UplinkQosDecision Evaluate(const UplinkQosSample &sample,
				   const std::vector<std::string> &p2pSessionIdsNewestFirst);

	void RecordDrop(int count);
private:
	UplinkQosConfig config;
	int64_t lastDropMs = 0;
	int dropCount = 0;
};
