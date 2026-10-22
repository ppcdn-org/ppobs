#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <obs-frontend-api.h>
#include <util/config-file.h>

/*
 * Shared publish loss-reconnect policy for the SRT (obs-ffmpeg) and WHIP
 * (obs-webrtc) transports. Both feed per-interval (lost, retransmitted,
 * sent) deltas here whenever the configured sample period elapses; the
 * monitor aggregates them per period and reports when the window's aggregate
 * unrecoverable-loss rate crosses the configured threshold.
 *
 * Two things matter for correctness:
 *
 *  - Unrecoverable loss is computed once per *period*, not per raw sample.
 *    Retransmissions lag their losses by ~one RTT, so subtracting them inside
 *    each 1s sample and clamping negatives to zero systematically over-counts
 *    whenever the retransmission lands just after a sample boundary (the
 *    compensating negative is thrown away). Aggregating lost/retransmitted
 *    over the whole period and subtracting once removes that bias.
 *
 *  - Nothing is evaluated until a full window of samples has accumulated
 *    (warm-up == one window, not a fixed short grace). A short grace let a
 *    single lossy interval right after it force a reconnect before the window
 *    had any history to average against.
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
	/* Window loss rate (%) that tripped the last reconnect verdict; kept after
	 * the reset so the caller can put it in the reconnect report. */
	double last_trigger_pct;
	int64_t start_ms; /* first record() after (re)arm; 0 until then */
	int64_t next_sample_ms;
	uint64_t acc_lost;
	uint64_t acc_retrans;
	uint64_t acc_sent;
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
	cfg->period_sec = 30;
	cfg->window_sec = 300;
	cfg->threshold_pct = 1.5;

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
		cfg->threshold_pct = 1.5;
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
	m->start_ms = 0;
	m->next_sample_ms = 0;
	m->acc_lost = 0;
	m->acc_retrans = 0;
	m->acc_sent = 0;
	m->head = 0;
	m->count = 0;
}

/*
 * Feed the per-interval deltas since the previous call: lost = packets
 * declared lost, retrans = packets retransmitted, sent = original packets
 * sent (all as deltas, not cumulative totals; WHIP callers pass retrans=0).
 * Returns true when the window's aggregate unrecoverable-loss rate exceeds
 * the threshold, meaning the caller should force a reconnect. When a full
 * sample period has elapsed, *have_sample is set and *sample_pct carries that
 * period's own loss rate (for logging).
 */
static inline bool publish_loss_monitor_record(struct publish_loss_monitor *m, uint64_t lost, uint64_t retrans,
					       uint64_t sent, int64_t now_ms, bool *have_sample, double *sample_pct)
{
	if (have_sample)
		*have_sample = false;
	if (sample_pct)
		*sample_pct = 0.0;
	if (!m->enable)
		return false;

	if (m->start_ms == 0)
		m->start_ms = now_ms;

	m->acc_lost += lost;
	m->acc_retrans += retrans;
	m->acc_sent += sent;

	if (m->next_sample_ms == 0) {
		m->next_sample_ms = now_ms + m->period_ms;
		return false;
	}

	if (now_ms < m->next_sample_ms)
		return false;

	m->next_sample_ms = now_ms + m->period_ms;

	/* Compute this period's unrecoverable loss ONCE, from the period
	 * aggregates - not per raw sample - so retransmissions that land just
	 * after a sample boundary can't bias the total upward. */
	uint64_t period_lost = m->acc_lost;
	uint64_t period_retrans = m->acc_retrans;
	uint64_t period_sent = m->acc_sent;
	m->acc_lost = 0;
	m->acc_retrans = 0;
	m->acc_sent = 0;

	uint64_t period_unrecoverable = period_lost > period_retrans ? period_lost - period_retrans : 0;
	uint64_t period_expected = period_sent > period_retrans ? period_sent - period_retrans : 0;

	m->sample_ms[m->head] = now_ms;
	m->sample_lost[m->head] = period_unrecoverable;
	m->sample_expected[m->head] = period_expected;
	m->head = (m->head + 1) % PUBLISH_LOSS_MAX_SAMPLES;
	if (m->count < PUBLISH_LOSS_MAX_SAMPLES)
		m->count++;

	if (have_sample)
		*have_sample = true;
	if (sample_pct && period_expected)
		*sample_pct = (double)period_unrecoverable * 100.0 / (double)period_expected;

	/* Warm-up == one full window: samples keep accumulating, but nothing is
	 * evaluated until the window spans window_ms, so a lossy opening period
	 * can't force a reconnect before the aggregate has real history. */
	if (now_ms - m->start_ms < m->window_ms)
		return false;

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
		m->last_trigger_pct = win_pct;
		return true;
	}
	return false;
}
