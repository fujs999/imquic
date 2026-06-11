/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Audio/video playback for imquic-roq-receiver
 *
 */

#include <arpa/inet.h>
#include <string.h>

#include <imquic/imquic.h>

#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>

#include <opus/opus.h>

#include <SDL2/SDL.h>

#include "roq-display.h"
#include "roq-utils.h"

#define ROQ_DISPLAY_ANNEXB_MAX (512 * 1024)
#define ROQ_DISPLAY_AUDIO_RATE 48000
#define ROQ_DISPLAY_AUDIO_MAX_SAMPLES 1920
#define ROQ_DISPLAY_AUDIO_MAX_QUEUE 10000

typedef struct roq_h264_depay {
	GByteArray *access_unit;
	GByteArray *fu_buffer;
	uint16_t last_seq;
	uint32_t last_ts;
	gboolean in_fu;
} roq_h264_depay;

static roq_display_config cfg = { 0 };
static roq_h264_depay depay = { 0 };

static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static int screen_w = 1280, screen_h = 720;
static int texture_w = -1, texture_h = -1;

static OpusDecoder *audiodec = NULL;
static SDL_AudioDeviceID audio_dev = 0;

static AVCodecContext *videodec_ctx = NULL;
static const AVCodec *video_codec = NULL;
static gboolean got_keyframe = FALSE;
static AVFrame *latest_frame = NULL;
static imquic_mutex frame_mutex = IMQUIC_MUTEX_INITIALIZER;
static uint32_t last_render_tick = 0;
static gboolean sdl_video_inited = FALSE, sdl_audio_inited = FALSE;

static void roq_display_reset_depay(void) {
	if(depay.access_unit != NULL)
		g_byte_array_set_size(depay.access_unit, 0);
	if(depay.fu_buffer != NULL)
		g_byte_array_set_size(depay.fu_buffer, 0);
	depay.in_fu = FALSE;
}

static void roq_display_append_nal(GByteArray *au, const uint8_t *nal, size_t nal_len) {
	static const uint8_t start_code[] = { 0x00, 0x00, 0x00, 0x01 };
	if(au == NULL || nal == NULL || nal_len == 0)
		return;
	g_byte_array_append(au, start_code, sizeof(start_code));
	g_byte_array_append(au, nal, (guint)nal_len);
}

static gboolean roq_display_annexb_is_keyframe(const uint8_t *data, size_t len) {
	size_t i = 0;
	while(i + 4 < len) {
		if(data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
			uint8_t nal_type = data[i + 4] & 0x1F;
			if(nal_type == 5 || nal_type == 7)
				return TRUE;
			i += 4;
		} else {
			i++;
		}
	}
	return FALSE;
}

static size_t roq_display_rtp_payload_offset(uint8_t *rtp, size_t rtp_len, size_t *payload_len) {
	if(rtp == NULL || rtp_len < 12)
		return 0;
	imquic_roq_rtp_header *header = (imquic_roq_rtp_header *)rtp;
	size_t offset = 12 + header->csrccount * 4;
	if(header->extension) {
		if(rtp_len < offset + 4)
			return 0;
		uint16_t ext_words = ntohs(*(uint16_t *)(rtp + offset + 2));
		offset += 4 + ext_words * 4;
	}
	if(offset > rtp_len)
		return 0;
	size_t end = rtp_len;
	if(header->padding && end > offset) {
		uint8_t padding = rtp[end - 1];
		if(padding > 0 && padding <= end - offset)
			end -= padding;
	}
	if(payload_len != NULL)
		*payload_len = end - offset;
	return offset;
}

static gboolean roq_display_depay_h264(const uint8_t *payload, size_t payload_len,
		gboolean marker, uint16_t seq, uint32_t timestamp, gboolean *complete) {
	if(payload == NULL || payload_len == 0 || complete == NULL)
		return FALSE;
	*complete = FALSE;
	if(depay.access_unit == NULL)
		depay.access_unit = g_byte_array_new();
	if(depay.fu_buffer == NULL)
		depay.fu_buffer = g_byte_array_new();

	if(depay.last_ts != timestamp) {
		roq_display_reset_depay();
		depay.last_ts = timestamp;
	} else if((uint16_t)(depay.last_seq + 1) != seq) {
		roq_display_reset_depay();
	}
	depay.last_seq = seq;

	uint8_t nal_type = payload[0] & 0x1F;
	if(nal_type >= 1 && nal_type <= 23) {
		roq_display_append_nal(depay.access_unit, payload, payload_len);
	} else if(nal_type == 28 || nal_type == 29) {
		if(payload_len < 2)
			return FALSE;
		uint8_t fu_indicator = payload[0];
		uint8_t fu_header = payload[1];
		gboolean start = (fu_header & 0x80) != 0;
		gboolean end = (fu_header & 0x40) != 0;
		if(start) {
			g_byte_array_set_size(depay.fu_buffer, 0);
			uint8_t nal_header = (uint8_t)((fu_indicator & 0xE0) | (fu_header & 0x1F));
			g_byte_array_append(depay.fu_buffer, &nal_header, 1);
			g_byte_array_append(depay.fu_buffer, payload + 2, (guint)(payload_len - 2));
			depay.in_fu = TRUE;
		} else if(depay.in_fu) {
			g_byte_array_append(depay.fu_buffer, payload + 2, (guint)(payload_len - 2));
		} else {
			return FALSE;
		}
		if(end) {
			roq_display_append_nal(depay.access_unit, depay.fu_buffer->data, depay.fu_buffer->len);
			g_byte_array_set_size(depay.fu_buffer, 0);
			depay.in_fu = FALSE;
		}
	} else if(nal_type == 24) {
		size_t offset = 1;
		while(offset + 2 <= payload_len) {
			uint16_t psize = 0;
			memcpy(&psize, payload + offset, 2);
			psize = ntohs(psize);
			offset += 2;
			if(psize == 0 || offset + psize > payload_len)
				break;
			roq_display_append_nal(depay.access_unit, payload + offset, psize);
			offset += psize;
		}
	} else {
		return FALSE;
	}

	if(marker && depay.access_unit->len > 0) {
		*complete = TRUE;
	}
	return TRUE;
}

static int roq_display_create_audio_decoder(void) {
	int opus_error = 0;
	audiodec = opus_decoder_create(ROQ_DISPLAY_AUDIO_RATE, 1, &opus_error);
	if(opus_error != OPUS_OK) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening audio decoder\n");
		return -1;
	}
	return 0;
}

static int roq_display_create_audio_output(void) {
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = ROQ_DISPLAY_AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = 960;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if(!audio_dev) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening audio device: %s\n", SDL_GetError());
		return -1;
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Opened audio device: %"SCNu16" Hz, %"SCNu8" channels, %"SCNu16" samples\n",
		have.freq, have.channels, have.samples);
	SDL_PauseAudioDevice(audio_dev, 0);
	return 0;
}

static int roq_display_decode_audio(const uint8_t *payload, size_t payload_len) {
	if(audiodec == NULL || audio_dev == 0 || payload == NULL || payload_len == 0)
		return -1;
	opus_int16 samples[ROQ_DISPLAY_AUDIO_MAX_SAMPLES];
	int ret = opus_decode(audiodec, payload, (opus_int32)payload_len,
		samples, ROQ_DISPLAY_AUDIO_MAX_SAMPLES, 0);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error decoding audio frame: %d (%s)\n", ret, opus_strerror(ret));
		return -1;
	}
	Uint32 queued = SDL_GetQueuedAudioSize(audio_dev);
	if(queued >= ROQ_DISPLAY_AUDIO_MAX_QUEUE)
		SDL_ClearQueuedAudio(audio_dev);
	SDL_QueueAudio(audio_dev, (uint8_t *)samples, (Uint32)(ret * 2));
	return 0;
}

static int roq_display_create_decoder(void) {
	if(video_codec == NULL)
		return -1;
	if(videodec_ctx != NULL)
		avcodec_free_context(&videodec_ctx);
	videodec_ctx = avcodec_alloc_context3(video_codec);
	if(videodec_ctx == NULL)
		return -1;
	if(avcodec_open2(videodec_ctx, video_codec, NULL) < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening video decoder\n");
		avcodec_free_context(&videodec_ctx);
		videodec_ctx = NULL;
		return -1;
	}
	return 0;
}

static int roq_display_decode_access_unit(uint8_t *buffer, size_t length, gboolean keyframe) {
	if(videodec_ctx == NULL)
		return -1;
	if(!got_keyframe) {
		if(!keyframe) {
			IMQUIC_LOG(IMQUIC_LOG_VERB, "Still waiting for a video keyframe, skipping this frame...\n");
			return 0;
		}
		got_keyframe = TRUE;
	}
	AVPacket packet = { 0 };
	av_new_packet(&packet, (int)length);
	memcpy(packet.data, buffer, length);
	if(keyframe)
		packet.flags |= AV_PKT_FLAG_KEY;
	int ret = avcodec_send_packet(videodec_ctx, &packet);
	av_packet_unref(&packet);
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error decoding video frame: %d (%s)\n", ret, av_err2str(ret));
		return -1;
	}
	AVFrame *decoded_frame = av_frame_alloc();
	ret = avcodec_receive_frame(videodec_ctx, decoded_frame);
	if(ret == AVERROR(EAGAIN)) {
		av_frame_free(&decoded_frame);
		return 0;
	}
	if(ret < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error decoding video frame: %d (%s)\n", ret, av_err2str(ret));
		av_frame_free(&decoded_frame);
		return -1;
	}
	imquic_mutex_lock(&frame_mutex);
	if(latest_frame != NULL)
		av_frame_free(&latest_frame);
	latest_frame = decoded_frame;
	imquic_mutex_unlock(&frame_mutex);
	return 0;
}

static void roq_display_feed_video(uint64_t flow_id, imquic_roq_rtp_header *header,
		uint8_t *rtp, size_t rtp_len) {
	if(videodec_ctx == NULL)
		return;
	if(cfg.video_flow >= 0 && (int64_t)flow_id != cfg.video_flow)
		return;
	if(cfg.video_pt != 255 && header->type != cfg.video_pt)
		return;
	size_t payload_len = 0;
	size_t payload_offset = roq_display_rtp_payload_offset(rtp, rtp_len, &payload_len);
	if(payload_offset == 0 || payload_len == 0)
		return;
	const uint8_t *payload = rtp + payload_offset;
	gboolean complete = FALSE;
	if(!roq_display_depay_h264(payload, payload_len, header->markerbit != 0,
			ntohs(header->seq_number), ntohl(header->timestamp), &complete))
		return;
	if(!complete || depay.access_unit == NULL || depay.access_unit->len == 0)
		return;
	if(depay.access_unit->len > ROQ_DISPLAY_ANNEXB_MAX) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "Dropping oversized H.264 access unit (%u bytes)\n", depay.access_unit->len);
		roq_display_reset_depay();
		return;
	}
	gboolean keyframe = roq_display_annexb_is_keyframe(depay.access_unit->data, depay.access_unit->len);
	roq_display_decode_access_unit(depay.access_unit->data, depay.access_unit->len, keyframe);
	roq_display_reset_depay();
}

static void roq_display_feed_audio(uint64_t flow_id, imquic_roq_rtp_header *header,
		uint8_t *rtp, size_t rtp_len) {
	if(audiodec == NULL)
		return;
	if(cfg.audio_flow >= 0 && (int64_t)flow_id != cfg.audio_flow)
		return;
	if(cfg.audio_pt != 255 && header->type != cfg.audio_pt)
		return;
	size_t payload_len = 0;
	size_t payload_offset = roq_display_rtp_payload_offset(rtp, rtp_len, &payload_len);
	if(payload_offset == 0 || payload_len == 0)
		return;
	roq_display_decode_audio(rtp + payload_offset, payload_len);
}

int roq_display_init(const roq_display_config *config) {
	if(config == NULL)
		return -1;
	if(!config->play_video && !config->play_audio) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "No audio or video playback enabled\n");
		return -1;
	}
	memcpy(&cfg, config, sizeof(cfg));
	if(cfg.window_width > 0)
		screen_w = config->window_width;
	if(cfg.window_height > 0)
		screen_h = config->window_height;
	if(cfg.debug_ffmpeg)
		av_log_set_level(AV_LOG_DEBUG);

	if(cfg.play_video) {
		video_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if(video_codec == NULL) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "H.264 decoder not available in libavcodec\n");
			return -1;
		}
		if(roq_display_create_decoder() < 0)
			return -1;
	}
	if(cfg.play_audio && roq_display_create_audio_decoder() < 0)
		return -1;

	Uint32 sdl_flags = 0;
	if(cfg.play_video)
		sdl_flags |= SDL_INIT_VIDEO;
	if(cfg.play_audio)
		sdl_flags |= SDL_INIT_AUDIO;
	if(SDL_Init(sdl_flags) < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Couldn't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}
	sdl_video_inited = cfg.play_video;
	sdl_audio_inited = cfg.play_audio;

	if(cfg.play_video) {
		window = SDL_CreateWindow("imquic-roq-receiver", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			screen_w, screen_h, SDL_WINDOW_SHOWN);
		if(window == NULL) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error creating window: %s\n", SDL_GetError());
			return -1;
		}
		renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
		if(renderer == NULL) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Error creating renderer: %s\n", SDL_GetError());
			return -1;
		}
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Video display enabled (flow=%"SCNi64", pt=%u, window=%dx%d)\n",
			cfg.video_flow, cfg.video_pt, screen_w, screen_h);
	}
	if(cfg.play_audio && roq_display_create_audio_output() < 0)
		return -1;
	if(cfg.play_audio)
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Audio playback enabled (flow=%"SCNi64", pt=%u)\n",
			cfg.audio_flow, cfg.audio_pt);
	return 0;
}

void roq_display_feed_rtp(uint64_t flow_id, uint8_t *rtp, size_t rtp_len) {
	if(rtp == NULL || rtp_len < 12)
		return;
	if(!imquic_roq_is_rtp(rtp, (guint)rtp_len))
		return;
	imquic_roq_rtp_header *header = (imquic_roq_rtp_header *)rtp;
	if(cfg.play_audio)
		roq_display_feed_audio(flow_id, header, rtp, rtp_len);
	if(cfg.play_video)
		roq_display_feed_video(flow_id, header, rtp, rtp_len);
}

int roq_display_handle_events(void) {
	if(window == NULL)
		return 0;
	SDL_Event event = { 0 };
	while(SDL_PollEvent(&event) != 0) {
		if(event.type == SDL_QUIT ||
				(event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE) ||
				(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
			return -1;
	}
	return 0;
}

int roq_display_render(void) {
	if(renderer == NULL)
		return 0;
	uint32_t ticks = SDL_GetTicks();
	if(last_render_tick == 0)
		last_render_tick = ticks;
	if(ticks - last_render_tick < (1000 / 30))
		return 0;
	last_render_tick = ticks;

	SDL_Rect rect = { 0 };
	SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
	SDL_RenderClear(renderer);
	imquic_mutex_lock(&frame_mutex);
	if(latest_frame != NULL) {
		if(latest_frame->width != texture_w || latest_frame->height != texture_h) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Video resolution: %dx%d\n", latest_frame->width, latest_frame->height);
			texture_w = latest_frame->width;
			texture_h = latest_frame->height;
			if(texture != NULL)
				SDL_DestroyTexture(texture);
			texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12,
				SDL_TEXTUREACCESS_STATIC, texture_w, texture_h);
		}
		if(texture != NULL) {
			SDL_UpdateYUVTexture(texture, NULL,
				latest_frame->data[0], latest_frame->linesize[0],
				latest_frame->data[1], latest_frame->linesize[1],
				latest_frame->data[2], latest_frame->linesize[2]);
			double screen_ratio = (double)screen_w / (double)screen_h;
			double texture_ratio = (double)texture_w / (double)texture_h;
			if(screen_ratio > texture_ratio) {
				double new_w = (double)texture_w * ((double)screen_h / (double)texture_h);
				rect.x = (screen_w - (int)new_w) / 2;
				rect.y = 0;
				rect.w = (int)new_w;
				rect.h = screen_h;
			} else if(screen_ratio < texture_ratio) {
				double new_h = (double)texture_h * ((double)screen_w / (double)texture_w);
				rect.x = 0;
				rect.y = (screen_h - (int)new_h) / 2;
				rect.w = screen_w;
				rect.h = (int)new_h;
			}
			SDL_RenderCopy(renderer, texture, NULL,
				(screen_ratio != texture_ratio ? &rect : NULL));
		}
	}
	imquic_mutex_unlock(&frame_mutex);
	SDL_RenderPresent(renderer);
	SDL_Delay(10);
	return 0;
}

void roq_display_destroy(void) {
	imquic_mutex_lock(&frame_mutex);
	if(latest_frame != NULL) {
		av_frame_free(&latest_frame);
		latest_frame = NULL;
	}
	imquic_mutex_unlock(&frame_mutex);
	if(audiodec != NULL) {
		opus_decoder_destroy(audiodec);
		audiodec = NULL;
	}
	if(audio_dev) {
		SDL_CloseAudioDevice(audio_dev);
		audio_dev = 0;
	}
	if(videodec_ctx != NULL)
		avcodec_free_context(&videodec_ctx);
	videodec_ctx = NULL;
	if(texture != NULL) {
		SDL_DestroyTexture(texture);
		texture = NULL;
	}
	if(renderer != NULL) {
		SDL_DestroyRenderer(renderer);
		renderer = NULL;
	}
	if(window != NULL) {
		SDL_DestroyWindow(window);
		window = NULL;
	}
	if(depay.access_unit != NULL) {
		g_byte_array_free(depay.access_unit, TRUE);
		depay.access_unit = NULL;
	}
	if(depay.fu_buffer != NULL) {
		g_byte_array_free(depay.fu_buffer, TRUE);
		depay.fu_buffer = NULL;
	}
	if(sdl_video_inited || sdl_audio_inited)
		SDL_Quit();
	sdl_video_inited = FALSE;
	sdl_audio_inited = FALSE;
}
