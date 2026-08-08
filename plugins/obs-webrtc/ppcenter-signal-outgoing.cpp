#include "ppcenter-signal-outgoing.h"

#include <obs.h>

std::string BuildP2PAnswerJSON(const std::string &sessionId, const std::string &sdp)
{
	obs_data_t *msg = obs_data_create();
	obs_data_set_int(msg, "v", 1);
	obs_data_set_string(msg, "type", "answer");
	obs_data_set_string(msg, "sessionId", sessionId.c_str());
	obs_data_set_string(msg, "sdp", sdp.c_str());
	std::string json = obs_data_get_json(msg);
	obs_data_release(msg);
	return json;
}

std::string BuildP2PIceJSON(const std::string &sessionId, const std::string &candidate,
			    const std::string &sdpMid, int sdpMLineIndex)
{
	obs_data_t *msg = obs_data_create();
	obs_data_t *candidateObj = obs_data_create();
	obs_data_set_int(msg, "v", 1);
	obs_data_set_string(msg, "type", "ice");
	obs_data_set_string(msg, "sessionId", sessionId.c_str());
	obs_data_set_string(candidateObj, "candidate", candidate.c_str());
	obs_data_set_string(candidateObj, "sdpMid", sdpMid.c_str());
	obs_data_set_int(candidateObj, "sdpMLineIndex", sdpMLineIndex);
	obs_data_set_obj(msg, "candidate", candidateObj);
	std::string json = obs_data_get_json(msg);
	obs_data_release(candidateObj);
	obs_data_release(msg);
	return json;
}

std::string BuildP2PConnectedJSON(const std::string &sessionId)
{
	obs_data_t *msg = obs_data_create();
	obs_data_set_int(msg, "v", 1);
	obs_data_set_string(msg, "type", "connected");
	obs_data_set_string(msg, "sessionId", sessionId.c_str());
	std::string json = obs_data_get_json(msg);
	obs_data_release(msg);
	return json;
}

std::string BuildP2PCloseJSON(const std::string &sessionId, const std::string &reason)
{
	obs_data_t *msg = obs_data_create();
	obs_data_set_int(msg, "v", 1);
	obs_data_set_string(msg, "type", "close");
	obs_data_set_string(msg, "sessionId", sessionId.c_str());
	obs_data_set_string(msg, "reason", reason.c_str());
	std::string json = obs_data_get_json(msg);
	obs_data_release(msg);
	return json;
}
