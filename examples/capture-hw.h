/*
 * imquic
 *
 * Hardware-accelerated video codec selection for capture demos
 *
 */

#ifndef CAPTURE_HW_H
#define CAPTURE_HW_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "moq-utils.h"

typedef enum imquic_demo_hw_vendor {
	IMQUIC_DEMO_HW_NONE = 0,
	IMQUIC_DEMO_HW_ROCKCHIP,
	IMQUIC_DEMO_HW_NVIDIA,
	IMQUIC_DEMO_HW_INTEL,
	IMQUIC_DEMO_HW_SOFTWARE
} imquic_demo_hw_vendor;

const char *imquic_demo_hw_vendor_str(imquic_demo_hw_vendor vendor);

/* Preferred v4l2 pixel formats (MJPEG first for lower USB/CPU load). */
const char * const *imquic_demo_v4l2_input_formats(void);

int imquic_demo_video_encoder_candidate_count(imquic_demo_video_codec codec);
gboolean imquic_demo_video_encoder_candidate_get(imquic_demo_video_codec codec, int index,
	const char **name, imquic_demo_hw_vendor *vendor);
gboolean imquic_demo_any_video_encoder_available(imquic_demo_video_codec codec);

int imquic_demo_video_decoder_candidate_count(enum AVCodecID codec_id);
gboolean imquic_demo_video_decoder_candidate_get(enum AVCodecID codec_id, int index,
	const char **name, imquic_demo_hw_vendor *vendor);

typedef void (*imquic_demo_encoder_extra_config_fn)(AVCodecContext *ctx, void *user_data);

/*
 * Open a v4l2 device, trying MJPEG then YUYV422 (or only the listed formats).
 * On success, opened_input_format receives the pixel format that worked.
 */
int imquic_demo_open_v4l2_capture(const char *device, const char *v4l2_format,
	const char *video_size, int framerate, AVFormatContext **fmt_out,
	char *opened_input_format, size_t opened_fmt_len);

/*
 * Open a video decoder, preferring Rockchip, then NVIDIA, then Intel, then software.
 * Returns 0 on success; on success *selected and *vendor are set.
 */
int imquic_demo_open_video_decoder(AVCodecContext **pctx, enum AVCodecID codec_id,
	const AVCodecParameters *par, const AVCodec **selected, imquic_demo_hw_vendor *vendor);

/*
 * Open a video encoder with hardware fallback. Common fields (resolution, bitrate, fps)
 * must be configured by the caller after a successful return if needed for re-open.
 * extra_config is invoked before each avcodec_open2 attempt (e.g. for SVC options).
 */
int imquic_demo_open_video_encoder(AVCodecContext **pctx, imquic_demo_video_codec codec,
	int width, int height, int fps, int bitrate, gboolean global_header,
	imquic_demo_encoder_extra_config_fn extra_config, void *extra_config_ud,
	const AVCodec **selected, imquic_demo_hw_vendor *vendor);

void imquic_demo_configure_video_encoder(AVCodecContext *ctx, imquic_demo_video_codec codec,
	imquic_demo_hw_vendor vendor, int bitrate, int fps);

#endif
