/*
 * imquic
 *
 * Hardware-accelerated video codec selection for capture demos
 *
 */

#include <imquic/imquic.h>

#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "capture-hw.h"
#include "moq-loc-svc.h"

typedef struct imquic_demo_codec_candidate {
	const char *name;
	imquic_demo_hw_vendor vendor;
} imquic_demo_codec_candidate;

static AVBufferRef *rkmpp_hw_device = NULL;

static const imquic_demo_codec_candidate h264_encoders[] = {
	{ "h264_rkmpp", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "h264_v4l2m2m", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "h264_nvenc", IMQUIC_DEMO_HW_NVIDIA },
	{ "h264_qsv", IMQUIC_DEMO_HW_INTEL },
	{ "h264_vaapi", IMQUIC_DEMO_HW_INTEL },
	{ "libx264", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const imquic_demo_codec_candidate h264_svc_encoders[] = {
	{ "libopenh264", IMQUIC_DEMO_HW_SOFTWARE },
	{ "libx264", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const imquic_demo_codec_candidate vp8_encoders[] = {
	{ "vp8_v4l2m2m", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "vp8_qsv", IMQUIC_DEMO_HW_INTEL },
	{ "libvpx", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const imquic_demo_codec_candidate vp9_encoders[] = {
	{ "vp9_v4l2m2m", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "vp9_qsv", IMQUIC_DEMO_HW_INTEL },
	{ "vp9_vaapi", IMQUIC_DEMO_HW_INTEL },
	{ "libvpx-vp9", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const imquic_demo_codec_candidate av1_encoders[] = {
	{ "av1_rkmpp", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "av1_nvenc", IMQUIC_DEMO_HW_NVIDIA },
	{ "av1_qsv", IMQUIC_DEMO_HW_INTEL },
	{ "av1_vaapi", IMQUIC_DEMO_HW_INTEL },
	{ "libaom-av1", IMQUIC_DEMO_HW_SOFTWARE },
	{ "libsvtav1", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const imquic_demo_codec_candidate mjpeg_decoders[] = {
	{ "mjpeg_rkmpp", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "mjpeg_v4l2m2m", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "mjpeg_qsv", IMQUIC_DEMO_HW_INTEL },
	{ "mjpeg", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const imquic_demo_codec_candidate h264_decoders[] = {
	{ "h264_rkmpp", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "h264_v4l2m2m", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "h264_cuvid", IMQUIC_DEMO_HW_NVIDIA },
	{ "h264_qsv", IMQUIC_DEMO_HW_INTEL },
	{ "h264", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const imquic_demo_codec_candidate hevc_decoders[] = {
	{ "hevc_rkmpp", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "hevc_v4l2m2m", IMQUIC_DEMO_HW_ROCKCHIP },
	{ "hevc_cuvid", IMQUIC_DEMO_HW_NVIDIA },
	{ "hevc_qsv", IMQUIC_DEMO_HW_INTEL },
	{ "hevc", IMQUIC_DEMO_HW_SOFTWARE },
	{ NULL, IMQUIC_DEMO_HW_NONE }
};

static const char * const v4l2_input_formats[] = {
	"mjpeg",
	"yuyv422",
	NULL
};

static const imquic_demo_codec_candidate *imquic_demo_encoder_list(imquic_demo_video_codec codec, int *count) {
	const imquic_demo_codec_candidate *list = NULL;
	if(count != NULL)
		*count = 0;
	switch(codec) {
		case DEMO_H264_AVCC:
		case DEMO_H264_ANNEXB:
			list = h264_encoders;
			break;
		case DEMO_H264_SVC:
			list = h264_svc_encoders;
			break;
		case DEMO_VP8:
			list = vp8_encoders;
			break;
		case DEMO_VP9:
		case DEMO_VP9_SVC:
			list = vp9_encoders;
			break;
		case DEMO_AV1:
			list = av1_encoders;
			break;
		default:
			return NULL;
	}
	if(list != NULL && count != NULL) {
		while(list[*count].name != NULL)
			(*count)++;
	}
	return list;
}

static const imquic_demo_codec_candidate *imquic_demo_decoder_list(enum AVCodecID codec_id, int *count) {
	const imquic_demo_codec_candidate *list = NULL;
	if(count != NULL)
		*count = 0;
	switch(codec_id) {
		case AV_CODEC_ID_MJPEG:
			list = mjpeg_decoders;
			break;
		case AV_CODEC_ID_H264:
			list = h264_decoders;
			break;
		case AV_CODEC_ID_HEVC:
			list = hevc_decoders;
			break;
		default:
			return NULL;
	}
	if(list != NULL && count != NULL) {
		while(list[*count].name != NULL)
			(*count)++;
	}
	return list;
}

static gboolean imquic_demo_is_rkmpp_codec(const char *name) {
	return name != NULL && g_str_has_suffix(name, "_rkmpp");
}

static int imquic_demo_ensure_rkmpp_device(void) {
	enum AVHWDeviceType type;
	int ret = 0;

	if(rkmpp_hw_device != NULL)
		return 0;
	type = av_hwdevice_find_type_by_name("rkmpp");
	if(type == AV_HWDEVICE_TYPE_NONE) {
		IMQUIC_LOG(IMQUIC_LOG_VERB, "RKMPP hwdevice type not available in this FFmpeg build\n");
		return AVERROR(ENOENT);
	}
	ret = av_hwdevice_ctx_create(&rkmpp_hw_device, type, NULL, NULL, 0);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "Failed to create RKMPP hw device: %d (%s)\n",
			ret, av_err2str(ret));
		return ret;
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Created RKMPP hardware device context\n");
	return 0;
}

static void imquic_demo_attach_rkmpp_device(AVCodecContext *ctx, const char *codec_name) {
	if(ctx == NULL || !imquic_demo_is_rkmpp_codec(codec_name))
		return;
	if(imquic_demo_ensure_rkmpp_device() < 0)
		return;
	ctx->hw_device_ctx = av_buffer_ref(rkmpp_hw_device);
}

void imquic_demo_capture_hw_deinit(void) {
	av_buffer_unref(&rkmpp_hw_device);
}

const char *imquic_demo_hw_vendor_str(imquic_demo_hw_vendor vendor) {
	switch(vendor) {
		case IMQUIC_DEMO_HW_ROCKCHIP:
			return "Rockchip";
		case IMQUIC_DEMO_HW_NVIDIA:
			return "NVIDIA";
		case IMQUIC_DEMO_HW_INTEL:
			return "Intel";
		case IMQUIC_DEMO_HW_SOFTWARE:
			return "software";
		default:
			return "unknown";
	}
}

const char * const *imquic_demo_v4l2_input_formats(void) {
	return v4l2_input_formats;
}

int imquic_demo_video_encoder_candidate_count(imquic_demo_video_codec codec) {
	int count = 0;
	imquic_demo_encoder_list(codec, &count);
	return count;
}

gboolean imquic_demo_video_encoder_candidate_get(imquic_demo_video_codec codec, int index,
		const char **name, imquic_demo_hw_vendor *vendor) {
	int count = 0;
	const imquic_demo_codec_candidate *list = imquic_demo_encoder_list(codec, &count);
	if(list == NULL || index < 0 || index >= count)
		return FALSE;
	if(name != NULL)
		*name = list[index].name;
	if(vendor != NULL)
		*vendor = list[index].vendor;
	return TRUE;
}

gboolean imquic_demo_any_video_encoder_available(imquic_demo_video_codec codec) {
	int count = imquic_demo_video_encoder_candidate_count(codec);
	const char *name = NULL;
	for(int i = 0; i < count; i++) {
		imquic_demo_video_encoder_candidate_get(codec, i, &name, NULL);
		if(name != NULL && avcodec_find_encoder_by_name(name) != NULL)
			return TRUE;
	}
	return FALSE;
}

int imquic_demo_video_decoder_candidate_count(enum AVCodecID codec_id) {
	int count = 0;
	imquic_demo_decoder_list(codec_id, &count);
	return count;
}

gboolean imquic_demo_video_decoder_candidate_get(enum AVCodecID codec_id, int index,
		const char **name, imquic_demo_hw_vendor *vendor) {
	int count = 0;
	const imquic_demo_codec_candidate *list = imquic_demo_decoder_list(codec_id, &count);
	if(list == NULL || index < 0 || index >= count)
		return FALSE;
	if(name != NULL)
		*name = list[index].name;
	if(vendor != NULL)
		*vendor = list[index].vendor;
	return TRUE;
}

int imquic_demo_open_v4l2_capture(const char *device, const char *v4l2_format,
		const char *video_size, int framerate, AVFormatContext **fmt_out,
		char *opened_input_format, size_t opened_fmt_len) {
	const AVInputFormat *vf = NULL;
	AVFormatContext *fmt = NULL;
	AVDictionary *opts = NULL;
	char webcam_fps[8];
	const char * const *pixel_formats = v4l2_input_formats;
	int ret = -1;

	if(fmt_out == NULL || device == NULL || video_size == NULL || framerate <= 0)
		return -1;
	*fmt_out = NULL;
	if(opened_input_format != NULL && opened_fmt_len > 0)
		opened_input_format[0] = '\0';

	if(v4l2_format == NULL)
		v4l2_format = "v4l2";
	vf = av_find_input_format(v4l2_format);
	if(vf == NULL) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Couldn't find '%s' input format\n", v4l2_format);
		return -1;
	}

	g_snprintf(webcam_fps, sizeof(webcam_fps), "%d", framerate);
	for(int i = 0; pixel_formats[i] != NULL; i++) {
		opts = NULL;
		av_dict_set(&opts, "framerate", webcam_fps, 0);
		av_dict_set(&opts, "video_size", video_size, 0);
		av_dict_set(&opts, "input_format", pixel_formats[i], 0);
		ret = avformat_open_input(&fmt, device, vf, &opts);
		av_dict_free(&opts);
		if(ret < 0) {
			fmt = NULL;
			continue;
		}
		ret = avformat_find_stream_info(fmt, NULL);
		if(ret < 0) {
			avformat_close_input(&fmt);
			fmt = NULL;
			continue;
		}
		if(opened_input_format != NULL && opened_fmt_len > 0)
			g_strlcpy(opened_input_format, pixel_formats[i], opened_fmt_len);
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Opened video device '%s' with pixel format '%s'\n",
			device, pixel_formats[i]);
		*fmt_out = fmt;
		return 0;
	}

	IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening video device '%s' (tried all pixel formats)\n", device);
	return -1;
}

static gboolean imquic_demo_encoder_option_available(AVCodecContext *ctx, const char *name) {
	if(ctx == NULL || ctx->priv_data == NULL || name == NULL)
		return FALSE;
	return av_opt_find(ctx->priv_data, name, NULL, 0, AV_OPT_SEARCH_CHILDREN) != NULL;
}

static gboolean imquic_demo_is_hw_pix_fmt(enum AVPixelFormat fmt) {
	return fmt == AV_PIX_FMT_DRM_PRIME || fmt == AV_PIX_FMT_VAAPI ||
		fmt == AV_PIX_FMT_CUDA || fmt == AV_PIX_FMT_QSV;
}

int imquic_demo_prepare_sw_decode_frame(AVFrame *src, AVFrame *sw_frame, AVFrame **out) {
	int ret = 0;
	if(src == NULL || sw_frame == NULL || out == NULL)
		return AVERROR(EINVAL);
	if(!imquic_demo_is_hw_pix_fmt(src->format)) {
		*out = src;
		return 0;
	}
	av_frame_unref(sw_frame);
	ret = av_hwframe_transfer_data(sw_frame, src, 0);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "Failed to transfer hw decode frame to CPU: %d (%s)\n",
			ret, av_err2str(ret));
		return ret;
	}
	*out = sw_frame;
	return 0;
}

void imquic_demo_configure_video_encoder(AVCodecContext *ctx, imquic_demo_video_codec codec,
		imquic_demo_hw_vendor vendor, int bitrate, int fps) {
	char br[32];
	if(ctx == NULL || bitrate <= 0)
		return;

	if(vendor != IMQUIC_DEMO_HW_SOFTWARE) {
		g_snprintf(br, sizeof(br), "%d", bitrate);
		switch(vendor) {
			case IMQUIC_DEMO_HW_NVIDIA:
				av_opt_set(ctx->priv_data, "rc", "cbr", 0);
				if(imquic_demo_encoder_option_available(ctx, "preset"))
					av_opt_set(ctx->priv_data, "preset", "p1", 0);
				if(imquic_demo_encoder_option_available(ctx, "tune"))
					av_opt_set(ctx->priv_data, "tune", "ull", 0);
				if(imquic_demo_encoder_option_available(ctx, "zerolatency"))
					av_opt_set(ctx->priv_data, "zerolatency", "1", 0);
				av_opt_set(ctx->priv_data, "b", br, 0);
				if(imquic_demo_encoder_option_available(ctx, "g"))
					av_opt_set_int(ctx->priv_data, "g", fps > 0 ? fps * 2 : 50, 0);
				break;
			case IMQUIC_DEMO_HW_INTEL:
				g_snprintf(br, sizeof(br), "%d", bitrate / 1024);
				av_opt_set(ctx->priv_data, "b", br, AV_OPT_SEARCH_CHILDREN);
				if(imquic_demo_encoder_option_available(ctx, "look_ahead"))
					av_opt_set(ctx->priv_data, "look_ahead", "0", AV_OPT_SEARCH_CHILDREN);
				if(imquic_demo_encoder_option_available(ctx, "async_depth"))
					av_opt_set(ctx->priv_data, "async_depth", "1", AV_OPT_SEARCH_CHILDREN);
				break;
			case IMQUIC_DEMO_HW_ROCKCHIP:
				if(imquic_demo_encoder_option_available(ctx, "rc_mode"))
					av_opt_set(ctx->priv_data, "rc_mode", "1", 0);
				g_snprintf(br, sizeof(br), "%d", bitrate);
				av_opt_set(ctx->priv_data, "b", br, 0);
				break;
			default:
				break;
		}
		return;
	}

	g_snprintf(br, sizeof(br), "%"SCNi64, ctx->bit_rate / 1024);
	if(codec == DEMO_H264_AVCC || codec == DEMO_H264_ANNEXB || codec == DEMO_H264_SVC) {
		av_opt_set(ctx->priv_data, "b", br, AV_OPT_SEARCH_CHILDREN);
		if(codec == DEMO_H264_SVC) {
			av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
			av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
		} else {
			if(imquic_demo_encoder_option_available(ctx, "crf"))
				av_opt_set(ctx->priv_data, "crf", "23", AV_OPT_SEARCH_CHILDREN);
			av_opt_set(ctx->priv_data, "profile", "baseline", 0);
			av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
			av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
		}
	} else if(codec == DEMO_VP8) {
		g_snprintf(br, sizeof(br), "%d", bitrate / 1024);
		av_opt_set(ctx->priv_data, "b", br, 0);
		av_opt_set(ctx->priv_data, "speed", "7", 0);
		av_opt_set_double(ctx->priv_data, "max-intra-rate", 300, 0);
		av_opt_set(ctx->priv_data, "quality", "realtime", 0);
		av_opt_set(ctx->priv_data, "threads", "2", 0);
	} else if(codec == DEMO_VP9 || codec == DEMO_VP9_SVC) {
		g_snprintf(br, sizeof(br), "%d", bitrate / 1024);
		av_opt_set(ctx->priv_data, "b", br, 0);
		av_opt_set(ctx->priv_data, "speed", "7", 0);
		av_opt_set_double(ctx->priv_data, "max-intra-rate", 300, 0);
		av_opt_set(ctx->priv_data, "quality", "realtime", 0);
		if(codec == DEMO_VP9) {
			av_opt_set(ctx->priv_data, "lag-in-frames", "0", 0);
			av_opt_set_int(ctx->priv_data, "auto-alt-ref", 0, 0);
		}
		av_opt_set(ctx->priv_data, "tile-columns", "2", 0);
		av_opt_set(ctx->priv_data, "row-mt", "1", 0);
		av_opt_set(ctx->priv_data, "threads", "2", 0);
	} else if(codec == DEMO_AV1) {
		g_snprintf(br, sizeof(br), "%d", bitrate / 1024);
		av_opt_set(ctx->priv_data, "b", br, 0);
		if(imquic_demo_encoder_option_available(ctx, "cpu-used"))
			av_opt_set(ctx->priv_data, "cpu-used", "8", 0);
		if(imquic_demo_encoder_option_available(ctx, "usage"))
			av_opt_set(ctx->priv_data, "usage", "realtime", 0);
	}
}

static void imquic_demo_prepare_video_encoder_ctx(AVCodecContext *ctx, imquic_demo_video_codec codec,
		const char *codec_name, int width, int height, int fps, int bitrate, gboolean global_header) {
	ctx->bit_rate = bitrate;
	ctx->rc_max_rate = bitrate + (bitrate / 10);
	ctx->rc_buffer_size = 2 * bitrate;
	ctx->width = width;
	ctx->height = height;
	ctx->time_base = (AVRational){ 1, fps };
	ctx->framerate = (AVRational){ fps, 1 };
	ctx->gop_size = fps * 2;
	ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	if(codec_name != NULL && g_str_has_suffix(codec_name, "_vaapi"))
		ctx->pix_fmt = AV_PIX_FMT_VAAPI;
	if(global_header)
		ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if(codec == DEMO_H264_AVCC || codec == DEMO_H264_ANNEXB || codec == DEMO_H264_SVC) {
		ctx->profile = (codec == DEMO_H264_SVC) ? FF_PROFILE_H264_HIGH : FF_PROFILE_H264_BASELINE;
		ctx->level = 41;
	}
}

static int imquic_demo_try_open_codec(AVCodecContext **pctx, const AVCodec *codec,
		imquic_demo_hw_vendor vendor, const char *label) {
	int ret = avcodec_open2(*pctx, codec, NULL);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "Video %s '%s' unavailable: %d (%s)\n",
			label, codec->name, ret, av_err2str(ret));
		avcodec_free_context(pctx);
		*pctx = NULL;
		return -1;
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Using %s video %s '%s'\n",
		imquic_demo_hw_vendor_str(vendor), label, codec->name);
	return 0;
}

int imquic_demo_open_video_encoder(AVCodecContext **pctx, imquic_demo_video_codec codec,
		int width, int height, int fps, int bitrate, gboolean global_header,
		imquic_demo_encoder_extra_config_fn extra_config, void *extra_config_ud,
		const AVCodec **selected, imquic_demo_hw_vendor *vendor) {
	int count = 0;
	const imquic_demo_codec_candidate *list = imquic_demo_encoder_list(codec, &count);
	const char *name = NULL;
	imquic_demo_hw_vendor try_vendor = IMQUIC_DEMO_HW_NONE;
	const AVCodec *try_codec = NULL;
	AVCodecContext *ctx = NULL;

	if(pctx == NULL || width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0 || list == NULL)
		return -1;
	if(*pctx != NULL) {
		avcodec_free_context(pctx);
		*pctx = NULL;
	}

	for(int i = 0; i < count; i++) {
		name = list[i].name;
		try_vendor = list[i].vendor;
		try_codec = avcodec_find_encoder_by_name(name);
		if(try_codec == NULL) {
			IMQUIC_LOG(IMQUIC_LOG_VERB, "Video encoder '%s' not available in this FFmpeg build\n", name);
			continue;
		}
		ctx = avcodec_alloc_context3(try_codec);
		if(ctx == NULL)
			continue;
		imquic_demo_prepare_video_encoder_ctx(ctx, codec, name, width, height, fps, bitrate, global_header);
		imquic_demo_attach_rkmpp_device(ctx, name);
		imquic_demo_configure_video_encoder(ctx, codec, try_vendor, bitrate, fps);
		if(extra_config != NULL)
			extra_config(ctx, extra_config_ud);
		if(imquic_demo_try_open_codec(&ctx, try_codec, try_vendor, "encoder") == 0) {
			*pctx = ctx;
			if(selected != NULL)
				*selected = try_codec;
			if(vendor != NULL)
				*vendor = try_vendor;
			return 0;
		}
	}

	IMQUIC_LOG(IMQUIC_LOG_ERR, "No working video encoder found for codec '%s'\n",
		imquic_demo_video_codec_str(codec));
	return -1;
}

int imquic_demo_open_video_decoder(AVCodecContext **pctx, enum AVCodecID codec_id,
		const AVCodecParameters *par, const AVCodec **selected, imquic_demo_hw_vendor *vendor) {
	int count = 0;
	const imquic_demo_codec_candidate *list = imquic_demo_decoder_list(codec_id, &count);
	const AVCodec *try_codec = NULL;
	AVCodecContext *ctx = NULL;

	if(pctx == NULL || par == NULL)
		return -1;
	if(*pctx != NULL) {
		avcodec_free_context(pctx);
		*pctx = NULL;
	}

	if(list != NULL) {
		for(int i = 0; i < count; i++) {
			try_codec = avcodec_find_decoder_by_name(list[i].name);
			if(try_codec == NULL) {
				IMQUIC_LOG(IMQUIC_LOG_VERB, "Video decoder '%s' not available in this FFmpeg build\n",
					list[i].name);
				continue;
			}
			ctx = avcodec_alloc_context3(try_codec);
			if(ctx == NULL)
				continue;
			if(avcodec_parameters_to_context(ctx, par) < 0) {
				avcodec_free_context(&ctx);
				continue;
			}
			imquic_demo_attach_rkmpp_device(ctx, list[i].name);
			if(imquic_demo_try_open_codec(&ctx, try_codec, list[i].vendor, "decoder") == 0) {
				*pctx = ctx;
				if(selected != NULL)
					*selected = try_codec;
				if(vendor != NULL)
					*vendor = list[i].vendor;
				return 0;
			}
		}
	}

	try_codec = avcodec_find_decoder(codec_id);
	if(try_codec == NULL) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "No decoder found for codec id %d\n", codec_id);
		return -1;
	}
	ctx = avcodec_alloc_context3(try_codec);
	if(ctx == NULL)
		return -1;
	if(avcodec_parameters_to_context(ctx, par) < 0) {
		avcodec_free_context(&ctx);
		return -1;
	}
	if(imquic_demo_try_open_codec(&ctx, try_codec, IMQUIC_DEMO_HW_SOFTWARE, "decoder") < 0)
		return -1;
	*pctx = ctx;
	if(selected != NULL)
		*selected = try_codec;
	if(vendor != NULL)
		*vendor = IMQUIC_DEMO_HW_SOFTWARE;
	return 0;
}
