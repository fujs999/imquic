/*
 * imquic
 *
 * Adaptive bitrate controller for imquic-moq-loc-send and imquic-roq-sender
 *
 */

#ifndef MOQ_LOC_ABR_H
#define MOQ_LOC_ABR_H

#include <stdint.h>

#include <glib.h>
#include <imquic/imquic.h>

#define MOQ_LOC_ABR_LEVELS    6
#define MOQ_LOC_ABR_SUBSTEPS  3
#define MOQ_LOC_ABR_TOTAL_STEPS (MOQ_LOC_ABR_LEVELS * MOQ_LOC_ABR_SUBSTEPS)

/* Target network limits (microseconds for RTT/jitter) */
#define MOQ_LOC_ABR_RTT_TARGET_US     150000
#define MOQ_LOC_ABR_JITTER_TARGET_US  50000
#define MOQ_LOC_ABR_LOSS_TARGET       0.50

/* ABR switching hysteresis (500ms update interval) */
#define MOQ_LOC_ABR_IMPROVE_HOLDOFF   6
#define MOQ_LOC_ABR_DEGRADE_HOLDOFF   3
#define MOQ_LOC_ABR_MIN_LEVEL_DWELL   4
#define MOQ_LOC_ABR_STRESS_HYSTERESIS 0.04

typedef struct moq_loc_abr_config {
	int width;
	int height;
	int fps;
	int video_bitrate;
	int audio_bitrate;
} moq_loc_abr_config;

typedef struct moq_loc_abr_stats {
	double stress;
	double loss_rate;
	uint64_t rtt_us;
	uint64_t jitter_us;
	uint64_t pacing_rate_bps;
	int level;
	int substep;
	int upgrade_holdoff;
} moq_loc_abr_stats;

typedef struct moq_loc_abr moq_loc_abr;

moq_loc_abr *moq_loc_abr_create(int max_width, int max_height, int max_fps,
	int max_video_bitrate, int max_audio_bitrate);
void moq_loc_abr_destroy(moq_loc_abr *abr);

void moq_loc_abr_update(moq_loc_abr *abr, imquic_connection *conn,
	uint64_t send_ok, uint64_t send_fail, uint64_t video_bytes_sent);

void moq_loc_abr_get_config(const moq_loc_abr *abr, moq_loc_abr_config *config);
void moq_loc_abr_get_stats(const moq_loc_abr *abr, moq_loc_abr_stats *stats);

int moq_loc_abr_config_generation(const moq_loc_abr *abr);

void moq_loc_abr_fit_dimensions(int max_width, int max_height, int target_width,
	int *width, int *height);

#endif
