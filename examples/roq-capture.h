/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Audio/video capture and encoding for imquic-roq-sender
 *
 */

#ifndef ROQ_CAPTURE
#define ROQ_CAPTURE

#include <glib.h>

typedef struct roq_capture_config {
	gboolean capture_audio;
	int audio_flow;
	int audio_bitrate;
	uint8_t audio_pt;
	gboolean capture_video;
	int video_flow;
	int video_bitrate;
	int width;
	int height;
	int video_framerate;
	const char *video_format;
	const char *video_device;
	const char *video_resolution;
	uint8_t video_pt;
	gboolean debug_ffmpeg;
} roq_capture_config;

typedef void (*roq_capture_rtp_cb)(uint64_t flow_id, uint8_t *rtp, size_t rtp_len, void *user_data);

int roq_capture_init(const roq_capture_config *config, roq_capture_rtp_cb cb, void *user_data);
void roq_capture_start(void);
void roq_capture_pause(void);
void roq_capture_destroy(void);

#endif
