#include "ppcenter-signal-message.h"

#include <obs.h>

P2PSignalMessage ParseP2PSignalMessage(const std::string &json)
{
	P2PSignalMessage parsed;
	obs_data_t *msg = obs_data_create_from_json(json.c_str());
	if (!msg)
		return parsed;

	const char *type = obs_data_get_string(msg, "type");
	const char *sessionId = obs_data_get_string(msg, "sessionId");
	if (type) parsed.type = type;
	if (sessionId) parsed.sessionId = sessionId;
	const char *sdp = obs_data_get_string(msg, "sdp");
	const char *reason = obs_data_get_string(msg, "reason");
	if (sdp) parsed.sdp = sdp;
	if (reason) parsed.reason = reason;

	obs_data_t *candidateObj = obs_data_get_obj(msg, "candidate");
	if (candidateObj) {
		const char *candidate = obs_data_get_string(candidateObj, "candidate");
		const char *sdpMid = obs_data_get_string(candidateObj, "sdpMid");
		if (candidate) parsed.candidate = candidate;
		if (sdpMid) parsed.sdpMid = sdpMid;
		obs_data_release(candidateObj);
	}

	parsed.valid = !parsed.type.empty() && !parsed.sessionId.empty();
	obs_data_release(msg);
	return parsed;
}
