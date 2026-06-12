/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Audio/video capture and encoding for imquic-roq-sender
 *
 */

#include <imquic/imquic.h>

#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavdevice/avdevice.h>

#include <opus/opus.h>
#include <SDL2/SDL.h>

#include "roq-capture.h"
#include "capture-hw.h"
#include "moq-loc-abr.h"
#include "moq-loc-svc.h"
#include "roq-utils.h"

#define ROQ_AUDIO_SAMPLE_RATE 48000
#define ROQ_AUDIO_FRAME_SAMPLES 960
#define ROQ_AUDIO_TS_INCREMENT 960

static roq_capture_config cfg = { 0 };
static roq_capture_rtp_cb rtp_out_cb = NULL;
static void *rtp_out_user_data = NULL;

static volatile int capture_stop = 0;
static volatile int capture_started = 0;

static imquic_roq_rtp_state audio_rtp = { 0 }, video_rtp = { 0 };

static OpusEncoder *audioenc = NULL;
static SDL_AudioDeviceID audio_dev = 0;
static GThread *audio_thread = NULL;

static AVFormatContext *webcam_fmt = NULL;
static unsigned int video_stream = 0;
static AVCodecContext *webcam_ctx = NULL, *videoenc_ctx = NULL;
static const AVCodec *video_codec = NULL;
static struct SwsContext *sws = NULL;
static GThread *video_capture_thread = NULL, *video_enc_thread = NULL;
static AVFrame *latest_frame = NULL;
static imquic_mutex frame_mutex = IMQUIC_MUTEX_INITIALIZER;

static moq_loc_abr *abr = NULL;
static moq_loc_svc_abr *svc_abr = NULL;
static moq_loc_svc_config svc_cfg = { 0 };
static imquic_demo_video_codec video_codec_id = DEMO_H264_ANNEXB;
static volatile int remote_max_temporal_layer = -1;
static int applied_enc_generation = 0, applied_audio_bitrate = 0;
static int enc_target_width = 0, enc_target_height = 0, enc_target_fps = 0;
static enum AVPixelFormat enc_target_pix_fmt = AV_PIX_FMT_YUV420P;
static imquic_demo_hw_vendor video_enc_vendor = IMQUIC_DEMO_HW_NONE;
static volatile int force_video_keyframe = 0;

static void *roq_capture_audio_thread(void *user_data);
static void *roq_capture_video_capture_thread(void *user_data);
static void *roq_capture_video_enc_thread(void *user_data);

static gboolean roq_capture_emit_rtp(uint8_t *rtp, size_t rtp_len, void *user_data) {
	uint64_t flow_id = GPOINTER_TO_UINT(user_data);
	if(rtp_out_cb != NULL)
		rtp_out_cb(flow_id, rtp, rtp_len, rtp_out_user_data);
	return TRUE;
}

static gboolean roq_capture_encoder_option_available(AVCodecContext *ctx, const char *name) {
	if(ctx == NULL || ctx->priv_data == NULL || name == NULL)
		return FALSE;
	return av_opt_find(ctx->priv_data, name, NULL, 0, AV_OPT_SEARCH_CHILDREN) != NULL;
}

static int roq_capture_build_vp9_ts_parameters(char *buf, size_t buflen, int temporal_layers, int total_bitrate_bps) {
	char br[128], dec[64];
	int total_kbps = 0, i = 0, br_len = 0, dec_len = 0;
	if(buf == NULL || buflen == 0 || temporal_layers < 2)
		return -1;
	total_kbps = total_bitrate_bps / 1024;
	if(total_kbps < temporal_layers)
		total_kbps = temporal_layers;
	br[0] = '\0';
	dec[0] = '\0';
	for(i = 0; i < temporal_layers; i++) {
		int layer_br = total_kbps / (1 << (temporal_layers - 1 - i));
		int dec_val = 1 << (temporal_layers - 1 - i);
		br_len += g_snprintf(br + br_len, sizeof(br) - br_len, "%s%d", i ? "," : "", layer_br);
		dec_len += g_snprintf(dec + dec_len, sizeof(dec) - dec_len, "%s%d", i ? "," : "", dec_val);
	}
	return g_snprintf(buf, buflen,
		"ts_number_layers=%d:ts_target_bitrate=%s:ts_rate_decimator=%s:ts_layering_mode=3",
		temporal_layers, br, dec);
}

static void roq_capture_configure_svc_encoder(AVCodecContext *ctx) {
	char layers[8], ts_buf[256];
	if(ctx == NULL || !svc_cfg.enabled)
		return;
	if(video_codec_id == DEMO_H264_SVC) {
		av_opt_set(ctx->priv_data, "profile", "high", AV_OPT_SEARCH_CHILDREN);
		av_opt_set_int(ctx->priv_data, "allow_skip_frames", 1, AV_OPT_SEARCH_CHILDREN);
		g_snprintf(layers, sizeof(layers), "%d", svc_cfg.temporal_layers);
		av_opt_set(ctx->priv_data, "slices", layers, AV_OPT_SEARCH_CHILDREN);
	} else if(video_codec_id == DEMO_VP9_SVC) {
		av_opt_set(ctx->priv_data, "lag-in-frames", "25", AV_OPT_SEARCH_CHILDREN);
		av_opt_set_int(ctx->priv_data, "auto-alt-ref", 1, AV_OPT_SEARCH_CHILDREN);
		av_opt_set(ctx->priv_data, "temporal-aq", "1", AV_OPT_SEARCH_CHILDREN);
		if(roq_capture_encoder_option_available(ctx, "ts-parameters")) {
			if(roq_capture_build_vp9_ts_parameters(ts_buf, sizeof(ts_buf),
					svc_cfg.temporal_layers, (int)ctx->bit_rate) < 0 ||
					av_opt_set(ctx->priv_data, "ts-parameters", ts_buf, AV_OPT_SEARCH_CHILDREN) < 0) {
				IMQUIC_LOG(IMQUIC_LOG_WARN, "Failed to configure VP9 SVC via ts-parameters\n");
			} else {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "VP9 SVC temporal scaling: %s\n", ts_buf);
			}
		} else if(roq_capture_encoder_option_available(ctx, "vp9-temporal-layers")) {
			IMQUIC_LOG(IMQUIC_LOG_WARN,
				"VP9 SVC: ts-parameters not available, falling back to legacy vp9-temporal-layers\n");
			g_snprintf(layers, sizeof(layers), "%d", svc_cfg.temporal_layers);
			if(av_opt_set(ctx->priv_data, "vp9-temporal-layers", layers, AV_OPT_SEARCH_CHILDREN) < 0)
				IMQUIC_LOG(IMQUIC_LOG_WARN, "Failed to set vp9-temporal-layers for VP9 SVC\n");
		} else {
			IMQUIC_LOG(IMQUIC_LOG_WARN,
				"VP9 SVC: libvpx-vp9 lacks ts-parameters and vp9-temporal-layers, temporal scalability disabled\n");
		}
	}
}

static void roq_capture_svc_encoder_extra_config(AVCodecContext *ctx, void *user_data) {
	(void)user_data;
	if(moq_loc_svc_is_svc_codec(video_codec_id))
		roq_capture_configure_svc_encoder(ctx);
}

static int roq_capture_open_video_encoder(int width, int height, int fps, int bitrate) {
	if(width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0)
		return -1;
	if(imquic_demo_open_video_encoder(&videoenc_ctx, video_codec_id, width, height, fps, bitrate, FALSE,
			roq_capture_svc_encoder_extra_config, NULL, &video_codec, &video_enc_vendor) < 0) {
		return -1;
	}
	enc_target_pix_fmt = imquic_demo_encoder_pix_fmt(videoenc_ctx);
	enc_target_width = width;
	enc_target_height = height;
	enc_target_fps = fps;
	g_atomic_int_set(&force_video_keyframe, 1);
	imquic_mutex_lock(&frame_mutex);
	if(latest_frame != NULL) {
		av_frame_free(&latest_frame);
		latest_frame = NULL;
	}
	imquic_mutex_unlock(&frame_mutex);
	return 0;
}

static int roq_capture_apply_video_bitrate(int bitrate) {
	char br[20];
	if(videoenc_ctx == NULL || bitrate <= 0)
		return -1;
	videoenc_ctx->bit_rate = bitrate;
	videoenc_ctx->rc_max_rate = bitrate + (bitrate / 10);
	videoenc_ctx->rc_buffer_size = 2 * bitrate;
	g_snprintf(br, sizeof(br), "%d", bitrate / 1024);
	av_opt_set(videoenc_ctx->priv_data, "b", br, AV_OPT_SEARCH_CHILDREN);
	return 0;
}

static void roq_capture_apply_abr_video_config(void) {
	moq_loc_abr_config abr_cfg = { 0 };
	int generation = 0;
	if(abr == NULL || moq_loc_svc_is_svc_codec(video_codec_id))
		return;
	generation = moq_loc_abr_config_generation(abr);
	if(generation == applied_enc_generation)
		return;
	moq_loc_abr_get_config(abr, &abr_cfg);
	{
		int enc_width = 0, enc_height = 0;
		moq_loc_abr_fit_dimensions(cfg.width, cfg.height, abr_cfg.width, &enc_width, &enc_height);
		if(enc_width != enc_target_width || enc_height != enc_target_height || abr_cfg.fps != enc_target_fps) {
			if(roq_capture_open_video_encoder(enc_width, enc_height, abr_cfg.fps, abr_cfg.video_bitrate) < 0)
				return;
		} else {
			roq_capture_apply_video_bitrate(abr_cfg.video_bitrate);
		}
	}
	applied_enc_generation = generation;
}

void roq_capture_set_abr(moq_loc_abr *controller) {
	abr = controller;
}

void roq_capture_set_svc_abr(moq_loc_svc_abr *controller) {
	svc_abr = controller;
}

void roq_capture_set_remote_max_temporal_layer(int max_layer) {
	g_atomic_int_set(&remote_max_temporal_layer, max_layer);
}

static int roq_capture_effective_max_temporal_layer(void) {
	int local_max = svc_cfg.max_send_temporal_layer;
	int remote_max = g_atomic_int_get(&remote_max_temporal_layer);
	if(svc_abr != NULL)
		local_max = moq_loc_svc_abr_get_max_temporal_layer(svc_abr);
	if(remote_max >= 0 && remote_max < local_max)
		local_max = remote_max;
	return local_max;
}

static void roq_capture_apply_svc_send_layer(void) {
	if(!svc_cfg.enabled)
		return;
	svc_cfg.max_send_temporal_layer = roq_capture_effective_max_temporal_layer();
}

static int roq_capture_create_audio(void) {
	int opus_error = 0;
	audioenc = opus_encoder_create(ROQ_AUDIO_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_error);
	if(opus_error != OPUS_OK) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening audio encoder\n");
		return -1;
	}
	if(opus_encoder_ctl(audioenc, OPUS_SET_BITRATE(cfg.audio_bitrate)) != OPUS_OK) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error setting audio bitrate to %d bps\n", cfg.audio_bitrate);
		return -1;
	}
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = ROQ_AUDIO_SAMPLE_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = ROQ_AUDIO_FRAME_SAMPLES;
	audio_dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
	if(!audio_dev) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening audio device: %s\n", SDL_GetError());
		return -2;
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Opened audio device: %"SCNu16" Hz, %"SCNu8" channels, %"SCNu16" samples\n",
		have.freq, have.channels, have.samples);
	GError *error = NULL;
	audio_thread = g_thread_try_new("roq-cap-audio", &roq_capture_audio_thread, NULL, &error);
	if(error != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start audio thread\n",
			error->code, error->message ? error->message : "??");
		return -3;
	}
	return 0;
}

static int roq_capture_create_video(void) {
	char opened_fmt[32] = { 0 };
	int ret = imquic_demo_open_v4l2_capture(cfg.video_device, cfg.video_format,
		cfg.video_resolution, cfg.video_framerate, &webcam_fmt, opened_fmt, sizeof(opened_fmt));
	if(ret < 0)
		return -1;
	video_stream = 0;
	for(unsigned int i=0; i<webcam_fmt->nb_streams; i++) {
		if(webcam_fmt->streams[i]->codecpar &&
				webcam_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream = i;
			break;
		}
	}
	ret = imquic_demo_open_video_decoder(&webcam_ctx,
		webcam_fmt->streams[video_stream]->codecpar->codec_id,
		webcam_fmt->streams[video_stream]->codecpar, NULL, NULL);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening video device decoder\n");
		return -1;
	}
	if(roq_capture_open_video_encoder(cfg.width, cfg.height, cfg.video_framerate, cfg.video_bitrate) < 0)
		return -1;
	GError *error = NULL;
	video_capture_thread = g_thread_try_new("roq-cap-video", &roq_capture_video_capture_thread, NULL, &error);
	if(error != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start video capture thread\n",
			error->code, error->message ? error->message : "??");
		return -3;
	}
	error = NULL;
	video_enc_thread = g_thread_try_new("roq-enc-video", &roq_capture_video_enc_thread, NULL, &error);
	if(error != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start video encode thread\n",
			error->code, error->message ? error->message : "??");
		return -4;
	}
	return 0;
}

int roq_capture_init(const roq_capture_config *config, roq_capture_rtp_cb cb, void *user_data) {
	if(config == NULL || cb == NULL)
		return -1;
	if(!config->capture_audio && !config->capture_video)
		return -1;
	memcpy(&cfg, config, sizeof(cfg));
	if(cfg.video_encode_device != NULL)
		imquic_demo_set_v4l2_encode_device(cfg.video_encode_device);
	rtp_out_cb = cb;
	rtp_out_user_data = user_data;
	capture_stop = 0;
	capture_started = 0;

	if(cfg.debug_ffmpeg)
		av_log_set_level(AV_LOG_DEBUG);
	avdevice_register_all();
	avformat_network_init();

	if(SDL_Init(SDL_INIT_AUDIO) < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Couldn't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if(cfg.capture_audio) {
		imquic_roq_rtp_state_init(&audio_rtp, cfg.audio_pt, (uint32_t)g_random_int());
		if(roq_capture_create_audio() < 0)
			return -1;
	}
	if(cfg.capture_video) {
		imquic_roq_rtp_state_init(&video_rtp, cfg.video_pt, (uint32_t)g_random_int());
		video_codec_id = cfg.video_codec;
		if(video_codec_id == DEMO_UNKOWN)
			video_codec_id = DEMO_H264_ANNEXB;
		if(!imquic_demo_any_video_encoder_available(video_codec_id)) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Video codec '%s' not available in libavcodec\n",
				imquic_demo_video_codec_str(video_codec_id));
			return -1;
		}
		if(moq_loc_svc_is_svc_codec(video_codec_id)) {
			moq_loc_svc_config_init(&svc_cfg);
			svc_cfg.enabled = TRUE;
			svc_cfg.codec = video_codec_id;
			if(cfg.svc_temporal_layers > 0)
				svc_cfg.temporal_layers = cfg.svc_temporal_layers;
			if(cfg.svc_spatial_layers > 0)
				svc_cfg.spatial_layers = cfg.svc_spatial_layers;
			svc_cfg.max_send_temporal_layer = svc_cfg.temporal_layers - 1;
			if(!moq_loc_svc_config_validate(&svc_cfg)) {
				IMQUIC_LOG(IMQUIC_LOG_ERR, "Invalid SVC configuration\n");
				return -1;
			}
			IMQUIC_LOG(IMQUIC_LOG_INFO, "SVC enabled: %d temporal layer(s), %d spatial layer(s)\n",
				svc_cfg.temporal_layers, svc_cfg.spatial_layers);
		}
		if(roq_capture_create_video() < 0)
			return -1;
	}
	return 0;
}

void roq_capture_start(void) {
	g_atomic_int_set(&capture_started, 1);
}

void roq_capture_pause(void) {
	g_atomic_int_set(&capture_started, 0);
}

void roq_capture_destroy(void) {
	g_atomic_int_set(&capture_stop, 1);
	if(audio_thread != NULL) {
		g_thread_join(audio_thread);
		audio_thread = NULL;
	}
	if(video_capture_thread != NULL) {
		g_thread_join(video_capture_thread);
		video_capture_thread = NULL;
	}
	if(video_enc_thread != NULL) {
		g_thread_join(video_enc_thread);
		video_enc_thread = NULL;
	}
	if(audio_dev)
		SDL_CloseAudioDevice(audio_dev);
	audio_dev = 0;
	if(audioenc != NULL) {
		opus_encoder_destroy(audioenc);
		audioenc = NULL;
	}
	imquic_mutex_lock(&frame_mutex);
	if(latest_frame != NULL) {
		av_frame_free(&latest_frame);
		latest_frame = NULL;
	}
	imquic_mutex_unlock(&frame_mutex);
	if(webcam_fmt != NULL) {
		avformat_close_input(&webcam_fmt);
		webcam_fmt = NULL;
	}
	if(webcam_ctx != NULL)
		avcodec_free_context(&webcam_ctx);
	if(videoenc_ctx != NULL)
		avcodec_free_context(&videoenc_ctx);
	if(sws != NULL) {
		sws_freeContext(sws);
		sws = NULL;
	}
	imquic_demo_capture_hw_deinit();
	avformat_network_deinit();
	SDL_Quit();
}

static void *roq_capture_audio_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting audio capture thread\n");
	gboolean paused = TRUE;
	uint8_t pcm[ROQ_AUDIO_FRAME_SAMPLES * 2], rtp[1500];
	uint32_t cached = 0, want = ROQ_AUDIO_FRAME_SAMPLES * 2;

	while(!g_atomic_int_get(&capture_stop)) {
		if(!g_atomic_int_get(&capture_started)) {
			if(!paused) {
				paused = TRUE;
				SDL_PauseAudioDevice(audio_dev, 1);
			}
			g_usleep(100000);
			continue;
		}
		if(paused) {
			paused = FALSE;
			SDL_PauseAudioDevice(audio_dev, 0);
		}
		uint32_t avail = SDL_GetQueuedAudioSize(audio_dev);
		if(avail == 0) {
			g_usleep(1000);
			continue;
		}
		if((cached + avail) >= want)
			avail = want - cached;
		uint32_t got = SDL_DequeueAudio(audio_dev, pcm + cached, avail);
		cached += got;
		if(cached < want)
			continue;
		if(abr != NULL) {
			moq_loc_abr_config abr_cfg = { 0 };
			moq_loc_abr_get_config(abr, &abr_cfg);
			if(abr_cfg.audio_bitrate != applied_audio_bitrate) {
				if(opus_encoder_ctl(audioenc, OPUS_SET_BITRATE(abr_cfg.audio_bitrate)) == OPUS_OK)
					applied_audio_bitrate = abr_cfg.audio_bitrate;
			}
		}
		int length = opus_encode(audioenc, (opus_int16 *)pcm, ROQ_AUDIO_FRAME_SAMPLES, rtp + 12, sizeof(rtp) - 12);
		cached = 0;
		if(length < 0) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error encoding the Opus frame: %d (%s)\n", length, opus_strerror(length));
			continue;
		}
		size_t plen = imquic_roq_rtp_build_packet(&audio_rtp, rtp, sizeof(rtp),
			rtp + 12, (size_t)length, FALSE, ROQ_AUDIO_TS_INCREMENT);
		if(plen > 0)
			rtp_out_cb((uint64_t)cfg.audio_flow, rtp, plen, rtp_out_user_data);
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving audio capture thread\n");
	return NULL;
}

static void *roq_capture_video_capture_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting video capture thread\n");
	AVPacket packet = { 0 };
	AVFrame *video_frame = av_frame_alloc();
	AVFrame *sw_video_frame = av_frame_alloc();
	int scale_width = 0, scale_height = 0, last_scale_width = 0, last_scale_height = 0;

	while(!g_atomic_int_get(&capture_stop)) {
		if(!g_atomic_int_get(&capture_started)) {
			g_usleep(100000);
			continue;
		}
		{
			int target_width = cfg.width;
			if(abr != NULL) {
				moq_loc_abr_config abr_cfg = { 0 };
				moq_loc_abr_get_config(abr, &abr_cfg);
				if(abr_cfg.width > 0)
					target_width = abr_cfg.width;
			}
			/* Keep scaling aligned with the active encoder dimensions */
			if(enc_target_width > 0)
				target_width = enc_target_width;
			moq_loc_abr_fit_dimensions(cfg.width, cfg.height, target_width,
				&scale_width, &scale_height);
		}
		memset(&packet, 0, sizeof(packet));
		int ret = av_read_frame(webcam_fmt, &packet);
		if(ret < 0) {
			av_packet_unref(&packet);
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error getting a frame from the video device: %d (%s)\n",
				ret, av_err2str(ret));
			break;
		}
		ret = avcodec_send_packet(webcam_ctx, &packet);
		av_packet_unref(&packet);
		if(ret < 0 && ret != AVERROR(EAGAIN)) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error decoding frame from the video device: %d (%s)\n",
				ret, av_err2str(ret));
			break;
		}
		while(TRUE) {
			ret = avcodec_receive_frame(webcam_ctx, video_frame);
			if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if(ret < 0) {
				IMQUIC_LOG(IMQUIC_LOG_ERR, "Error decoding frame from the video device: %d (%s)\n",
					ret, av_err2str(ret));
				break;
			}
			AVFrame *decode_frame = video_frame;
			if(imquic_demo_prepare_sw_decode_frame(video_frame, sw_video_frame, &decode_frame) < 0)
				continue;
			if(sws == NULL || scale_width != last_scale_width || scale_height != last_scale_height) {
				if(sws != NULL)
					sws_freeContext(sws);
				sws = sws_getContext(decode_frame->width, decode_frame->height, decode_frame->format,
					scale_width, scale_height, enc_target_pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
				last_scale_width = scale_width;
				last_scale_height = scale_height;
			}
			AVFrame *scaled_frame = av_frame_alloc();
			scaled_frame->format = enc_target_pix_fmt;
			scaled_frame->width = scale_width;
			scaled_frame->height = scale_height;
			ret = av_frame_get_buffer(scaled_frame, 32);
			if(ret < 0) {
				av_frame_free(&scaled_frame);
				break;
			}
			sws_scale(sws, (const uint8_t * const*)decode_frame->data, decode_frame->linesize,
				0, decode_frame->height, scaled_frame->data, scaled_frame->linesize);
			imquic_mutex_lock(&frame_mutex);
			if(latest_frame != NULL)
				av_frame_free(&latest_frame);
			latest_frame = scaled_frame;
			imquic_mutex_unlock(&frame_mutex);
		}
	}
	av_frame_free(&sw_video_frame);
	av_frame_free(&video_frame);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving video capture thread\n");
	return NULL;
}

static void *roq_capture_video_enc_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting video encoding thread\n");
	AVPacket packet = { 0 };
	int64_t now = 0, before = 0;
	int target_fps = cfg.video_framerate > 0 ? cfg.video_framerate : 25;
	int64_t wait = G_USEC_PER_SEC / target_fps;

	while(!g_atomic_int_get(&capture_stop)) {
		if(!g_atomic_int_get(&capture_started)) {
			g_usleep(100000);
			continue;
		}
		if(abr != NULL) {
			moq_loc_abr_config abr_cfg = { 0 };
			roq_capture_apply_abr_video_config();
			moq_loc_abr_get_config(abr, &abr_cfg);
			if(abr_cfg.fps > 0)
				target_fps = abr_cfg.fps;
		}
		if(target_fps <= 0)
			target_fps = 1;
		wait = G_USEC_PER_SEC / target_fps;
		now = g_get_monotonic_time();
		if(before == 0)
			before = now - wait;
		if((now - before) < (wait - 1000)) {
			g_usleep(1000);
			continue;
		}
		before += wait;

		imquic_mutex_lock(&frame_mutex);
		if(latest_frame == NULL || videoenc_ctx == NULL) {
			imquic_mutex_unlock(&frame_mutex);
			continue;
		}
		if(latest_frame->width != videoenc_ctx->width || latest_frame->height != videoenc_ctx->height) {
			imquic_mutex_unlock(&frame_mutex);
			continue;
		}
		if(g_atomic_int_get(&force_video_keyframe)) {
			latest_frame->pict_type = AV_PICTURE_TYPE_I;
			g_atomic_int_set(&force_video_keyframe, 0);
		} else {
			latest_frame->pict_type = AV_PICTURE_TYPE_NONE;
		}
		int ret = avcodec_send_frame(videoenc_ctx, latest_frame);
		imquic_mutex_unlock(&frame_mutex);
		if(ret < 0) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error encoding video frame: %d (%s)\n", ret, av_err2str(ret));
			continue;
		}
		memset(&packet, 0, sizeof(packet));
		ret = avcodec_receive_packet(videoenc_ctx, &packet);
		if(ret == AVERROR(EAGAIN))
			continue;
		if(ret < 0) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error encoding video frame: %d (%s)\n", ret, av_err2str(ret));
			continue;
		}
		gboolean kf = (packet.flags & AV_PKT_FLAG_KEY);
		moq_loc_svc_layer layer = { 0 };
		gboolean svc = moq_loc_svc_is_svc_codec(video_codec_id);
		if(svc) {
			roq_capture_apply_svc_send_layer();
			if(moq_loc_svc_parse_packet(video_codec_id, packet.data, (size_t)packet.size, FALSE, &layer) < 0) {
				layer.temporal_id = 0;
				layer.spatial_id = 0;
				layer.is_keyframe = kf;
			} else if(!layer.is_keyframe) {
				layer.is_keyframe = kf;
			}
			if(layer.temporal_id > svc_cfg.max_send_temporal_layer) {
				IMQUIC_LOG(IMQUIC_LOG_VERB, "Dropping SVC layer T%d (max=%d)\n",
					layer.temporal_id, svc_cfg.max_send_temporal_layer);
				av_packet_unref(&packet);
				continue;
			}
		}
		if(video_codec_id == DEMO_VP9 || video_codec_id == DEMO_VP9_SVC) {
			imquic_roq_rtp_packetize_vp9(&video_rtp, packet.data, (size_t)packet.size, target_fps, kf,
				roq_capture_emit_rtp, GUINT_TO_POINTER((guint)cfg.video_flow));
		} else if(video_codec_id == DEMO_VP8) {
			imquic_roq_rtp_packetize_vp8(&video_rtp, packet.data, (size_t)packet.size, target_fps, kf,
				roq_capture_emit_rtp, GUINT_TO_POINTER((guint)cfg.video_flow));
		} else {
			imquic_roq_rtp_packetize_h264_annexb(&video_rtp, packet.data, (size_t)packet.size, target_fps,
				roq_capture_emit_rtp, GUINT_TO_POINTER((guint)cfg.video_flow));
		}
		av_packet_unref(&packet);
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving video encoding thread\n");
	return NULL;
}
