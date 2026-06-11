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

static void *roq_capture_audio_thread(void *user_data);
static void *roq_capture_video_capture_thread(void *user_data);
static void *roq_capture_video_enc_thread(void *user_data);

static gboolean roq_capture_emit_rtp(uint8_t *rtp, size_t rtp_len, void *user_data) {
	uint64_t flow_id = GPOINTER_TO_UINT(user_data);
	if(rtp_out_cb != NULL)
		rtp_out_cb(flow_id, rtp, rtp_len, rtp_out_user_data);
	return TRUE;
}

static int roq_capture_open_video_encoder(int width, int height, int fps, int bitrate) {
	if(video_codec == NULL || width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0)
		return -1;
	if(videoenc_ctx != NULL) {
		avcodec_flush_buffers(videoenc_ctx);
		avcodec_free_context(&videoenc_ctx);
		videoenc_ctx = NULL;
	}
	videoenc_ctx = avcodec_alloc_context3(video_codec);
	videoenc_ctx->bit_rate = bitrate;
	videoenc_ctx->rc_max_rate = videoenc_ctx->bit_rate + (videoenc_ctx->bit_rate / 10);
	videoenc_ctx->rc_buffer_size = 2 * videoenc_ctx->bit_rate;
	videoenc_ctx->width = width;
	videoenc_ctx->height = height;
	videoenc_ctx->time_base = (AVRational){ 1, fps };
	videoenc_ctx->framerate = (AVRational){ fps, 1 };
	videoenc_ctx->gop_size = fps * 2;
	videoenc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	videoenc_ctx->profile = FF_PROFILE_H264_BASELINE;
	videoenc_ctx->level = 41;
	{
		char br[20];
		g_snprintf(br, sizeof(br), "%"SCNi64, videoenc_ctx->bit_rate / 1024);
		av_opt_set(videoenc_ctx->priv_data, "b", br, AV_OPT_SEARCH_CHILDREN);
		av_opt_set(videoenc_ctx->priv_data, "crf", "23", AV_OPT_SEARCH_CHILDREN);
		av_opt_set(videoenc_ctx->priv_data, "profile", "baseline", 0);
		av_opt_set(videoenc_ctx->priv_data, "preset", "ultrafast", 0);
		av_opt_set(videoenc_ctx->priv_data, "tune", "zerolatency", 0);
	}
	if(avcodec_open2(videoenc_ctx, video_codec, NULL) < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening video encoder\n");
		avcodec_free_context(&videoenc_ctx);
		videoenc_ctx = NULL;
		return -1;
	}
	return 0;
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
	const AVInputFormat *vf = av_find_input_format(cfg.video_format);
	if(vf == NULL) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Couldn't find '%s' format\n", cfg.video_format);
		return -1;
	}
	AVDictionary *opts = NULL;
	char webcam_fps[8];
	g_snprintf(webcam_fps, sizeof(webcam_fps), "%d", cfg.video_framerate);
	av_dict_set(&opts, "framerate", webcam_fps, 0);
	av_dict_set(&opts, "video_size", cfg.video_resolution, 0);
	av_dict_set(&opts, "input_format", "yuyv422", 0);
	int ret = avformat_open_input(&webcam_fmt, cfg.video_device, vf, &opts);
	av_dict_free(&opts);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening video device '%s'\n", cfg.video_device);
		return -1;
	}
	ret = avformat_find_stream_info(webcam_fmt, NULL);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error accessing video device stream info\n");
		return -1;
	}
	video_stream = 0;
	for(unsigned int i=0; i<webcam_fmt->nb_streams; i++) {
		if(webcam_fmt->streams[i]->codecpar &&
				webcam_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream = i;
			break;
		}
	}
	const AVCodec *webcam_codec = avcodec_find_decoder(webcam_fmt->streams[video_stream]->codecpar->codec_id);
	webcam_ctx = avcodec_alloc_context3(webcam_codec);
	avcodec_parameters_to_context(webcam_ctx, webcam_fmt->streams[video_stream]->codecpar);
	if(avcodec_open2(webcam_ctx, webcam_codec, NULL) < 0) {
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
		video_codec = avcodec_find_encoder_by_name("libx264");
		if(video_codec == NULL) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Video codec 'libx264' not available in libavcodec\n");
			return -1;
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
		av_freep(&latest_frame->data[0]);
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
	int last_width = 0, last_height = 0;

	while(!g_atomic_int_get(&capture_stop)) {
		if(!g_atomic_int_get(&capture_started)) {
			g_usleep(100000);
			continue;
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
			if(sws == NULL || cfg.width != last_width || cfg.height != last_height) {
				if(sws != NULL)
					sws_freeContext(sws);
				sws = sws_getContext(video_frame->width, video_frame->height, video_frame->format,
					cfg.width, cfg.height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
				last_width = cfg.width;
				last_height = cfg.height;
			}
			AVFrame *scaled_frame = av_frame_alloc();
			scaled_frame->width = cfg.width;
			scaled_frame->height = cfg.height;
			scaled_frame->format = AV_PIX_FMT_YUV420P;
			ret = av_image_alloc(scaled_frame->data, scaled_frame->linesize,
				scaled_frame->width, scaled_frame->height, AV_PIX_FMT_YUV420P, 1);
			if(ret < 0) {
				av_freep(&scaled_frame->data[0]);
				av_frame_free(&scaled_frame);
				break;
			}
			sws_scale(sws, (const uint8_t * const*)video_frame->data, video_frame->linesize,
				0, video_frame->height, scaled_frame->data, scaled_frame->linesize);
			imquic_mutex_lock(&frame_mutex);
			if(latest_frame != NULL) {
				av_freep(&latest_frame->data[0]);
				av_frame_free(&latest_frame);
			}
			latest_frame = scaled_frame;
			imquic_mutex_unlock(&frame_mutex);
		}
	}
	av_frame_free(&video_frame);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving video capture thread\n");
	return NULL;
}

static void *roq_capture_video_enc_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting video encoding thread\n");
	AVPacket packet = { 0 };
	int64_t now = 0, before = 0;
	int fps = cfg.video_framerate > 0 ? cfg.video_framerate : 25;
	int64_t wait = G_USEC_PER_SEC / fps;

	while(!g_atomic_int_get(&capture_stop)) {
		if(!g_atomic_int_get(&capture_started)) {
			g_usleep(100000);
			continue;
		}
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
		latest_frame->pict_type = AV_PICTURE_TYPE_NONE;
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
		imquic_roq_rtp_packetize_h264_annexb(&video_rtp, packet.data, (size_t)packet.size, fps,
			roq_capture_emit_rtp, GUINT_TO_POINTER((guint)cfg.video_flow));
		av_packet_unref(&packet);
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving video encoding thread\n");
	return NULL;
}
