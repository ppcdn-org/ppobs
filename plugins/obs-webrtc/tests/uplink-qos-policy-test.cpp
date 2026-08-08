#include "uplink-qos-policy.h"

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
}

int main()
{
	UplinkQosConfig config;
	config.maxRttMs = 300;
	config.maxPeersToDropAtOnce = 1;
	config.cooldownMs = 10000;
	UplinkQosPolicy policy(config);

	auto normal = policy.Evaluate({299, 1000}, {"newest", "older"});
	Require(!normal.congested, "RTT below threshold should not be congested");
	Require(normal.sessionsToClose.empty(), "RTT below threshold should not drop sessions");

	auto congested = policy.Evaluate({300, 2000}, {"newest", "older"});
	Require(congested.congested, "RTT at threshold should be congested");
	Require(congested.sessionsToClose.size() == 1, "should drop one peer");
	Require(congested.sessionsToClose[0] == "newest", "should drop newest peer first");
	Require(congested.reason == "publisher_uplink_congestion", "reason mismatch");

	auto cooldown = policy.Evaluate({400, 5000}, {"another"});
	Require(!cooldown.congested, "cooldown should suppress repeated drop");
	Require(cooldown.cooldownActive, "cooldown should be active");
	Require(cooldown.sessionsToClose.empty(), "cooldown should not drop sessions");

	auto afterCooldown = policy.Evaluate({400, 13000}, {"next", "older"});
	Require(afterCooldown.congested, "drop should resume after cooldown");
	Require(afterCooldown.sessionsToClose.size() == 1 && afterCooldown.sessionsToClose[0] == "next", "post-cooldown drop mismatch");

	UplinkQosConfig dropAllConfig;
	dropAllConfig.maxRttMs = 300;
	dropAllConfig.maxPeersToDropAtOnce = 0;
	dropAllConfig.cooldownMs = 10000;
	UplinkQosPolicy dropAll(dropAllConfig);
	auto all = dropAll.Evaluate({500, 1000}, {"a", "b", "c"});
	Require(all.sessionsToClose.size() == 3, "non-positive maxPeersToDropAtOnce should drop all peers");
	return 0;
}
