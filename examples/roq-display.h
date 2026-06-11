/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Audio/video playback for imquic-roq-receiver
 *
 */

#ifndef ROQ_DISPLAY
#define ROQ_DISPLAY

#include <glib.h>

#include <imquic/imquic.h>

#include "moq-utils.h"

typedef struct roq_display_config {
	gboolean play_video;
	int64_t video_flow;
	uint8_t video_pt;
	imquic_demo_video_codec video_codec;
	int svc_max_temporal_layer;
	int svc_max_spatial_layer;
	gboolean no_svc_adaptive;
	gboolean play_audio;
	int64_t audio_flow;
	uint8_t audio_pt;
	int window_width;
	int window_height;
	gboolean debug_ffmpeg;
} roq_display_config;

int roq_display_init(const roq_display_config *config);
void roq_display_connection_gone(imquic_connection *conn);
void roq_display_feed_rtp(imquic_connection *conn, uint64_t flow_id, uint8_t *rtp, size_t rtp_len);
int roq_display_handle_events(void);
int roq_display_render(void);
void roq_display_destroy(void);

#endif
