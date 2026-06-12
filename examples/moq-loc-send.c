/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Basic MoQ audio/video publisher using LOC
 *
 */

#include <arpa/inet.h>

#include <imquic/imquic.h>
#include <imquic/moq.h>

#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#ifndef AV_CH_LAYOUT_MONO
#include <libavutil/channel_layout.h>
#endif
#include <libavdevice/avdevice.h>

#include <opus/opus.h>

#include <SDL2/SDL.h>

#include "moq-loc-send-options.h"
#include "moq-loc-abr.h"
#include "moq-loc-svc.h"
#include "capture-hw.h"
#include "moq-utils.h"

/* Command line options */
static demo_options options = { 0 };

/* Signal */
static volatile int stop = 0, connected = 0;
static void imquic_demo_handle_signal(int signum) {
	switch(g_atomic_int_get(&stop)) {
		case 0:
			IMQUIC_PRINT("Stopping LOC sender, please wait...\n");
			break;
		case 1:
			IMQUIC_PRINT("In a hurry? I'm trying to free resources cleanly, here!\n");
			break;
		default:
			IMQUIC_PRINT("Ok, leaving immediately...\n");
			break;
	}
	g_atomic_int_inc(&stop);
	if(g_atomic_int_get(&stop) > 2)
		exit(1);
}

/* Publisher state */
static imquic_connection *moq_conn = NULL;
static imquic_moq_version moq_version = IMQUIC_MOQ_VERSION_ANY;
static uint64_t max_request_id = 100,
	moq_tns_request_id = 0,
	catalog_request_id = 0, catalog_track_alias = 0,
	audio_request_id = 0, audio_track_alias = 1,
	video_request_id = 0, video_track_alias = 2;
static imquic_moq_namespace pub_namespace[32] = { 0 };
static imquic_moq_track catalog_trackname = { 0 },
	audio_trackname = { 0 }, video_trackname = { 0 };
static char pub_tns_buffer[256], audio_tn_buffer[256], video_tn_buffer[256];
static const char *pub_tns = NULL, *catalog_tn = "catalog",
	*audio_tn = NULL, *video_tn = NULL;
static volatile int catalog_started = 0, catalog_done = 0,
	audio_started = 0, audio_done = 0,
	video_started = 0, video_done = 0;
static uint64_t audio_group_id = 0, audio_object_id = 0,
	video_group_id = 0, video_object_id = 0;
static int64_t audio_ts = 0, video_ts = 0;
static imquic_moq_location audio_sub_start = { 0 }, audio_sub_end = { 0 },
	video_sub_start = { 0 }, video_sub_end = { 0 };

/* Global SDL resources */
static SDL_AudioDeviceID dev;
static const char *imquic_demo_sdl_audioformat_str(SDL_AudioFormat format) {
	switch(format) {
		case AUDIO_U16SYS:
			return "AUDIO_U16SYS";
		case AUDIO_S16SYS:
			return "AUDIO_S16SYS";
		case AUDIO_S32SYS:
			return "AUDIO_S32SYS";
		case AUDIO_F32SYS:
			return "AUDIO_F32SYS";
		default:
			break;
	}
	return NULL;
}

/* Encoder related stuff */
static imquic_moq_catalog *catalog = NULL;
static OpusEncoder *audioenc = NULL;
static AVFormatContext *webcam_fmt = NULL;
static unsigned int video_stream = -1;
static imquic_demo_video_codec codec = DEMO_H264_AVCC;
static const AVCodec *video_codec = NULL;
static AVCodecContext *webcam_ctx = NULL, *videoenc_ctx = NULL;
static AVCodecContext *svc_spatial_enc[MOQ_LOC_SVC_MAX_SPATIAL_LAYERS] = { 0 };
static struct SwsContext *sws = NULL;
static struct SwsContext *svc_spatial_sws[MOQ_LOC_SVC_MAX_SPATIAL_LAYERS] = { 0 };
static int svc_spatial_enc_width[MOQ_LOC_SVC_MAX_SPATIAL_LAYERS] = { 0 };
static int svc_spatial_enc_height[MOQ_LOC_SVC_MAX_SPATIAL_LAYERS] = { 0 };
static GThread *audio_thread = NULL, *video_capture_thread = NULL, *video_enc_thread = NULL, *abr_thread = NULL;
static AVFrame *latest_frame = NULL;
static imquic_mutex mutex = IMQUIC_MUTEX_INITIALIZER;
static imquic_mutex send_mutex = IMQUIC_MUTEX_INITIALIZER;

/* Adaptive bitrate */
static moq_loc_abr *abr = NULL;
static moq_loc_svc_abr *svc_abr = NULL;
static moq_loc_svc_config svc_cfg = { 0 };
static uint64_t send_ok_count = 0, send_fail_count = 0, video_bytes_sent = 0;
static int applied_enc_generation = 0, applied_audio_bitrate = 0;
static int enc_target_width = 0, enc_target_height = 0, enc_target_fps = 0;
static enum AVPixelFormat enc_target_pix_fmt = AV_PIX_FMT_YUV420P;
static imquic_demo_hw_vendor video_enc_vendor = IMQUIC_DEMO_HW_NONE;
static volatile int force_video_keyframe = 0;
static int64_t video_input_pts = 0;

static void *imquic_demo_audio_thread(void *user_data);
static void *imquic_demo_video_capture_thread(void *user_data);
static void *imquic_demo_video_enc_thread(void *user_data);
static void *imquic_demo_abr_thread(void *user_data);

static void imquic_demo_clear_latest_frame(void) {
	imquic_mutex_lock(&mutex);
	if(latest_frame != NULL) {
		av_freep(&latest_frame->data[0]);
		av_frame_free(&latest_frame);
		latest_frame = NULL;
	}
	imquic_mutex_unlock(&mutex);
}

static void imquic_demo_track_send(gboolean video, int ret, size_t bytes) {
	imquic_mutex_lock(&send_mutex);
	if(ret >= 0) {
		send_ok_count++;
		if(video)
			video_bytes_sent += bytes;
	} else {
		send_fail_count++;
	}
	imquic_mutex_unlock(&send_mutex);
}

static int imquic_demo_send_object(imquic_moq_object *object, gboolean video) {
	int ret = imquic_moq_send_object(moq_conn, object);
	imquic_demo_track_send(video, ret, object ? object->payload_len : 0);
	return ret;
}

static int imquic_demo_apply_video_bitrate(int bitrate) {
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

static void imquic_demo_svc_encoder_extra_config(AVCodecContext *ctx, void *user_data) {
	(void)user_data;
	if(moq_loc_svc_is_svc_codec(codec))
		moq_loc_svc_configure_encoder(ctx, &svc_cfg);
}

static void imquic_demo_destroy_svc_spatial_encoders(void) {
	int i = 0;
	for(i = 0; i < MOQ_LOC_SVC_MAX_SPATIAL_LAYERS; i++) {
		if(svc_spatial_sws[i] != NULL) {
			sws_freeContext(svc_spatial_sws[i]);
			svc_spatial_sws[i] = NULL;
		}
		if(svc_spatial_enc[i] != NULL) {
			avcodec_free_context(&svc_spatial_enc[i]);
			svc_spatial_enc[i] = NULL;
		}
		svc_spatial_enc_width[i] = 0;
		svc_spatial_enc_height[i] = 0;
	}
}

static int imquic_demo_open_svc_spatial_encoder(int spatial_id, int width, int height, int fps, int bitrate) {
	if(spatial_id < 0 || spatial_id >= MOQ_LOC_SVC_MAX_SPATIAL_LAYERS)
		return -1;
	if(svc_spatial_enc[spatial_id] != NULL &&
			svc_spatial_enc_width[spatial_id] == width &&
			svc_spatial_enc_height[spatial_id] == height)
		return 0;
	if(svc_spatial_enc[spatial_id] != NULL) {
		avcodec_free_context(&svc_spatial_enc[spatial_id]);
		svc_spatial_enc[spatial_id] = NULL;
	}
	if(imquic_demo_open_video_encoder(&svc_spatial_enc[spatial_id], codec, width, height, fps, bitrate, TRUE,
			imquic_demo_svc_encoder_extra_config, NULL, NULL, NULL) < 0) {
		return -1;
	}
	svc_spatial_enc_width[spatial_id] = width;
	svc_spatial_enc_height[spatial_id] = height;
	IMQUIC_LOG(IMQUIC_LOG_INFO, "SVC spatial encoder S%d: %dx%d at %d bps\n",
		spatial_id, width, height, bitrate);
	return 0;
}

static int imquic_demo_init_svc_spatial_encoders(int full_width, int full_height, int fps, int total_bitrate) {
	int s = 0, w = 0, h = 0, br = 0;
	if(!moq_loc_svc_use_multi_spatial_encode(&svc_cfg))
		return 0;
	for(s = 0; s < svc_cfg.spatial_layers; s++) {
		moq_loc_svc_spatial_layer_dimensions(full_width, full_height, svc_cfg.spatial_layers, s, &w, &h);
		br = moq_loc_svc_spatial_layer_bitrate(total_bitrate, full_width, full_height,
			svc_cfg.spatial_layers, s);
		if(imquic_demo_open_svc_spatial_encoder(s, w, h, fps, br) < 0)
			return -1;
	}
	enc_target_width = full_width;
	enc_target_height = full_height;
	enc_target_fps = fps;
	if(svc_spatial_enc[svc_cfg.spatial_layers - 1] != NULL) {
		enc_target_pix_fmt = imquic_demo_encoder_pix_fmt(svc_spatial_enc[svc_cfg.spatial_layers - 1]);
	}
	return 0;
}

static int imquic_demo_open_video_encoder_ctx(int width, int height, int fps, int bitrate) {
	if(width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0)
		return -1;
	if(moq_loc_svc_use_multi_spatial_encode(&svc_cfg))
		return imquic_demo_init_svc_spatial_encoders(width, height, fps, bitrate);
	if(imquic_demo_open_video_encoder(&videoenc_ctx, codec, width, height, fps, bitrate, TRUE,
			imquic_demo_svc_encoder_extra_config, NULL, &video_codec, &video_enc_vendor) < 0) {
		return -1;
	}
	enc_target_pix_fmt = imquic_demo_encoder_pix_fmt(videoenc_ctx);
	enc_target_width = width;
	enc_target_height = height;
	enc_target_fps = fps;
	imquic_demo_clear_latest_frame();
	g_atomic_int_set(&force_video_keyframe, 1);
	video_input_pts = 0;
	return 0;
}

static void imquic_demo_apply_abr_video_config(void) {
	moq_loc_abr_config cfg = { 0 };
	int generation = 0;
	if(abr == NULL || moq_loc_svc_is_svc_codec(codec))
		return;
	generation = moq_loc_abr_config_generation(abr);
	if(generation == applied_enc_generation)
		return;
	moq_loc_abr_get_config(abr, &cfg);
	if(videoenc_ctx == NULL) {
		if(imquic_demo_open_video_encoder_ctx(cfg.width, cfg.height, cfg.fps, cfg.video_bitrate) < 0)
			return;
	} else if(cfg.width != enc_target_width || cfg.height != enc_target_height || cfg.fps != enc_target_fps) {
		if(imquic_demo_open_video_encoder_ctx(cfg.width, cfg.height, cfg.fps, cfg.video_bitrate) < 0)
			return;
	} else {
		imquic_demo_apply_video_bitrate(cfg.video_bitrate);
	}
	applied_enc_generation = generation;
}

static int imquic_demo_create_audio_encoder(void) {
	if(options.audio_track_name == NULL)
		return -1;
	/* Audio (Opus) */
	int opus_error;
	audioenc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &opus_error);
	if(opus_error != OPUS_OK) {
		/* Error creating audio decoder */
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening audio encoder\n");
		return -1;
	}
	if(opus_encoder_ctl(audioenc, OPUS_SET_BITRATE(options.audio_bitrate)) != OPUS_OK) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error setting audio bitrate to %d bps\n", options.audio_bitrate);
		return -1;
	}
	/* SDL audio capture */
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 48000;
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = 960;
	dev = SDL_OpenAudioDevice(NULL, 1, &want, &have, 0);
	if(!dev) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening audio device: %s\n", SDL_GetError());
		return -2;
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Opened audio device %d: %"SCNu16", %"SCNu8" channels, %s, %"SCNu16" samples\n",
		dev, have.freq, have.channels, imquic_demo_sdl_audioformat_str(have.format), have.samples);

	GError *error = NULL;
	audio_thread = g_thread_try_new("loc-send-audio", &imquic_demo_audio_thread, NULL, &error);
	if(error != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start audio thread\n",
			error->code, error->message ? error->message : "??");
		return -3;
	}
	/* Done */
	return 0;
}

static int imquic_demo_create_video_encoder(void) {
	if(options.video_track_name == NULL)
		return -1;
	char opened_fmt[32] = { 0 };
	int ret = imquic_demo_open_v4l2_capture(options.video_device, options.video_format,
		options.video_resolution, options.video_framerate, &webcam_fmt, opened_fmt, sizeof(opened_fmt));
	if(ret < 0)
		return -1;
	for(unsigned int i=0; i<webcam_fmt->nb_streams; i++) {
		if(webcam_fmt->streams[i]->codecpar &&
				webcam_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream = i;
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Opened video capture device (stream #%d)\n", video_stream);
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Capture rate: %d/%d (%d/%d)\n",
				webcam_fmt->streams[i]->avg_frame_rate.num,
				webcam_fmt->streams[i]->avg_frame_rate.den,
				webcam_fmt->streams[i]->r_frame_rate.num,
				webcam_fmt->streams[i]->r_frame_rate.den);
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

	/* Create a video encoder */
	int width = options.width, height = options.height, fps = options.video_framerate;
	if(imquic_demo_open_video_encoder_ctx(width, height, fps, options.video_bitrate) < 0)
		return -1;

	/* Spawn threads: we use one to capture and one to encode in order
	 * to allow us to separate capture rate and encoding rate properly */
	GError *error = NULL;
	video_capture_thread = g_thread_try_new("loc-cap-video", &imquic_demo_video_capture_thread, NULL, &error);
	if(error != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start video thread\n",
			error->code, error->message ? error->message : "??");
		return -3;
	}
	video_enc_thread = g_thread_try_new("loc-enc-video", &imquic_demo_video_enc_thread, NULL, &error);
	if(error != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start video thread\n",
			error->code, error->message ? error->message : "??");
		return -4;
	}

	/* Done */
	return 0;
}

static void imquic_demo_destroy_audio_encoder(void) {
	if(audioenc != NULL)
		opus_encoder_destroy(audioenc);
	audioenc = NULL;
}

static void imquic_demo_destroy_video_encoder(void) {
	if(webcam_fmt != NULL) {
		avformat_close_input(&webcam_fmt);
		webcam_fmt = NULL;
	}
	if(webcam_ctx != NULL)
		avcodec_free_context(&webcam_ctx);
	if(videoenc_ctx != NULL)
		avcodec_free_context(&videoenc_ctx);
	videoenc_ctx = NULL;
	imquic_demo_destroy_svc_spatial_encoders();
	if(sws != NULL)
		sws_freeContext(sws);
	sws = NULL;
	imquic_demo_capture_hw_deinit();
}

/* Annex-B to AVCC translation for SPS/PPS (AVCC extradata) */
static size_t imquic_demo_h264_spspps_to_avcc(uint8_t *avcc_data, uint8_t *buffer, size_t len) {
	/* We use this function to return a metadata JSON object for AVC1 */
	avcc_data[0] = 1;
	/* Let's check if it's the right profile, first */
	size_t index = 0;
	if(buffer[0] == 0x00 && buffer[1] == 0x00 && buffer[2] == 0x01) {
		index = 3;
	} else if(buffer[0] == 0x00 && buffer[1] == 0x00 && buffer[2] == 0x00 && buffer[3] == 0x01) {
		index = 4;
	} else {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "No NAL start code\n");
		return 0;
	}
	if(buffer[index] != 0x67) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "Not an SPS NAL (%02x)\n", buffer[index]);
		return 0;
	}
	size_t sps_index = index;
	index++;
	int profile_idc = *(buffer+index);
	avcc_data[1] = (uint8_t)profile_idc;
	if((index + 2) < len)
		avcc_data[2] = buffer[index + 1];
	if((index + 2) < len)
		avcc_data[3] = buffer[index + 2];
	avcc_data[4] = 3;
	avcc_data[5] = 1;
	size_t avcc_size = 6;

	/* Find the next NAL */
	uint16_t sps_size = 0;
	while((index + 3) < len) {
		if(buffer[index] == 0x00 && buffer[index+1] == 0x00 && buffer[index+2] == 0x01) {
			sps_size = index - sps_index;
			index += 3;
			break;
		} else if(buffer[index] == 0x00 && buffer[index+1] == 0x00 && buffer[index+2] == 0x00 && buffer[index+3] == 0x01) {
			sps_size = index - sps_index;
			index += 4;
			break;
		}
		index++;
	}
	size_t pps_index = index;

	/* Append SPS to the AVCC buffer */
	sps_size = htons(sps_size);
	memcpy(&avcc_data[avcc_size], &sps_size, 2);
	avcc_size += 2;
	sps_size = ntohs(sps_size);
	memcpy(&avcc_data[avcc_size], &buffer[sps_index], sps_size);
	avcc_size += sps_size;

	/* Append PPS to the AVCC buffer */
	size_t pps_size = len - pps_index;
	avcc_data[avcc_size] = pps_size ? 1 : 0;
	avcc_size++;
	pps_size = htons(pps_size);
	memcpy(&avcc_data[avcc_size], &pps_size, 2);
	avcc_size += 2;
	pps_size = ntohs(pps_size);
	if(pps_size > 0) {
		memcpy(&avcc_data[avcc_size], &buffer[pps_index], pps_size);
		avcc_size += pps_size;
	}

	/* Done */
	return avcc_size;
}

/* Threads */
static void *imquic_demo_audio_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting audio thread\n");

	gboolean paused = TRUE;
	int samples = 960, length = 0;
	uint8_t audio[1920], outgoing[500];
	size_t outlen = sizeof(outgoing);
	uint32_t avail = 0, want = samples*2, got = 0, cached = 0;

	while(!stop) {
		/* FIXME Loop */
		if(!g_atomic_int_get(&audio_started)) {
			if(!paused) {
				paused = TRUE;
				SDL_PauseAudioDevice(dev, 1);
			}
			g_usleep(100000);
			continue;
		}
		if(paused) {
			paused = FALSE;
			SDL_PauseAudioDevice(dev, 0);
		}

		avail = SDL_GetQueuedAudioSize(dev);
		if(avail == 0) {
			g_usleep(1000);
			continue;
		}
		IMQUIC_LOG(IMQUIC_LOG_VERB, "%"SCNu32" audio chunks available\n", avail);
		if((cached + avail) >= want)
			avail = want - cached;
		IMQUIC_LOG(IMQUIC_LOG_VERB, "  -- Dequeueing %"SCNu32" chunks (%d samples, current index %"SCNu32")\n",
			avail, avail/2, cached);
		got = SDL_DequeueAudio(dev, audio + cached, avail);
		IMQUIC_LOG(IMQUIC_LOG_VERB, "  -- -- Got %"SCNu32"/%"SCNu32" chunks (%"SCNu32" samples)\n", got, avail, got/2);
		cached += got;
		if(cached == want) {
			/* We have enough to send, encode the audio */
			if(abr != NULL) {
				moq_loc_abr_config cfg = { 0 };
				moq_loc_abr_get_config(abr, &cfg);
				if(cfg.audio_bitrate != applied_audio_bitrate) {
					if(opus_encoder_ctl(audioenc, OPUS_SET_BITRATE(cfg.audio_bitrate)) == OPUS_OK)
						applied_audio_bitrate = cfg.audio_bitrate;
				}
			}
			IMQUIC_LOG(IMQUIC_LOG_VERB, "  -- %"SCNu32" chunks cached, encoding to Opus\n", cached);
			length = opus_encode(audioenc, (opus_int16 *)audio, cached/2, outgoing, outlen);
			cached = 0;
			if(length < 0) {
				IMQUIC_LOG(IMQUIC_LOG_ERR, "Error encoding the Opus frame: %d (%s)\n", length, opus_strerror(length));
				audio_ts += 20000;	/* FIXME */
				continue;
			}
			IMQUIC_LOG(IMQUIC_LOG_VERB, "  -- -- Encoded samples to %d bytes\n", length);
			/* Write the LOC info first as properties */
			GList *props = NULL;
			imquic_moq_property timescale = { 0 };
			timescale.id = IMQUIC_MOQ_LOC_TIMESCALE;
			timescale.value.number = G_USEC_PER_SEC;
			props = g_list_append(props, &timescale);
			imquic_moq_property timestamp = { 0 };
			timestamp.id = IMQUIC_MOQ_LOC_TIMESTAMP;
			timestamp.value.number = audio_ts;
			props = g_list_append(props, &timestamp);
			audio_ts += 20000;	/* FIXME */
			/* Prepare a MoQ object and send it */
			imquic_moq_object object = {
				.request_id = audio_request_id,
				.track_alias = audio_track_alias,
				.group_id = audio_group_id++,
				.subgroup_id = 0,	/* FIXME */
				.object_id = audio_object_id,
				.payload = outgoing,
				.payload_len = length,
				.properties = props,
				.delivery = IMQUIC_MOQ_USE_DATAGRAM,
				.end_of_stream = TRUE
			};
			imquic_demo_send_object(&object, FALSE);
			g_list_free(props);
		}
	}

	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving audio thread\n");
	return NULL;
}

static void *imquic_demo_video_capture_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting video capture thread\n");

	AVPacket packet = { 0 };
	AVFrame *video_frame = av_frame_alloc();
	AVFrame *sw_video_frame = av_frame_alloc();
	int scale_width = 0, scale_height = 0, last_scale_width = 0, last_scale_height = 0;

	while(!stop) {
		/* FIXME Loop */
		if(!g_atomic_int_get(&video_started)) {
			g_usleep(100000);
			continue;
		}

		if(moq_loc_svc_use_multi_spatial_encode(&svc_cfg)) {
			scale_width = options.width;
			scale_height = options.height;
		} else if(abr != NULL) {
			moq_loc_abr_config cfg = { 0 };
			moq_loc_abr_get_config(abr, &cfg);
			scale_width = cfg.width;
			scale_height = cfg.height;
		} else {
			scale_width = options.width;
			scale_height = options.height;
		}
		if(scale_width <= 0)
			scale_width = options.width;
		if(scale_height <= 0)
			scale_height = options.height;

		/* Read from the video device */
		memset(&packet, 0, sizeof(packet));
		packet.pts = AV_NOPTS_VALUE;
		packet.dts = AV_NOPTS_VALUE;
		packet.pos = -1;
		int ret = av_read_frame(webcam_fmt, &packet);
		if(ret < 0) {
			av_packet_unref(&packet);
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error getting a frame from the video device: %d (%s)\n",
				ret, av_err2str(ret));
			break;
		}
		ret = avcodec_send_packet(webcam_ctx, &packet);
		if(ret < 0) {
			av_packet_unref(&packet);
			if(ret == AVERROR(EAGAIN)) {
				/* Decoder needs more input? */
				continue;
			}
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error decoding frame from the video device: %d (%s)\n",
				ret, av_err2str(ret));
			break;
		}
		while(TRUE) {
			ret = avcodec_receive_frame(webcam_ctx, video_frame);
			if(ret < 0) {
				if(ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
					IMQUIC_LOG(IMQUIC_LOG_ERR, "Error decoding frame from the video device: %d (%s)\n",
						ret, av_err2str(ret));
				}
				break;
			}
			IMQUIC_LOG(IMQUIC_LOG_VERB, "Frame resolution: %dx%d\n",
				video_frame->width, video_frame->height);
			AVFrame *decode_frame = video_frame;
			if(imquic_demo_prepare_sw_decode_frame(video_frame, sw_video_frame, &decode_frame) < 0)
				continue;
			/* Convert the video frame to the right format and scale to ABR target */
			if(sws == NULL || scale_width != last_scale_width || scale_height != last_scale_height) {
				if(sws != NULL)
					sws_freeContext(sws);
				sws = sws_getContext(decode_frame->width, decode_frame->height, decode_frame->format,
					scale_width, scale_height, enc_target_pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
				last_scale_width = scale_width;
				last_scale_height = scale_height;
			}
			AVFrame *scaled_frame = av_frame_alloc();
			scaled_frame->width = scale_width;
			scaled_frame->height = scale_height;
			scaled_frame->format = enc_target_pix_fmt;
			ret = av_image_alloc(scaled_frame->data, scaled_frame->linesize,
				scaled_frame->width, scaled_frame->height, enc_target_pix_fmt, 1);
			if(ret < 0) {
				IMQUIC_LOG(IMQUIC_LOG_ERR, "Error allocating video frame: %d (%s)\n",
					ret, av_err2str(ret));
				av_freep(&scaled_frame->data[0]);
				av_frame_free(&scaled_frame);
				break;
			}
			sws_scale(sws, (const uint8_t * const*)decode_frame->data, decode_frame->linesize,
				0, decode_frame->height, scaled_frame->data, scaled_frame->linesize);
			/* Update the latest video frame, for the encoding thread */
			imquic_mutex_lock(&mutex);
			if(latest_frame != NULL) {
				av_freep(&latest_frame->data[0]);
				av_frame_free(&latest_frame);
			}
			latest_frame = scaled_frame;
			imquic_mutex_unlock(&mutex);
		}
		av_packet_unref(&packet);
	}
	av_frame_free(&sw_video_frame);
	av_frame_free(&video_frame);

	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving video capture thread\n");
	return NULL;
}

static gboolean imquic_demo_moq_send_video_packet(AVPacket *packet, int64_t wait,
		const moq_loc_svc_layer *layer_override, AVCodecContext *enc_ctx) {
	gboolean kf = (packet->flags & AV_PKT_FLAG_KEY);
	moq_loc_svc_layer layer = { 0 };
	gboolean svc = moq_loc_svc_is_svc_codec(codec);
	AVCodecContext *active_enc = enc_ctx != NULL ? enc_ctx : videoenc_ctx;
	if(svc) {
		gboolean avcc = (codec == DEMO_H264_SVC || codec == DEMO_H264_AVCC);
		if(layer_override != NULL) {
			layer = *layer_override;
			if(moq_loc_svc_parse_packet(codec, packet->data, packet->size, avcc, &layer) >= 0) {
				layer.spatial_id = layer_override->spatial_id;
				if(!layer_override->is_keyframe)
					layer.is_keyframe = kf;
			}
		} else if(moq_loc_svc_parse_packet(codec, packet->data, packet->size, avcc, &layer) < 0) {
			layer.temporal_id = 0;
			layer.spatial_id = 0;
			layer.is_keyframe = kf;
		} else if(!layer.is_keyframe) {
			layer.is_keyframe = kf;
		}
		if(!moq_loc_svc_layer_within_send_limits(&svc_cfg, &layer)) {
			IMQUIC_LOG(IMQUIC_LOG_VERB, "  -- Dropping SVC layer S%d T%d (max S=%d T=%d)\n",
				layer.spatial_id, layer.temporal_id,
				svc_cfg.max_send_spatial_layer, svc_cfg.max_send_temporal_layer);
			return FALSE;
		}
	}
	IMQUIC_LOG(IMQUIC_LOG_VERB, "  -- Encoded to %d bytes, %s%s\n",
		packet->size, kf ? "keyframe" : "NOT a keyframe",
		svc ? " (SVC)" : "");
	if(kf && (!svc || (layer.temporal_id == 0 && layer.spatial_id == 0))) {
		if(video_group_id > 0) {
			imquic_moq_object object = {
				.request_id = video_request_id,
				.track_alias = video_track_alias,
				.group_id = video_group_id,
				.subgroup_id = moq_loc_svc_is_svc_codec(codec) ?
					moq_loc_svc_subgroup_id(layer.spatial_id, layer.temporal_id) : 0,
				.object_id = video_object_id,
				.payload = NULL,
				.payload_len = 0,
				.properties = NULL,
				.delivery = IMQUIC_MOQ_USE_SUBGROUP,
				.end_of_stream = TRUE
			};
			imquic_demo_send_object(&object, TRUE);
		}
		video_group_id++;
		video_object_id = 0;
	}
	uint8_t *video_payload = packet->data;
	size_t video_payload_len = (size_t)packet->size;
	uint8_t *avcc_payload = NULL;
	if(codec == DEMO_H264_AVCC || codec == DEMO_H264_SVC) {
		size_t avcc_cap = (size_t)packet->size + 4096;
		avcc_payload = g_malloc(avcc_cap);
		size_t avcc_len = imquic_demo_h264_annexb_to_avcc_pack(packet->data, (size_t)packet->size,
			avcc_payload, avcc_cap);
		if(avcc_len == 0) {
			IMQUIC_LOG(IMQUIC_LOG_WARN, "Failed to convert Annex-B to AVCC, dropping frame\n");
			g_free(avcc_payload);
			return FALSE;
		}
		video_payload = avcc_payload;
		video_payload_len = avcc_len;
	}
	GList *props = NULL;
	imquic_moq_property timescale = { 0 };
	timescale.id = IMQUIC_MOQ_LOC_TIMESCALE;
	timescale.value.number = G_USEC_PER_SEC;
	props = g_list_append(props, &timescale);
	imquic_moq_property timestamp = { 0 };
	timestamp.id = IMQUIC_MOQ_LOC_TIMESTAMP;
	timestamp.value.number = video_ts;
	props = g_list_append(props, &timestamp);
	video_ts += wait;
	imquic_moq_property videoconfig = { 0 };
	imquic_moq_property frame_marking = { 0 };
	uint8_t avcc_data[1500];
	if(kf && active_enc != NULL && active_enc->extradata != NULL) {
		uint8_t *extradata = active_enc->extradata;
		size_t extradata_size = active_enc->extradata_size;
		if(codec == DEMO_H264_AVCC || codec == DEMO_H264_SVC) {
			size_t avcc_size = imquic_demo_h264_spspps_to_avcc(avcc_data, active_enc->extradata,
				active_enc->extradata_size);
			if(avcc_size > 0) {
				extradata = avcc_data;
				extradata_size = avcc_size;
			}
		}
		videoconfig.id = IMQUIC_MOQ_LOC_VIDEO_CONFIG;
		videoconfig.value.data.buffer = extradata;
		videoconfig.value.data.length = extradata_size;
		props = g_list_append(props, &videoconfig);
	}
	if(svc) {
		moq_loc_svc_set_frame_marking(&frame_marking, &layer);
		props = g_list_append(props, &frame_marking);
	}
	imquic_moq_object object = {
		.request_id = video_request_id,
		.track_alias = video_track_alias,
		.group_id = video_group_id,
		.subgroup_id = svc ? moq_loc_svc_subgroup_id(layer.spatial_id, layer.temporal_id) : 0,
		.object_id = video_object_id,
		.payload = video_payload,
		.payload_len = video_payload_len,
		.properties = props,
		.delivery = IMQUIC_MOQ_USE_SUBGROUP,
		.end_of_stream = FALSE
	};
	video_object_id++;
	imquic_demo_send_object(&object, TRUE);
	g_free(avcc_payload);
	g_list_free(props);
	return TRUE;
}

static void imquic_demo_drain_video_encoder_ctx(AVCodecContext *ctx, int64_t wait,
		const moq_loc_svc_layer *layer_override) {
	AVPacket packet = { 0 };
	if(ctx == NULL)
		return;
	while(TRUE) {
		int ret = avcodec_receive_packet(ctx, &packet);
		if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if(ret < 0) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error receiving encoded video packet: %d (%s)\n",
				ret, av_err2str(ret));
			break;
		}
		imquic_demo_moq_send_video_packet(&packet, wait, layer_override, ctx);
		av_packet_unref(&packet);
	}
}

static void imquic_demo_drain_video_encoder(int64_t wait) {
	if(moq_loc_svc_use_multi_spatial_encode(&svc_cfg)) {
		int s = 0;
		for(s = 0; s < svc_cfg.spatial_layers; s++) {
			moq_loc_svc_layer layer = { 0 };
			layer.spatial_id = (uint8_t)s;
			if(svc_spatial_enc[s] != NULL)
				imquic_demo_drain_video_encoder_ctx(svc_spatial_enc[s], wait, &layer);
		}
		return;
	}
	imquic_demo_drain_video_encoder_ctx(videoenc_ctx, wait, NULL);
}

static void imquic_demo_encode_spatial_layers(AVFrame *source, int64_t wait, gboolean force_kf) {
	int s = 0, ret = 0;
	if(source == NULL || !moq_loc_svc_use_multi_spatial_encode(&svc_cfg))
		return;
	for(s = 0; s < svc_cfg.spatial_layers; s++) {
		AVCodecContext *ctx = svc_spatial_enc[s];
		moq_loc_svc_layer layer_override = { 0 };
		AVFrame *scaled = NULL;
		if(ctx == NULL)
			continue;
		layer_override.spatial_id = (uint8_t)s;
		if(svc_spatial_sws[s] == NULL ||
				svc_spatial_enc_width[s] != ctx->width ||
				svc_spatial_enc_height[s] != ctx->height) {
			if(svc_spatial_sws[s] != NULL)
				sws_freeContext(svc_spatial_sws[s]);
			svc_spatial_sws[s] = sws_getContext(source->width, source->height, source->format,
				ctx->width, ctx->height, ctx->pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
		}
		if(svc_spatial_sws[s] == NULL)
			continue;
		scaled = av_frame_alloc();
		if(scaled == NULL)
			continue;
		scaled->format = ctx->pix_fmt;
		scaled->width = ctx->width;
		scaled->height = ctx->height;
		ret = av_frame_get_buffer(scaled, 32);
		if(ret < 0) {
			av_frame_free(&scaled);
			continue;
		}
		sws_scale(svc_spatial_sws[s], (const uint8_t * const*)source->data, source->linesize,
			0, source->height, scaled->data, scaled->linesize);
		if(force_kf && s == 0)
			scaled->pict_type = AV_PICTURE_TYPE_I;
		else
			scaled->pict_type = AV_PICTURE_TYPE_NONE;
		scaled->pts = video_input_pts;
		if(scaled->color_range == AVCOL_RANGE_UNSPECIFIED)
			scaled->color_range = AVCOL_RANGE_MPEG;
		ret = avcodec_send_frame(ctx, scaled);
		if(ret == AVERROR(EAGAIN)) {
			imquic_demo_drain_video_encoder_ctx(ctx, wait, &layer_override);
			ret = avcodec_send_frame(ctx, scaled);
		}
		av_frame_free(&scaled);
		if(ret < 0) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error encoding SVC spatial layer %d: %d (%s)\n",
				s, ret, av_err2str(ret));
			continue;
		}
		imquic_demo_drain_video_encoder_ctx(ctx, wait, &layer_override);
	}
}

static void *imquic_demo_video_enc_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting video encoding thread\n");

	int64_t now = 0, before = 0, wait = G_USEC_PER_SEC / options.video_framerate;
	int target_fps = options.video_framerate;

	while(!stop) {
		if(!g_atomic_int_get(&video_started)) {
			g_usleep(100000);
			continue;
		}

		if(abr != NULL) {
			moq_loc_abr_config cfg = { 0 };
			imquic_demo_apply_abr_video_config();
			moq_loc_abr_get_config(abr, &cfg);
			if(cfg.fps > 0)
				target_fps = cfg.fps;
		}
		if(target_fps <= 0)
			target_fps = 1;
		wait = G_USEC_PER_SEC / target_fps;

		now = g_get_monotonic_time();
		if(before == 0)
			before = now - wait;
		if((now - before) < (wait - 1000)) {
			usleep(1000);
			imquic_demo_drain_video_encoder(wait);
			continue;
		}
		before += wait;

		imquic_demo_drain_video_encoder(wait);

		imquic_mutex_lock(&mutex);
		if(latest_frame == NULL) {
			imquic_mutex_unlock(&mutex);
			continue;
		}
		if(!moq_loc_svc_use_multi_spatial_encode(&svc_cfg) && videoenc_ctx == NULL) {
			imquic_mutex_unlock(&mutex);
			continue;
		}
		if(latest_frame->width != enc_target_width || latest_frame->height != enc_target_height) {
			imquic_mutex_unlock(&mutex);
			continue;
		}
		AVFrame *encode_frame = av_frame_clone(latest_frame);
		imquic_mutex_unlock(&mutex);
		if(encode_frame == NULL) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Failed to clone video frame for encoding\n");
			continue;
		}
		gboolean force_kf = g_atomic_int_get(&force_video_keyframe) ? TRUE : FALSE;
		if(force_kf)
			g_atomic_int_set(&force_video_keyframe, 0);
		encode_frame->pts = video_input_pts++;
		if(moq_loc_svc_use_multi_spatial_encode(&svc_cfg)) {
			imquic_demo_encode_spatial_layers(encode_frame, wait, force_kf);
			av_frame_free(&encode_frame);
			continue;
		}
		if(force_kf)
			encode_frame->pict_type = AV_PICTURE_TYPE_I;
		else
			encode_frame->pict_type = AV_PICTURE_TYPE_NONE;
		if(encode_frame->color_range == AVCOL_RANGE_UNSPECIFIED)
			encode_frame->color_range = AVCOL_RANGE_MPEG;

		int ret = avcodec_send_frame(videoenc_ctx, encode_frame);
		if(ret == AVERROR(EAGAIN)) {
			imquic_demo_drain_video_encoder(wait);
			ret = avcodec_send_frame(videoenc_ctx, encode_frame);
		}
		av_frame_free(&encode_frame);
		if(ret < 0) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error encoding video frame: %d (%s)\n",
				ret, av_err2str(ret));
			continue;
		}
		imquic_demo_drain_video_encoder(wait);
	}

	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving video encoding thread\n");
	return NULL;
}

static void *imquic_demo_abr_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting ABR control thread\n");

	while(!stop) {
		if(moq_conn != NULL && g_atomic_int_get(&video_started)) {
			uint64_t ok = 0, fail = 0, bytes = 0;
			imquic_mutex_lock(&send_mutex);
			ok = send_ok_count;
			fail = send_fail_count;
			bytes = video_bytes_sent;
			imquic_mutex_unlock(&send_mutex);
			if(svc_abr != NULL && moq_loc_svc_is_svc_codec(codec) && svc_cfg.enabled) {
				moq_loc_svc_abr_update(svc_abr, moq_conn, ok, fail, -1.0);
				svc_cfg.max_send_temporal_layer = moq_loc_svc_abr_get_max_temporal_layer(svc_abr);
				svc_cfg.max_send_spatial_layer = moq_loc_svc_abr_get_max_spatial_layer(svc_abr);
			}
			if(abr != NULL)
				moq_loc_abr_update(abr, moq_conn, ok, fail, bytes);
		}
		g_usleep(500000);
	}

	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving ABR control thread\n");
	return NULL;
}

/* Callbacks */
static void imquic_demo_new_connection(imquic_connection *conn, void *user_data) {
	/* Got new connection */
	imquic_connection_ref(conn);
	moq_conn = conn;
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] New MoQ connection (configuring parameters)\n", imquic_get_connection_name(conn));
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]   -- %s (%s)\n", imquic_get_connection_name(conn),
		imquic_is_connection_webtransport(conn) ? "WebTransport" : "Raw QUIC",
		imquic_is_connection_webtransport(conn) ? imquic_get_connection_wt_protocol(conn) : imquic_get_connection_alpn(conn));
	imquic_moq_set_max_request_id(conn, max_request_id);
	if(!options.no_adaptive || (moq_loc_svc_is_svc_codec(codec) && svc_cfg.enabled))
		imquic_enable_connection_loss_feedback(conn);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Waiting for MoQ connection to be ready (SETUP)...\n",
		imquic_get_connection_name(conn));
}

static void imquic_demo_ready(imquic_connection *conn) {
	/* Negotiation was done */
	const char *peer = imquic_moq_get_remote_implementation(conn);
	moq_version = imquic_moq_get_version(conn);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] MoQ connection ready\n", imquic_get_connection_name(conn));
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]   -- %s\n", imquic_get_connection_name(conn),
		imquic_moq_version_str(moq_version));
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]   -- %s\n", imquic_get_connection_name(conn),
		peer ? peer : "unknown implementation");
	g_atomic_int_set(&connected, 1);
	/* Let's publish our namespace or publish right away */
	if(!options.publish) {
		/* We use PUBLISH_NAMESPACE + incoming SUBSCRIBE */
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Announcing namespace '%s'\n", imquic_get_connection_name(conn), pub_tns);
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- Will serve track '%s'\n", imquic_get_connection_name(conn), catalog_tn);
		if(options.audio_track_name != NULL)
			IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- Will serve track '%s'\n", imquic_get_connection_name(conn), audio_tn);
		if(options.video_track_name != NULL)
			IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- Will serve track '%s'\n", imquic_get_connection_name(conn), video_tn);
		moq_tns_request_id = imquic_moq_get_next_request_id(conn);
		imquic_moq_publish_namespace(conn, moq_tns_request_id, &pub_namespace[0], NULL);
	} else {
		/* We use PUBLISH */
		gboolean forward = FALSE;
		imquic_moq_request_parameters params;
		imquic_moq_request_parameters_init_defaults(&params);
		params.group_order_set = TRUE;
		params.group_order = IMQUIC_MOQ_ORDERING_ASCENDING;
		params.forward_set = TRUE;
		params.forward = forward;
		/* Catalog track */
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Publishing namespace/track '%s--%s'\n", imquic_get_connection_name(conn), pub_tns, catalog_tn);
		catalog_request_id = imquic_moq_get_next_request_id(conn);
		imquic_moq_publish(conn, catalog_request_id, &pub_namespace[0], &catalog_trackname, catalog_track_alias, &params, NULL);
		/* Audio track */
		if(options.audio_track_name != NULL) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Publishing namespace/track '%s--%s'\n", imquic_get_connection_name(conn), pub_tns, audio_tn);
			audio_request_id = imquic_moq_get_next_request_id(conn);
			imquic_moq_publish(conn, audio_request_id, &pub_namespace[0], &audio_trackname, audio_track_alias, &params, NULL);
		}
		/* Video track */
		if(options.video_track_name != NULL) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Publishing namespace/track '%s--%s'\n", imquic_get_connection_name(conn), pub_tns, video_tn);
			video_request_id = imquic_moq_get_next_request_id(conn);
			imquic_moq_publish(conn, video_request_id, &pub_namespace[0], &video_trackname, video_track_alias, &params, NULL);
		}
	}
}

static void imquic_demo_publish_namespace_accepted(imquic_connection *conn, uint64_t request_id, imquic_moq_request_parameters *parameters) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Publish Namespace '%"SCNu64"' accepted\n",
		imquic_get_connection_name(conn), request_id);
}

static void imquic_demo_publish_namespace_error(imquic_connection *conn, uint64_t request_id, imquic_moq_request_error_code error_code,
		const char *reason, uint64_t retry_interval, imquic_moq_redirect *redirect) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Got an error announcing namespace: error %d (%s)\n",
		imquic_get_connection_name(conn), error_code, reason);
	/* Stop here */
	g_atomic_int_inc(&stop);
}

static void imquic_demo_publish_accepted(imquic_connection *conn, uint64_t request_id, imquic_moq_request_parameters *parameters) {
	if(request_id == catalog_request_id) {
		/* Catalog track */
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Publish '%"SCNu64"' (catalog) accepted\n",
			imquic_get_connection_name(conn), request_id);
		g_atomic_int_set(&catalog_started, 1);
		return;
	}
	/* Audio or video */
	gboolean video = (options.video_track_name != NULL && request_id == video_request_id);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Publish '%"SCNu64"' (%s) accepted\n",
		imquic_get_connection_name(conn), request_id, video ? "video" : "audio");
	/* Start sending objects */
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- Starting delivery of %s objects\n",
		imquic_get_connection_name(conn), video ? "video" : "audio");
	if(video)
		g_atomic_int_set(&video_started, 1);
	else
		g_atomic_int_set(&audio_started, 1);
}

static void imquic_demo_publish_error(imquic_connection *conn, uint64_t request_id, imquic_moq_request_error_code error_code,
		const char *reason, uint64_t retry_interval, imquic_moq_redirect *redirect) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Got an error publishing with ID %"SCNu64": error %d (%s)\n",
		imquic_get_connection_name(conn), request_id, error_code, reason);
	/* Stop here */
	g_atomic_int_inc(&stop);
}

static void imquic_demo_incoming_subscribe(imquic_connection *conn, uint64_t request_id,
		imquic_moq_namespace *tns, imquic_moq_track *tn, imquic_moq_request_parameters *parameters) {
	char tns_buffer[256], tn_buffer[256];
	const char *ns = imquic_moq_namespace_str(tns, tns_buffer, sizeof(tns_buffer), TRUE);
	if(!strcasecmp(ns, ".2e")) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "[%s] Reserved namespace\n", imquic_get_connection_name(conn));
		imquic_moq_reject_subscribe(conn, request_id, IMQUIC_MOQ_REQERR_DOES_NOT_EXIST, "Reserved namespace", 0, NULL);
		return;
	}
	const char *name = imquic_moq_track_str(tn, tn_buffer, sizeof(tn_buffer));
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Incoming subscribe for '%s--%s' (ID %"SCNu64")\n",
		imquic_get_connection_name(conn), ns, name, request_id);
	if(pub_tns == NULL || strcasecmp(ns, pub_tns)) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "[%s] Unknown namespace\n", imquic_get_connection_name(conn));
		imquic_moq_reject_subscribe(conn, request_id, IMQUIC_MOQ_REQERR_DOES_NOT_EXIST, "Unknown namespace", 0, NULL);
		return;
	}
	if(!strcasecmp(name, catalog_tn)) {
		/* Catalog track, accept the subscription */
		catalog_request_id = request_id;
		imquic_moq_accept_subscribe(conn, request_id, catalog_track_alias, NULL, NULL);
		g_atomic_int_set(&catalog_started, 1);
		return;
	}
	/* Audio or video */
	gboolean is_audio = (audio_tn != NULL && !strcasecmp(name, audio_tn));
	gboolean is_video = (video_tn != NULL && !strcasecmp(name, video_tn));
	if(!is_audio && !is_video) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "[%s] Unknown track\n", imquic_get_connection_name(conn));
		imquic_moq_reject_subscribe(conn, request_id, IMQUIC_MOQ_REQERR_DOES_NOT_EXIST, "Unknown track", 0, NULL);
		return;
	}
	gboolean video = is_video;
	if(options.publish || (!video && g_atomic_int_get(&audio_started)) || (video && g_atomic_int_get(&video_started))) {
		/* FIXME In this demo, we only allow one subscriber at a time,
		 * as we expect a relay to mediate between us and subscribers */
		IMQUIC_LOG(IMQUIC_LOG_WARN, "[%s] We already have a %s subscriber\n",
			imquic_get_connection_name(conn), video ? "video" : "audio");
		imquic_moq_reject_subscribe(conn, request_id, IMQUIC_MOQ_REQERR_DUPLICATE_SUBSCRIPTION, "We already have a subscriber", 0, NULL);
		return;
	}
	/* TODO Check priority, filters, forwarding */
	if(parameters->group_order == IMQUIC_MOQ_ORDERING_DESCENDING) {
		/* We don't support descending mode yet */
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Descending group order unsupported, will send objects in ascending group order\n",
			imquic_get_connection_name(conn));
	}
	/* Check the filter */
	uint64_t filter_type = parameters->subscription_filter_set ?
		parameters->subscription_filter.type : IMQUIC_MOQ_FILTER_LARGEST_OBJECT;
	gboolean pub_started = g_atomic_int_get(video ? &video_started : &audio_started);
	static imquic_moq_location sub_start = { 0 }, sub_end = { 0 };
	sub_end.group = IMQUIC_MAX_VARINT;
	sub_end.object = IMQUIC_MAX_VARINT;
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- Requested filter type '%s'\n",
		imquic_get_connection_name(conn), imquic_moq_filter_type_str(filter_type));
	if(filter_type == IMQUIC_MOQ_FILTER_LARGEST_OBJECT) {
		sub_start.group = video ? video_group_id : audio_group_id;
		sub_start.object = video ? video_object_id : audio_object_id;
	} else if(filter_type == IMQUIC_MOQ_FILTER_NEXT_GROUP_START) {
		sub_start.group = (video ? video_group_id : audio_group_id) + 1;
		sub_start.object = 0;
	} else if(filter_type == IMQUIC_MOQ_FILTER_ABSOLUTE_START) {
		sub_start = parameters->subscription_filter.start_location;
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- -- Start location: [%"SCNu64"/%"SCNu64"]\n",
			imquic_get_connection_name(conn), sub_start.group, sub_start.object);
	} else if(filter_type == IMQUIC_MOQ_FILTER_ABSOLUTE_RANGE) {
		sub_start = parameters->subscription_filter.start_location;
		if(parameters->subscription_filter.end_group == 0)
			sub_end.group = IMQUIC_MAX_VARINT;
		else
			sub_end.group = parameters->subscription_filter.end_group - 1;
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- -- Start location: [%"SCNu64"/%"SCNu64"] --> End group [%"SCNu64"]\n",
			imquic_get_connection_name(conn), sub_start.group, sub_start.object, sub_end.group);
	}
	/* Accept the subscription */
	imquic_moq_request_parameters rparams;
	imquic_moq_request_parameters_init_defaults(&rparams);
	rparams.expires_set = TRUE;
	rparams.expires = 0;
	rparams.group_order_set = TRUE;
	rparams.group_order = IMQUIC_MOQ_ORDERING_ASCENDING;
	if(pub_started) {
		rparams.largest_object_set = TRUE;
		rparams.largest_object = sub_start;
	}
	imquic_moq_accept_subscribe(conn, request_id,
		video ? video_track_alias : audio_track_alias, &rparams, NULL);
	/* Start sending objects */
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- Starting delivery of %s objects: [%"SCNu64"/%"SCNu64"] --> [%"SCNu64"/%"SCNu64"]\n",
		imquic_get_connection_name(conn), video ? "video" : "audio",
		sub_start.group, sub_start.object, sub_end.group, sub_end.object);
	if(video) {
		video_request_id = request_id;
		video_sub_start = sub_start;
		video_sub_end = sub_end;
		g_atomic_int_set(&video_started, 1);
	} else {
		audio_request_id = request_id;
		audio_sub_start = sub_start;
		audio_sub_end = sub_end;
		g_atomic_int_set(&audio_started, 1);
	}
}

static void imquic_demo_incoming_unsubscribe(imquic_connection *conn, uint64_t request_id) {
	gboolean video = (options.video_track_name != NULL && request_id == video_request_id);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Incoming unsubscribe for %s subscription %"SCNu64"\n",
		imquic_get_connection_name(conn), video ? "video" : "audio", request_id);
	/* Stop sending objects */
	if(video)
		g_atomic_int_set(&video_started, 0);
	else
		g_atomic_int_set(&audio_started, 0);
}

static void imquic_demo_incoming_go_away(imquic_connection *conn, const char *uri, uint64_t timeout) {
	/* Connection was closed */
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Got a GOAWAY: %s (timeout=%"SCNu64"ms)\n",
		imquic_get_connection_name(conn), uri, timeout);
	/* Stop here */
	g_atomic_int_inc(&stop);
}

static void imquic_demo_connection_failed(void *user_data) {
	/* Connection failed */
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Connection failed\n");
	/* Stop here */
	g_atomic_int_inc(&stop);
}

static void imquic_demo_connection_gone(imquic_connection *conn, uint64_t error_code, const char *reason) {
	/* Connection was closed */
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] MoQ connection gone\n", imquic_get_connection_name(conn));
	if(conn == moq_conn)
		imquic_connection_unref(conn);
	moq_conn = NULL;
	/* Stop here */
	g_atomic_int_inc(&stop);
}

int main(int argc, char *argv[]) {
	int ret = 0;

	/* Handle SIGINT (CTRL-C), SIGTERM (from service managers) */
	signal(SIGINT, imquic_demo_handle_signal);
	signal(SIGTERM, imquic_demo_handle_signal);

	IMQUIC_PRINT("imquic version %s\n", imquic_get_version_string_full());
	IMQUIC_PRINT("  -- %s (commit hash)\n", imquic_get_build_sha());
	IMQUIC_PRINT("  -- %s (build time)\n\n", imquic_get_build_time());

	/* Initialize some command line options defaults */
	options.debug_level = IMQUIC_LOG_INFO;
	/* Let's call our cmdline parser */
	if(!demo_options_parse(&options, argc, argv)) {
		demo_options_show_usage();
		demo_options_destroy();
		exit(1);
	}
	/* Logging level */
	imquic_set_log_level(options.debug_level);
	/* Debugging */
	if(options.debug_locks)
		imquic_set_lock_debugging(TRUE);
	if(options.debug_refcounts)
		imquic_set_refcount_debugging(TRUE);

	/* Initialize SDL backends */
	if(SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO) < 0) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Error initializing SDL2: %s\n", SDL_GetError());
		ret = 1;
		goto done;
	}

	/* FFmpeg initialization */
#if (LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100))
	av_register_all();
#endif
	avdevice_register_all();
	if(options.debug_ffmpeg)
		av_log_set_level(AV_LOG_DEBUG);

	/* Parse the command line arguments*/
	if(options.remote_host == NULL || options.remote_port == 0) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid QUIC server address\n");
		ret = 1;
		goto done;
	}
	if(options.port > 65535) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid port\n");
		ret = 1;
		goto done;
	}
	if(!options.raw_quic && !options.webtransport) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "No raw QUIC or WebTransport enabled (enable at least one)\n");
		ret = 1;
		goto done;
	}
	if(options.ticket_file != NULL)
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Early data support enabled (ticket file '%s')\n", options.ticket_file);
	if(options.moq_version != NULL) {
		if(!strcasecmp(options.moq_version, "any")) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Negotiating version of MoQ between %d and %d\n",
				IMQUIC_MOQ_VERSION_MIN - IMQUIC_MOQ_VERSION_BASE, IMQUIC_MOQ_VERSION_MAX - IMQUIC_MOQ_VERSION_BASE);
			moq_version = IMQUIC_MOQ_VERSION_ANY;
		} else {
			moq_version = IMQUIC_MOQ_VERSION_BASE + atoi(options.moq_version);
			if(moq_version < IMQUIC_MOQ_VERSION_MIN || moq_version > IMQUIC_MOQ_VERSION_MAX) {
				IMQUIC_LOG(IMQUIC_LOG_FATAL, "Unsupported MoQ version %s\n", options.moq_version);
				ret = 1;
				goto done;
			}
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Negotiating version of MoQ %d\n", moq_version - IMQUIC_MOQ_VERSION_BASE);
		}
	}

	if(options.track_namespace == NULL || options.track_namespace[0] == NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Missing track namespace(s)\n");
		ret = 1;
		goto done;
	}
	int i = 0;
	while(options.track_namespace[i] != NULL) {
		const char *track_namespace = options.track_namespace[i];
		pub_namespace[i].buffer = (uint8_t *)track_namespace;
		pub_namespace[i].length = strlen(track_namespace);
		pub_namespace[i].next = (options.track_namespace[i+1] != NULL) ? &pub_namespace[i+1] : NULL;
		i++;
	}
	uint64_t tns_num = 0;
	if(!imquic_moq_namespace_is_valid(&pub_namespace[0], TRUE, &tns_num)) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid track namespace\n");
		ret = 1;
		goto done;
	}
	pub_tns = imquic_moq_namespace_str(pub_namespace, pub_tns_buffer, sizeof(pub_tns_buffer), TRUE);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Using namespace '%s' (%"SCNu64" tuples)\n", pub_tns, tns_num);

	/* Create a catalog track */
	catalog_trackname.buffer = (uint8_t *)catalog_tn;
	catalog_trackname.length = strlen(catalog_tn);
	catalog = imquic_moq_catalog_create("draft-01");
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Using track name '%s' for catalog\n", catalog_tn);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will use track_alias=%"SCNu64"\n",
		catalog_track_alias);

	if(options.audio_track_name == NULL && options.video_track_name == NULL) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Missing track name(s)\n");
		ret = 1;
		goto done;
	}
	if(options.audio_track_name != NULL) {
		/* Create an audio track */
		audio_trackname.buffer = (uint8_t *)options.audio_track_name;
		audio_trackname.length = strlen(options.audio_track_name);
		if(!imquic_moq_track_is_valid(&audio_trackname)) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid audio track name\n");
			ret = 1;
			goto done;
		}
		audio_tn = imquic_moq_track_str(&audio_trackname, audio_tn_buffer, sizeof(audio_tn_buffer));
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Using track name '%s' for audio\n", audio_tn);
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will use track_alias=%"SCNu64"\n",
			audio_track_alias);
		if(options.audio_bitrate <= 0)
			options.audio_bitrate = 32000;
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will encode audio at %d bps\n", options.audio_bitrate);
		/* Add the audio track to the catalog */
		imquic_moq_catalog_track *track = imquic_moq_catalog_create_track(pub_tns, audio_tn, "loc", TRUE);
		track->role = g_strdup("audio");
		track->render_group = 1;
		track->target_latency = 200;
		track->codec = g_strdup("opus");
		track->samplerate = 48000;
		track->channel_config = g_strdup("1");
		track->bitrate = options.audio_bitrate;
		imquic_moq_catalog_add_track(catalog, track);
	}
	if(options.video_track_name != NULL) {
		/* Create a video track */
		video_trackname.buffer = (uint8_t *)options.video_track_name;
		video_trackname.length = strlen(options.video_track_name);
		if(!imquic_moq_track_is_valid(&video_trackname)) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid video track name\n");
			ret = 1;
			goto done;
		}
		video_tn = imquic_moq_track_str(&video_trackname, video_tn_buffer, sizeof(video_tn_buffer));
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Using track name '%s' for video\n", video_tn);
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will use track_alias=%"SCNu64"\n",
			video_track_alias);
		if(options.video_format == NULL)
			options.video_format = "v4l2";
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will use the '%s' video format\n", options.video_format);
		if(options.video_device == NULL)
			options.video_device = "/dev/video0";
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will use the '%s' video device\n", options.video_device);
		if(options.video_resolution == NULL)
			options.video_resolution = "640x480";
		if(sscanf(options.video_resolution, "%dx%d", &options.width, &options.height) != 2 ||
				options.width <= 0 || options.height <= 0) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid video resolution\n");
			ret = 1;
			goto done;
		}
		if(options.video_framerate <= 0)
			options.video_framerate = 25;
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will capture video at '%dx%d@%d'\n",
			options.width, options.height, options.video_framerate);
		if(options.video_bitrate <= 0)
			options.video_bitrate = 1000 * 1024;
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will encode video at %d bps\n", options.video_bitrate);
		if(options.video_codec != NULL) {
			codec = imquic_demo_video_codec_from_str(options.video_codec);
			if(codec == DEMO_UNKOWN) {
				IMQUIC_LOG(IMQUIC_LOG_FATAL, "Unsupported video codec '%s'\n", options.video_codec);
				ret = 1;
				goto done;
			}
		}
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Will use video codec '%s'\n",
			imquic_demo_video_codec_str(codec));
		if(!imquic_demo_any_video_encoder_available(codec)) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Video codec '%s' not supported in provided libavcodec\n",
				imquic_demo_video_codec_str(codec));
			ret = 1;
			goto done;
		}
		if(moq_loc_svc_is_svc_codec(codec)) {
			moq_loc_svc_config_init(&svc_cfg);
			svc_cfg.enabled = TRUE;
			svc_cfg.codec = codec;
			if(options.svc_temporal_layers > 0)
				svc_cfg.temporal_layers = options.svc_temporal_layers;
			if(options.svc_spatial_layers > 0)
				svc_cfg.spatial_layers = options.svc_spatial_layers;
			svc_cfg.max_send_temporal_layer = svc_cfg.temporal_layers - 1;
			svc_cfg.max_send_spatial_layer = svc_cfg.spatial_layers - 1;
			if(!moq_loc_svc_config_validate(&svc_cfg)) {
				IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid SVC configuration\n");
				ret = 1;
				goto done;
			}
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- SVC enabled: %d temporal layer(s), %d spatial layer(s)\n",
				svc_cfg.temporal_layers, svc_cfg.spatial_layers);
		}
		/* Add the video track to the catalog */
		imquic_moq_catalog_track *track = imquic_moq_catalog_create_track(pub_tns, video_tn, "loc", TRUE);
		track->role = g_strdup("video");
		track->render_group = 1;
		track->target_latency = 200;
		if(codec == DEMO_H264_AVCC)
			track->codec = g_strdup("avc1.42001F");
		else if(codec == DEMO_H264_ANNEXB)	/* FIXME */
			track->codec = g_strdup("annexb.42001F");
		else if(codec == DEMO_H264_SVC) {
			const char *svc_codec = moq_loc_svc_catalog_codec(codec);
			track->codec = g_strdup(svc_codec ? svc_codec : "avc1.534015");
			track->temporal_id = (uint8_t)(svc_cfg.temporal_layers - 1);
			track->spatial_id = (uint8_t)(svc_cfg.spatial_layers - 1);
		} else if(codec == DEMO_VP8)
			track->codec = g_strdup("vp8");
		else if(codec == DEMO_VP9)	/* FIXME */
			track->codec = g_strdup("vp9");
		else if(codec == DEMO_VP9_SVC) {
			const char *svc_codec = moq_loc_svc_catalog_codec(codec);
			track->codec = g_strdup(svc_codec ? svc_codec : "vp9.svc");
			track->temporal_id = (uint8_t)(svc_cfg.temporal_layers - 1);
			track->spatial_id = (uint8_t)(svc_cfg.spatial_layers - 1);
		} else if(codec == DEMO_AV1)	/* FIXME */
			track->codec = g_strdup("av1");
		track->width = options.width;
		track->height = options.height;
		track->framerate = options.video_framerate;
		track->bitrate = options.video_bitrate;
		imquic_moq_catalog_add_track(catalog, track);
	}

	if(options.video_track_name != NULL && !options.no_adaptive && !moq_loc_svc_is_svc_codec(codec) &&
			(!options.no_adaptive_resolution || !options.no_adaptive_bitrate || !options.no_adaptive_framerate)) {
		gboolean adapt_resolution = !options.no_adaptive_resolution;
		gboolean adapt_bitrate = !options.no_adaptive_bitrate;
		gboolean adapt_framerate = !options.no_adaptive_framerate;
		abr = moq_loc_abr_create(options.width, options.height, options.video_framerate,
			options.video_bitrate, options.audio_bitrate > 0 ? options.audio_bitrate : 32000);
		moq_loc_abr_set_adapt_flags(abr, adapt_resolution, adapt_bitrate, adapt_framerate);
		applied_enc_generation = moq_loc_abr_config_generation(abr);
		applied_audio_bitrate = options.audio_bitrate > 0 ? options.audio_bitrate : 32000;
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Adaptive streaming enabled (targets: RTT<=150ms, jitter<=50ms, loss<=50%%)\n");
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Max quality: %dx%d@%d, %d bps video, %d bps audio\n",
			options.width, options.height, options.video_framerate,
			options.video_bitrate, applied_audio_bitrate);
		IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Adapt: resolution=%s, bitrate=%s, framerate=%s\n",
			adapt_resolution ? "on" : "off", adapt_bitrate ? "on" : "off",
			adapt_framerate ? "on" : "off");
	} else if(moq_loc_svc_is_svc_codec(codec) && !options.no_adaptive) {
		svc_abr = moq_loc_svc_abr_create(svc_cfg.temporal_layers, svc_cfg.spatial_layers);
		IMQUIC_LOG(IMQUIC_LOG_INFO,
			"SVC adaptive layer selection enabled (temporal + spatial, targets: RTT<=150ms, jitter<=50ms, loss<=50%%)\n");
	}

	if(options.publish)
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Will use PUBLISH instead of PUBLISH_NAMESPACE + SUBSCRIBE\n");

	/* Check if we need to create a QLOG file, and which we should save */
	gboolean qlog_quic = FALSE, qlog_http3 = FALSE, qlog_moq = FALSE;
	if(options.qlog_path != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Creating QLOG file(s) in '%s'\n", options.qlog_path);
		if(options.qlog_sequential)
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Using sequential JSON\n");
		int i = 0;
		while(options.qlog_logging != NULL && options.qlog_logging[i] != NULL) {
			if(!strcasecmp(options.qlog_logging[i], "quic")) {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Logging QUIC events\n");
				qlog_quic = TRUE;
			} else if(!strcasecmp(options.qlog_logging[i], "http3") && options.webtransport) {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Logging HTTP/3 events\n");
				qlog_http3 = TRUE;
			} else if(!strcasecmp(options.qlog_logging[i], "moq")) {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Logging MoQT events\n");
				qlog_moq = TRUE;
				if(options.qlog_moq_messages)
					IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- -- Logging the payload of MoQT control messages\n");
				if(options.qlog_moq_objects)
					IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- -- Logging the payload of MoQT objects\n");
			}
			i++;
		}
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "\n");

	/* Initialize the library and create a client */
	if(imquic_init(options.secrets_log) < 0) {
		ret = 1;
		goto done;
	}

	/* Create encoders */
	if(options.video_encode_device != NULL)
		imquic_demo_set_v4l2_encode_device(options.video_encode_device);
	if(options.audio_track_name != NULL && imquic_demo_create_audio_encoder() < 0) {
		g_atomic_int_set(&stop, 1);
		goto done;
	}
	if(options.video_track_name != NULL && imquic_demo_create_video_encoder() < 0) {
		g_atomic_int_set(&stop, 1);
		goto done;
	}
	if(abr != NULL || svc_abr != NULL) {
		GError *error = NULL;
		abr_thread = g_thread_try_new("loc-abr", &imquic_demo_abr_thread, NULL, &error);
		if(error != NULL) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start ABR thread\n",
				error->code, error->message ? error->message : "??");
			g_atomic_int_set(&stop, 1);
			goto done;
		}
	}

	/* Create a client endpoint */
	imquic_server *client = imquic_create_moq_client("moq-loc-send",
		IMQUIC_CONFIG_INIT,
		IMQUIC_CONFIG_TLS_CERT, options.cert_pem,
		IMQUIC_CONFIG_TLS_KEY, options.cert_key,
		IMQUIC_CONFIG_TLS_NO_VERIFY, TRUE,
		IMQUIC_CONFIG_LOCAL_BIND, options.ip,
		IMQUIC_CONFIG_LOCAL_PORT, options.port,
		IMQUIC_CONFIG_REMOTE_HOST, options.remote_host,
		IMQUIC_CONFIG_REMOTE_PORT, options.remote_port,
		IMQUIC_CONFIG_SNI, options.sni,
		IMQUIC_CONFIG_RAW_QUIC, options.raw_quic,
		IMQUIC_CONFIG_WEBTRANSPORT, options.webtransport,
		IMQUIC_CONFIG_EARLY_DATA, (options.ticket_file != NULL),
		IMQUIC_CONFIG_TICKET_FILE, options.ticket_file,
		IMQUIC_CONFIG_HTTP3_PATH, options.path,
		IMQUIC_CONFIG_QLOG_PATH, options.qlog_path,
		IMQUIC_CONFIG_QLOG_QUIC, qlog_quic,
		IMQUIC_CONFIG_QLOG_HTTP3, qlog_http3,
		IMQUIC_CONFIG_QLOG_MOQ, qlog_moq,
		IMQUIC_CONFIG_QLOG_MOQ_MESSAGES, options.qlog_moq_messages,
		IMQUIC_CONFIG_QLOG_MOQ_OBJECTS, options.qlog_moq_objects,
		IMQUIC_CONFIG_QLOG_SEQUENTIAL, options.qlog_sequential,
		IMQUIC_CONFIG_MOQ_VERSION, moq_version,
		IMQUIC_CONFIG_MOQ_GREASE, options.test_grease,
		IMQUIC_CONFIG_CC_ALGO, options.cc_algo,
		IMQUIC_CONFIG_CC_OPTION, options.cc_algo_option,
		IMQUIC_CONFIG_DONE, NULL);
	if(client == NULL) {
		ret = 1;
		goto done;
	}
	if(options.raw_quic) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "ALPN(s):\n");
		int i = 0;
		const char **alpns = imquic_get_endpoint_alpns(client);
		while(alpns[i] != NULL) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- %s\n", alpns[i]);
			i++;
		}
	}
	if(options.webtransport && imquic_get_endpoint_wt_protocols(client) != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "WebTransport Protocol(s):\n");
		int i = 0;
		const char **wt_protocols = imquic_get_endpoint_wt_protocols(client);
		while(wt_protocols[i] != NULL) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- %s\n", wt_protocols[i]);
			i++;
		}
	}
	imquic_set_new_moq_connection_cb(client, imquic_demo_new_connection);
	imquic_set_moq_ready_cb(client, imquic_demo_ready);
	imquic_set_publish_namespace_accepted_cb(client, imquic_demo_publish_namespace_accepted);
	imquic_set_publish_namespace_error_cb(client, imquic_demo_publish_namespace_error);
	imquic_set_publish_accepted_cb(client, imquic_demo_publish_accepted);
	imquic_set_publish_error_cb(client, imquic_demo_publish_error);
	imquic_set_incoming_subscribe_cb(client, imquic_demo_incoming_subscribe);
	imquic_set_incoming_unsubscribe_cb(client, imquic_demo_incoming_unsubscribe);
	imquic_set_incoming_goaway_cb(client, imquic_demo_incoming_go_away);
	imquic_set_connection_failed_cb(client, imquic_demo_connection_failed);
	imquic_set_moq_connection_gone_cb(client, imquic_demo_connection_gone);
	imquic_start_endpoint(client);

	while(!stop) {
		if(g_atomic_int_compare_and_exchange(&catalog_started, 1, 2)) {
			/* Send the catalog */
			char *json = imquic_moq_catalog_serialize(catalog);
			if(json != NULL) {
				imquic_moq_object object = {
					.request_id = catalog_request_id,
					.track_alias = catalog_track_alias,
					.group_id = 0,	/* FIXME */
					.subgroup_id = 0,
					.object_id = 0,
					.payload = (uint8_t *)json,
					.payload_len = strlen(json),
					.delivery = IMQUIC_MOQ_USE_SUBGROUP,
					.end_of_stream = TRUE
				};
				imquic_demo_send_object(&object, FALSE);
				g_free(json);
			}
		}
		g_usleep(100000);
	}

	/* We're done, check if we need to send a PUBLISH_DONE and/or an PUBLISH_NAMESPACE_DONE */
	if(g_atomic_int_get(&catalog_started) && !g_atomic_int_get(&catalog_done)) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Sending PUBLISH_DONE for catalog\n");
		imquic_moq_publish_done(moq_conn, catalog_request_id, IMQUIC_MOQ_PUBDONE_SUBSCRIPTION_ENDED, "Publisher left");
	}
	if(g_atomic_int_get(&audio_started) && !g_atomic_int_get(&audio_done)) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Sending PUBLISH_DONE for audio\n");
		imquic_moq_publish_done(moq_conn, audio_request_id, IMQUIC_MOQ_PUBDONE_SUBSCRIPTION_ENDED, "Publisher left");
	}
	if(g_atomic_int_get(&video_started) && !g_atomic_int_get(&video_done)) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Sending PUBLISH_DONE for video\n");
		imquic_moq_publish_done(moq_conn, video_request_id, IMQUIC_MOQ_PUBDONE_SUBSCRIPTION_ENDED, "Publisher left");
	}
	if(!options.publish) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Sending PUBLISH_NAMESPACE_DONE\n");
		imquic_moq_publish_namespace_done(moq_conn, moq_tns_request_id);
	}
	/* Shutdown the client */
	imquic_shutdown_endpoint(client);

done:
	imquic_deinit();
	if(ret == 1)
		demo_options_show_usage();
	demo_options_destroy();

	/* Decoder stuff */
	imquic_moq_catalog_destroy(catalog);
	if(audio_thread != NULL)
		g_thread_join(audio_thread);
	if(video_capture_thread != NULL)
		g_thread_join(video_capture_thread);
	if(video_enc_thread != NULL)
		g_thread_join(video_enc_thread);
	if(abr_thread != NULL)
		g_thread_join(abr_thread);
	if(abr != NULL)
		moq_loc_abr_destroy(abr);
	if(svc_abr != NULL)
		moq_loc_svc_abr_destroy(svc_abr);
	abr = NULL;
	if(latest_frame != NULL) {
		av_freep(&latest_frame->data[0]);
		av_frame_free(&latest_frame);
	}
	avformat_network_deinit();
	imquic_demo_destroy_audio_encoder();
	imquic_demo_destroy_video_encoder();

	/* SDL stuff */
	SDL_Quit();

	/* Done */
	IMQUIC_PRINT("Bye!\n");
	exit(ret);
}
