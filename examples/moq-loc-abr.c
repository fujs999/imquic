/*
 * imquic
 *
 * Adaptive bitrate controller for imquic-moq-loc-send and imquic-roq-sender
 *
 */

#include <math.h>
#include <string.h>

#include <imquic/debug.h>
#include <imquic/mutex.h>

#include "moq-loc-abr.h"

struct moq_loc_abr {
	int max_width, max_height, max_fps, max_video_bitrate, max_audio_bitrate;
	moq_loc_abr_config levels[MOQ_LOC_ABR_LEVELS];
	moq_loc_abr_config active;
	moq_loc_abr_stats stats;
	imquic_mutex mutex;
	int active_level;
	int config_generation;
	uint64_t prev_packets_sent;
	uint64_t prev_packets_lost;
	uint64_t prev_send_ok;
	uint64_t prev_send_fail;
	int improve_holdoff;
	int degrade_holdoff;
	int level_dwell;
};

static int moq_loc_abr_even_dim(int value) {
	if(value <= 0)
		return 2;
	return (value & 1) ? value + 1 : value;
}

static void moq_loc_abr_scale_resolution(int max_width, int max_height, double factor,
		int min_width, int *width, int *height) {
	double aspect = (double)max_width / (double)max_height;
	int w = 0, h = 0, default_min_width = 0;

	w = (int)(max_width * factor);
	if(min_width > 0 && w < min_width)
		w = min_width;
	default_min_width = moq_loc_abr_even_dim(max_width / 4);
	if(min_width <= 0 && w < default_min_width)
		w = default_min_width;
	w = moq_loc_abr_even_dim(w);
	h = moq_loc_abr_even_dim((int)(w / aspect + 0.5));

	if(w > max_width) {
		w = max_width;
		h = moq_loc_abr_even_dim((int)(w / aspect + 0.5));
	}
	if(h > max_height) {
		h = max_height;
		w = moq_loc_abr_even_dim((int)(h * aspect + 0.5));
	}

	*width = w;
	*height = h;
}

void moq_loc_abr_fit_dimensions(int max_width, int max_height, int target_width,
		int *width, int *height) {
	double factor = 0.0;
	if(max_width <= 0 || max_height <= 0 || target_width <= 0 || width == NULL || height == NULL)
		return;
	factor = (double)target_width / (double)max_width;
	if(factor > 1.0)
		factor = 1.0;
	moq_loc_abr_scale_resolution(max_width, max_height, factor, 0, width, height);
}

static void moq_loc_abr_build_ladder(moq_loc_abr *abr) {
	const double fps_factors[MOQ_LOC_ABR_LEVELS] = { 1.0, 0.80, 0.60, 0.40, 0.25, 0.10 };
	const double vbr_factors[MOQ_LOC_ABR_LEVELS] = { 1.0, 0.50, 0.25, 0.125, 0.0625, 0.03125 };
	const double abr_factors[MOQ_LOC_ABR_LEVELS] = { 1.0, 0.75, 0.50, 0.375, 0.25, 0.1875 };
	const int min_fps[MOQ_LOC_ABR_LEVELS] = { 0, 0, 0, 0, 5, 2 };
	const int min_vbr[MOQ_LOC_ABR_LEVELS] = { 0, 0, 0, 0, 64000, 32000 };
	const int min_abr[MOQ_LOC_ABR_LEVELS] = { 0, 0, 0, 0, 8000, 6000 };
	int i = 0;

	for(i = 0; i < MOQ_LOC_ABR_LEVELS; i++) {
		moq_loc_abr_config *level = &abr->levels[i];
		/* Keep max resolution at every level; adapt fps and bitrate only */
		level->width = abr->max_width;
		level->height = abr->max_height;
		level->fps = (int)(abr->max_fps * fps_factors[i]);
		if(level->fps < min_fps[i])
			level->fps = min_fps[i];
		if(level->fps <= 0)
			level->fps = 1;
		level->video_bitrate = (int)(abr->max_video_bitrate * vbr_factors[i]);
		if(level->video_bitrate < min_vbr[i])
			level->video_bitrate = min_vbr[i];
		level->audio_bitrate = (int)(abr->max_audio_bitrate * abr_factors[i]);
		if(level->audio_bitrate < min_abr[i])
			level->audio_bitrate = min_abr[i];
	}
	abr->active = abr->levels[0];
}

moq_loc_abr *moq_loc_abr_create(int max_width, int max_height, int max_fps,
		int max_video_bitrate, int max_audio_bitrate) {
	moq_loc_abr *abr = g_malloc0(sizeof(moq_loc_abr));
	abr->max_width = moq_loc_abr_even_dim(max_width);
	abr->max_height = moq_loc_abr_even_dim(max_height);
	abr->max_fps = max_fps > 0 ? max_fps : 1;
	abr->max_video_bitrate = max_video_bitrate > 0 ? max_video_bitrate : 32000;
	abr->max_audio_bitrate = max_audio_bitrate > 0 ? max_audio_bitrate : 8000;
	imquic_mutex_init(&abr->mutex);
	moq_loc_abr_build_ladder(abr);
	abr->active_level = 0;
	abr->config_generation = 1;
	abr->stats.level = 0;
	abr->improve_holdoff = MOQ_LOC_ABR_IMPROVE_HOLDOFF;
	abr->degrade_holdoff = MOQ_LOC_ABR_DEGRADE_HOLDOFF;
	abr->level_dwell = MOQ_LOC_ABR_MIN_LEVEL_DWELL;
	return abr;
}

void moq_loc_abr_destroy(moq_loc_abr *abr) {
	if(abr == NULL)
		return;
	imquic_mutex_destroy(&abr->mutex);
	g_free(abr);
}

static double moq_loc_abr_clamp01(double value) {
	if(value < 0.0)
		return 0.0;
	if(value > 1.0)
		return 1.0;
	return value;
}

static int moq_loc_abr_stress_to_level(double stress, double margin, gboolean worsen) {
	const double thresholds[MOQ_LOC_ABR_LEVELS - 1] = { 0.12, 0.28, 0.42, 0.58, 0.74 };
	int level = 0;

	for(level = 0; level < (MOQ_LOC_ABR_LEVELS - 1); level++) {
		double threshold = thresholds[level];
		if(worsen)
			threshold += margin;
		else
			threshold -= margin;
		if(stress < threshold)
			return level;
	}
	return MOQ_LOC_ABR_LEVELS - 1;
}

void moq_loc_abr_update(moq_loc_abr *abr, imquic_connection *conn,
		uint64_t send_ok, uint64_t send_fail, uint64_t video_bytes_sent) {
	imquic_path_quality pq = { 0 };
	double loss_rate = 0.0, rtt_factor = 0.0, jitter_factor = 0.0;
	double congestion_factor = 0.0, send_fail_factor = 0.0, bandwidth_factor = 0.0;
	double stress = 0.0;
	uint64_t delta_sent = 0, delta_lost = 0, delta_ok = 0, delta_fail = 0;
	int required_bps = 0;
	gboolean config_changed = FALSE;

	if(abr == NULL)
		return;

	if(conn != NULL)
		imquic_get_connection_path_quality(conn, &pq);

	imquic_mutex_lock(&abr->mutex);
	delta_sent = pq.packets_sent - abr->prev_packets_sent;
	delta_lost = pq.packets_lost - abr->prev_packets_lost;
	delta_ok = send_ok - abr->prev_send_ok;
	delta_fail = send_fail - abr->prev_send_fail;
	abr->prev_packets_sent = pq.packets_sent;
	abr->prev_packets_lost = pq.packets_lost;
	abr->prev_send_ok = send_ok;
	abr->prev_send_fail = send_fail;

	if(delta_sent > 0)
		loss_rate = (double)delta_lost / (double)delta_sent;
	else if((delta_ok + delta_fail) > 0)
		loss_rate = (double)delta_fail / (double)(delta_ok + delta_fail);

	rtt_factor = moq_loc_abr_clamp01((double)pq.rtt_us / (double)MOQ_LOC_ABR_RTT_TARGET_US);
	jitter_factor = moq_loc_abr_clamp01((double)pq.rtt_jitter_us / (double)MOQ_LOC_ABR_JITTER_TARGET_US);
	if(pq.cwin > 0)
		congestion_factor = moq_loc_abr_clamp01((double)pq.bytes_in_transit / (double)pq.cwin);
	if((delta_ok + delta_fail) > 0)
		send_fail_factor = moq_loc_abr_clamp01((double)delta_fail / (double)(delta_ok + delta_fail));

	required_bps = abr->active.video_bitrate + abr->active.audio_bitrate;
	if(pq.pacing_rate_bps > 0 && required_bps > 0) {
		double pacing_bps = (double)pq.pacing_rate_bps * 8.0;
		if(pacing_bps < (double)required_bps * 1.10)
			bandwidth_factor = moq_loc_abr_clamp01(1.0 - (pacing_bps / ((double)required_bps * 1.10)));
	}

	stress = 0.35 * moq_loc_abr_clamp01(loss_rate / MOQ_LOC_ABR_LOSS_TARGET);
	stress += 0.20 * rtt_factor;
	stress += 0.15 * jitter_factor;
	stress += 0.15 * congestion_factor;
	stress += 0.10 * send_fail_factor;
	stress += 0.05 * bandwidth_factor;
	if(stress > 1.0)
		stress = 1.0;

	if(abr->level_dwell < MOQ_LOC_ABR_MIN_LEVEL_DWELL) {
		abr->level_dwell++;
	} else {
		int target_improve = moq_loc_abr_stress_to_level(stress, MOQ_LOC_ABR_STRESS_HYSTERESIS, FALSE);
		int target_degrade = moq_loc_abr_stress_to_level(stress, MOQ_LOC_ABR_STRESS_HYSTERESIS, TRUE);

		if(target_degrade > abr->active_level) {
			if(abr->degrade_holdoff > 0) {
				abr->degrade_holdoff--;
			} else {
				abr->active_level++;
				abr->degrade_holdoff = MOQ_LOC_ABR_DEGRADE_HOLDOFF;
				abr->improve_holdoff = MOQ_LOC_ABR_IMPROVE_HOLDOFF;
				abr->level_dwell = 0;
				config_changed = TRUE;
			}
		} else if(target_improve < abr->active_level) {
			if(abr->improve_holdoff > 0) {
				abr->improve_holdoff--;
			} else {
				abr->active_level--;
				abr->improve_holdoff = MOQ_LOC_ABR_IMPROVE_HOLDOFF;
				abr->degrade_holdoff = MOQ_LOC_ABR_DEGRADE_HOLDOFF;
				abr->level_dwell = 0;
				config_changed = TRUE;
			}
		} else {
			abr->improve_holdoff = MOQ_LOC_ABR_IMPROVE_HOLDOFF;
			abr->degrade_holdoff = MOQ_LOC_ABR_DEGRADE_HOLDOFF;
		}
	}
	abr->stats.upgrade_holdoff = abr->improve_holdoff;

	abr->stats.stress = stress;
	abr->stats.loss_rate = loss_rate;
	abr->stats.rtt_us = pq.rtt_us;
	abr->stats.jitter_us = pq.rtt_jitter_us;
	abr->stats.pacing_rate_bps = pq.pacing_rate_bps;
	abr->stats.level = abr->active_level;

	if(config_changed) {
		abr->active = abr->levels[abr->active_level];
		abr->config_generation++;
		IMQUIC_LOG(IMQUIC_LOG_INFO,
			"ABR level %d (stress=%.2f, loss=%.1f%%, RTT=%"SCNu64"ms, jitter=%"SCNu64"ms): %dx%d@%dfps, video=%dbps, audio=%dbps\n",
			abr->active_level, stress, loss_rate * 100.0,
			pq.rtt_us / 1000, pq.rtt_jitter_us / 1000,
			abr->active.width, abr->active.height, abr->active.fps,
			abr->active.video_bitrate, abr->active.audio_bitrate);
	}

	(void)video_bytes_sent;
	imquic_mutex_unlock(&abr->mutex);
}

void moq_loc_abr_get_config(const moq_loc_abr *abr, moq_loc_abr_config *config) {
	if(abr == NULL || config == NULL)
		return;
	imquic_mutex_lock((imquic_mutex *)&abr->mutex);
	*config = abr->active;
	imquic_mutex_unlock((imquic_mutex *)&abr->mutex);
}

void moq_loc_abr_get_stats(const moq_loc_abr *abr, moq_loc_abr_stats *stats) {
	if(abr == NULL || stats == NULL)
		return;
	imquic_mutex_lock((imquic_mutex *)&abr->mutex);
	*stats = abr->stats;
	imquic_mutex_unlock((imquic_mutex *)&abr->mutex);
}

int moq_loc_abr_config_generation(const moq_loc_abr *abr) {
	int generation = 0;
	if(abr == NULL)
		return 0;
	imquic_mutex_lock((imquic_mutex *)&abr->mutex);
	generation = abr->config_generation;
	imquic_mutex_unlock((imquic_mutex *)&abr->mutex);
	return generation;
}
