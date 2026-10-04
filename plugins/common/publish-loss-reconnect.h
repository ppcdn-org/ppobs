#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <obs-frontend-api.h>
#include <util/config-file.h>

/*
 * Shared publish loss-reconnect policy for the SRT (obs-ffmpeg) and WHIP
 * (obs-webrtc) transports. Both feed per-interval (lost, expected) deltas
 * here whenever the configured sample period elapses; the monitor keeps a
 * time window of samples and reports when the window's aggregate loss rate
 * crosses the configured threshold.
 */

#define PUBLISH_LOSS_SETTINGS_SECTION "General"
#define PUBLISH_LOSS_SETTINGS_ENABLE "LossReconnectEnable"
#define PUBLISH_LOSS_SETTINGS_PERIOD "LossReconnectPeriodSec"
#define PUBLISH_LOSS_SETTINGS_WINDOW "LossReconnectWindowSec"
#define PUBLISH_LOSS_SETTINGS_THRESHOLD "LossReconnectThresholdPct"

/* Samples whose own loss rate exceeds this are written to the OBS log. */
#define PUBLISH_LOSS_LOG_THRESHOLD_PCT 0.5

#define PUBLISH_LOSS_MAX_SAMPLES 1024

struct publish_loss_config {
	bool enable;
	int period_sec;
	int window_sec;
	double threshold_pct;
};

struct publish_loss_monitor {
	bool enable;
	int64_t period_ms;
	int64_t window_ms;
	double threshold_pct;
	int64_t next_sample_ms;
	uint64_t acc_lost;
	uint64_t acc_expected;
	int head;
	int count;
	int64_t sample_ms[PUBLISH_LOSS_MAX_SAMPLES];
	uint64_t sample_lost[PUBLISH_LOSS_MAX_SAMPLES];
	uint64_t sample_expected[PUBLISH_LOSS_MAX_SAMPLES];
};

static inline void publish_loss_config_load(struct publish_loss_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->enable = true;
	cfg->period_sec = 10;
	cfg->window_sec = 60;
	cfg->threshold_pct = 1.2;

	config_t *conf = obs_frontend_get_app_config();
	if (!conf)
		return;

	if (config_has_user_value(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_ENABLE))
		cfg->enable = config_get_bool(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_ENABLE);
	if (config_has_user_value(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_PERIOD))
		cfg->period_sec = (int)config_get_int(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_PERIOD);
	if (config_has_user_value(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_WINDOW))
		cfg->window_sec = (int)config_get_int(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_WINDOW);
	if (config_has_user_value(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_THRESHOLD))
		cfg->threshold_pct = config_get_double(conf, PUBLISH_LOSS_SETTINGS_SECTION, PUBLISH_LOSS_SETTINGS_THRESHOLD);

	if (cfg->period_sec < 1)
		cfg->period_sec = 1;
	if (cfg->window_sec < cfg->period_sec)
		cfg->window_sec = cfg->period_sec;
	if (cfg->threshold_pct <= 0.0)
		cfg->threshold_pct = 1.2;
}

static inline void publish_loss_monitor_init(struct publish_loss_monitor *m, const struct publish_loss_config *cfg)
{
	memset(m, 0, sizeof(*m));
	m->enable = cfg->enable;
	m->period_ms = (int64_t)cfg->period_sec * 1000;
	m->window_ms = (int64_t)cfg->window_sec * 1000;
	m->threshold_pct = cfg->threshold_pct;
}

static inline void publish_loss_monitor_reset(struct publish_loss_monitor *m)
{
	m->next_sample_ms = 0;
	m->acc_lost = 0;
	m->acc_expected = 0;
	m->head = 0;
	m->count = 0;
}

/*
 * Feed the per-interval (lost, expected) counters since the previous call.
 * Callers pass deltas, not cumulative totals. Returns true when the window's
 * aggregate loss rate exceeds the threshold, meaning the caller should force
 * a reconnect. When a full sample period has elapsed, *have_sample is set and
 * *sample_pct carries that sample's own loss rate (for logging).
 */
static inline bool publish_loss_monitor_record(struct publish_loss_monitor *m, uint64_t lost, uint64_t expected,
					       int64_t now_ms, bool *have_sample, double *sample_pct)
{
	if (have_sample)
		*have_sample = false;
	if (sample_pct)
		*sample_pct = 0.0;
	if (!m->enable)
		return false;

	m->acc_lost += lost;
	m->acc_expected += expected;

	if (m->next_sample_ms == 0) {
		m->next_sample_ms = now_ms + m->period_ms;
		return false;
	}
	if (now_ms < m->next_sample_ms)
		return false;

	m->next_sample_ms = now_ms + m->period_ms;

	m->sample_ms[m->head] = now_ms;
	m->sample_lost[m->head] = m->acc_lost;
	m->sample_expected[m->head] = m->acc_expected;
	m->head = (m->head + 1) % PUBLISH_LOSS_MAX_SAMPLES;
	if (m->count < PUBLISH_LOSS_MAX_SAMPLES)
		m->count++;

	uint64_t period_lost = m->acc_lost;
	uint64_t period_expected = m->acc_expected;
	m->acc_lost = 0;
	m->acc_expected = 0;

	if (have_sample)
		*have_sample = true;
	if (sample_pct && period_expected)
		*sample_pct = (double)period_lost * 100.0 / (double)period_expected;

	int64_t cutoff = now_ms - m->window_ms;
	while (m->count > 0) {
		int oldest = (m->head - m->count + PUBLISH_LOSS_MAX_SAMPLES) % PUBLISH_LOSS_MAX_SAMPLES;
		if (m->sample_ms[oldest] >= cutoff)
			break;
		m->count--;
	}

	uint64_t win_lost = 0;
	uint64_t win_expected = 0;
	for (int i = 0; i < m->count; i++) {
		int idx = (m->head - 1 - i + PUBLISH_LOSS_MAX_SAMPLES * 2) % PUBLISH_LOSS_MAX_SAMPLES;
		win_lost += m->sample_lost[idx];
		win_expected += m->sample_expected[idx];
	}

	if (win_expected == 0)
		return false;

	double win_pct = (double)win_lost * 100.0 / (double)win_expected;
	if (win_pct > m->threshold_pct) {
		publish_loss_monitor_reset(m);
		return true;
	}
	return false;
}
