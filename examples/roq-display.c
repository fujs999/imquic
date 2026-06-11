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
#include <SDL2/SDL_ttf.h>

#include "roq-display.h"
#include "moq-loc-svc.h"
#include "roq-utils.h"

#define ROQ_DISPLAY_ANNEXB_MAX (512 * 1024)
#define ROQ_DISPLAY_AUDIO_RATE 48000
#define ROQ_DISPLAY_AUDIO_MAX_SAMPLES 1920
#define ROQ_DISPLAY_AUDIO_MAX_QUEUE 10000
#define ROQ_DISPLAY_FONT_SIZE 15
#define ROQ_DISPLAY_OVERLAY_MAX_LINES 12
#define ROQ_DISPLAY_VIDEO_RTP_CLOCK 90000

typedef struct roq_h264_depay {
	GByteArray *access_unit;
	GByteArray *fu_buffer;
	uint16_t last_seq;
	uint32_t last_ts;
	gboolean in_fu;
} roq_h264_depay;

static roq_display_config cfg = { 0 };
static roq_h264_depay depay = { 0 };
static imquic_roq_vp9_depay vp9_depay = { 0 };
static imquic_demo_video_codec video_codec_id = DEMO_H264_ANNEXB;
static int svc_max_temporal_layer = -1;
static int svc_max_spatial_layer = -1;
static int svc_max_temporal_cap = -1;
static int svc_temporal_layers = 2;
static int svc_current_temporal_layer = 0;
static moq_loc_svc_abr *svc_abr = NULL;

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

static TTF_Font *overlay_font = NULL;
static gboolean overlay_font_inited = FALSE;
static size_t video_bytes_window = 0;
static uint32_t video_frames_window = 0;
static double video_measured_fps = 0;
static double video_measured_bitrate = 0;
static uint32_t video_stats_last_tick = 0;

typedef struct roq_video_rtp_stats {
	gboolean initialized;
	uint16_t last_seq;
	uint32_t last_rtp_ts;
	int64_t last_arrival_us;
	double jitter_ms;
	uint32_t packets_received_window;
	uint32_t packets_lost_window;
} roq_video_rtp_stats;

static roq_video_rtp_stats video_rtp_stats = { 0 };
static imquic_mutex stats_mutex = IMQUIC_MUTEX_INITIALIZER;
static imquic_connection *stats_conn = NULL;

static uint32_t rtp_packets_lost_display = 0;
static uint32_t rtp_packets_recv_display = 0;
static double rtp_loss_rate_display = 0;
static double rtp_jitter_display = 0;
static double quic_rtt_display_ms = 0;
static double quic_jitter_display_ms = 0;
static double playout_delay_display_ms = 0;

static const char *roq_display_font_paths[] = {
	"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
	"/usr/share/fonts/TTF/DejaVuSans.ttf",
	"/usr/share/fonts/dejavu/DejaVuSans.ttf",
	"/usr/share/fonts/truetype/freefont/FreeSans.ttf",
	NULL
};

static void roq_display_update_stats(uint32_t ticks);
static void roq_display_update_svc_abr(void);

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
	if(video_codec_id == DEMO_VP9 || video_codec_id == DEMO_VP9_SVC)
		videodec_ctx->thread_count = 2;
	if(avcodec_open2(videodec_ctx, video_codec, NULL) < 0) {
		IMQUIC_LOG(IMQUIC_LOG_ERR, "Error opening video decoder\n");
		avcodec_free_context(&videodec_ctx);
		videodec_ctx = NULL;
		return -1;
	}
	return 0;
}

static void roq_display_format_bitrate(char *buf, size_t buflen, double bps) {
	if(bps >= 1000000.0)
		g_snprintf(buf, buflen, "%.2f Mbps", bps / 1000000.0);
	else if(bps >= 1000.0)
		g_snprintf(buf, buflen, "%.1f Kbps", bps / 1000.0);
	else
		g_snprintf(buf, buflen, "%.0f bps", bps);
}

static int roq_display_init_overlay_font(void) {
	const char *const *path = roq_display_font_paths;

	if(TTF_Init() < 0) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "TTF_Init failed: %s (video overlay disabled)\n", TTF_GetError());
		return -1;
	}
	overlay_font_inited = TRUE;
	for(; *path != NULL; path++) {
		overlay_font = TTF_OpenFont(*path, ROQ_DISPLAY_FONT_SIZE);
		if(overlay_font != NULL) {
			TTF_SetFontHinting(overlay_font, TTF_HINTING_LIGHT);
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Using overlay font '%s'\n", *path);
			return 0;
		}
	}
	IMQUIC_LOG(IMQUIC_LOG_WARN, "Could not open overlay font: %s (video overlay disabled)\n", TTF_GetError());
	return -1;
}

static void roq_display_destroy_overlay_font(void) {
	if(overlay_font != NULL) {
		TTF_CloseFont(overlay_font);
		overlay_font = NULL;
	}
	if(overlay_font_inited) {
		TTF_Quit();
		overlay_font_inited = FALSE;
	}
}

static void roq_display_set_stats_conn(imquic_connection *conn) {
	if(conn == NULL)
		return;
	imquic_mutex_lock(&stats_mutex);
	if(stats_conn != conn) {
		if(stats_conn != NULL)
			imquic_connection_unref(stats_conn);
		stats_conn = conn;
		imquic_connection_ref(stats_conn);
	}
	imquic_mutex_unlock(&stats_mutex);
}

static void roq_display_track_video_rtp(imquic_roq_rtp_header *header) {
	uint16_t seq = ntohs(header->seq_number);
	uint32_t ts = ntohl(header->timestamp);
	int64_t now_us = g_get_monotonic_time();

	imquic_mutex_lock(&stats_mutex);
	if(!video_rtp_stats.initialized) {
		video_rtp_stats.last_seq = seq;
		video_rtp_stats.last_rtp_ts = ts;
		video_rtp_stats.last_arrival_us = now_us;
		video_rtp_stats.initialized = TRUE;
	} else {
		uint16_t udiff = (uint16_t)(seq - video_rtp_stats.last_seq);
		if(udiff > 0 && udiff < 0x8000) {
			if(udiff > 1)
				video_rtp_stats.packets_lost_window += (udiff - 1);
			if(video_rtp_stats.last_arrival_us > 0) {
				double transit_ms = (double)(now_us - video_rtp_stats.last_arrival_us) / 1000.0;
				int32_t ts_diff = (int32_t)(ts - video_rtp_stats.last_rtp_ts);
				double media_ms = (double)ts_diff * 1000.0 / (double)ROQ_DISPLAY_VIDEO_RTP_CLOCK;
				double d = transit_ms - media_ms;
				if(d < 0.0)
					d = -d;
				video_rtp_stats.jitter_ms += (d - video_rtp_stats.jitter_ms) / 16.0;
			}
			video_rtp_stats.last_seq = seq;
			video_rtp_stats.last_rtp_ts = ts;
			video_rtp_stats.last_arrival_us = now_us;
		}
	}
	video_rtp_stats.packets_received_window++;
	imquic_mutex_unlock(&stats_mutex);
}

static double roq_display_audio_playout_delay_ms(void) {
	Uint32 queued = 0;

	if(audio_dev == 0)
		return 0.0;
	queued = SDL_GetQueuedAudioSize(audio_dev);
	return (double)queued * 1000.0 / (double)(ROQ_DISPLAY_AUDIO_RATE * 2);
}

static void roq_display_update_stats(uint32_t ticks) {
	uint32_t elapsed = 0;
	imquic_path_quality pq = { 0 };

	if(video_stats_last_tick == 0) {
		video_stats_last_tick = ticks;
		return;
	}
	elapsed = ticks - video_stats_last_tick;
	if(elapsed < 1000)
		return;

	imquic_mutex_lock(&frame_mutex);
	video_measured_fps = (double)video_frames_window * 1000.0 / (double)elapsed;
	video_measured_bitrate = (double)video_bytes_window * 8.0 * 1000.0 / (double)elapsed;
	video_bytes_window = 0;
	video_frames_window = 0;
	imquic_mutex_unlock(&frame_mutex);

	imquic_mutex_lock(&stats_mutex);
	rtp_packets_lost_display = video_rtp_stats.packets_lost_window;
	rtp_packets_recv_display = video_rtp_stats.packets_received_window;
	if(rtp_packets_recv_display + rtp_packets_lost_display > 0)
		rtp_loss_rate_display = 100.0 * (double)rtp_packets_lost_display /
			(double)(rtp_packets_recv_display + rtp_packets_lost_display);
	else
		rtp_loss_rate_display = 0.0;
	rtp_jitter_display = video_rtp_stats.jitter_ms;
	video_rtp_stats.packets_lost_window = 0;
	video_rtp_stats.packets_received_window = 0;

	if(stats_conn != NULL && imquic_get_connection_path_quality(stats_conn, &pq) == 0) {
		quic_rtt_display_ms = (double)pq.rtt_us / 1000.0;
		quic_jitter_display_ms = (double)pq.rtt_jitter_us / 1000.0;
	}
	imquic_mutex_unlock(&stats_mutex);

	playout_delay_display_ms = roq_display_audio_playout_delay_ms();
	roq_display_update_svc_abr();
	video_stats_last_tick = ticks;
}

static void roq_display_draw_overlay_line(SDL_Renderer *r, int x, int y, const char *line) {
	SDL_Color white = { 255, 255, 255, 255 };
	SDL_Surface *surface = NULL;
	SDL_Texture *line_texture = NULL;
	SDL_Rect dst = { 0 };

	if(line == NULL || line[0] == '\0')
		return;
	surface = TTF_RenderUTF8_Blended(overlay_font, line, white);
	if(surface == NULL)
		return;
	line_texture = SDL_CreateTextureFromSurface(r, surface);
	dst.w = surface->w;
	dst.h = surface->h;
	SDL_FreeSurface(surface);
	if(line_texture == NULL)
		return;
	SDL_SetTextureBlendMode(line_texture, SDL_BLENDMODE_BLEND);
	dst.x = x;
	dst.y = y;
	SDL_RenderCopy(r, line_texture, NULL, &dst);
	SDL_DestroyTexture(line_texture);
}

static void roq_display_render_video_overlay(SDL_Renderer *r) {
	char line_bufs[ROQ_DISPLAY_OVERLAY_MAX_LINES][128], bitrate_str[32];
	const char *lines[ROQ_DISPLAY_OVERLAY_MAX_LINES];
	SDL_Rect bg = { 0 };
	int x = 10, y = 10, line_h = 0, max_w = 0, total_h = 0;
	int width = 0, height = 0, line_count = 0, i = 0, lw = 0, lh = 0;
	double fps = 0, bitrate = 0;
	uint32_t rtp_lost = 0, rtp_recv = 0;
	double rtp_loss = 0, rtp_jitter = 0;
	double quic_rtt = 0, quic_jitter = 0;
	double playout = 0;

	if(overlay_font == NULL)
		return;
	line_h = TTF_FontLineSkip(overlay_font);
	imquic_mutex_lock(&frame_mutex);
	if(latest_frame != NULL) {
		width = latest_frame->width;
		height = latest_frame->height;
	}
	fps = video_measured_fps;
	bitrate = video_measured_bitrate;
	imquic_mutex_unlock(&frame_mutex);
	imquic_mutex_lock(&stats_mutex);
	rtp_lost = rtp_packets_lost_display;
	rtp_recv = rtp_packets_recv_display;
	rtp_loss = rtp_loss_rate_display;
	rtp_jitter = rtp_jitter_display;
	quic_rtt = quic_rtt_display_ms;
	quic_jitter = quic_jitter_display_ms;
	imquic_mutex_unlock(&stats_mutex);
	playout = playout_delay_display_ms;
	if(width <= 0 && height <= 0 && fps <= 0.0 && bitrate <= 0.0 &&
			rtp_recv == 0 && quic_rtt <= 0.0)
		return;
	if(width > 0 && height > 0) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "Resolution: %dx%d", width, height);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "Codec: %s",
		imquic_demo_video_codec_str(video_codec_id));
	lines[line_count] = line_bufs[line_count];
	line_count++;
	if(fps > 0.0) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "FPS: %.1f", fps);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(bitrate > 0.0) {
		roq_display_format_bitrate(bitrate_str, sizeof(bitrate_str), bitrate);
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "Bitrate: %s", bitrate_str);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(rtp_recv > 0 || rtp_lost > 0) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]),
			"RTP loss: %"G_GUINT32_FORMAT" (%.1f%%)", rtp_lost, rtp_loss);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(rtp_jitter > 0.0) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "RTP jitter: %.1f ms", rtp_jitter);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(quic_rtt > 0.0) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "RTT: %.1f ms", quic_rtt);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(quic_jitter > 0.0) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "QUIC jitter: %.1f ms", quic_jitter);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(playout > 0.0) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]), "Playout delay: %.0f ms", playout);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(moq_loc_svc_is_svc_codec(video_codec_id)) {
		g_snprintf(line_bufs[line_count], sizeof(line_bufs[0]),
			"SVC layer: T%d (max=%d)", svc_current_temporal_layer,
			svc_max_temporal_layer >= 0 ? svc_max_temporal_layer : svc_temporal_layers - 1);
		lines[line_count] = line_bufs[line_count];
		line_count++;
	}
	if(line_count == 0)
		return;
	for(i = 0; i < line_count; i++) {
		if(TTF_SizeUTF8(overlay_font, lines[i], &lw, &lh) == 0 && lw > max_w)
			max_w = lw;
	}
	total_h = line_count * line_h;
	SDL_SetRenderDrawColor(r, 32, 32, 32, 255);
	bg.x = x - 8;
	bg.y = y - 6;
	bg.w = max_w + 16;
	bg.h = total_h + 12;
	SDL_RenderFillRect(r, &bg);
	for(i = 0; i < line_count; i++) {
		roq_display_draw_overlay_line(r, x, y, lines[i]);
		y += line_h;
	}
}

static gboolean roq_display_svc_within_limits(const uint8_t *data, size_t len) {
	moq_loc_svc_layer layer = { 0 };
	if(!moq_loc_svc_is_svc_codec(video_codec_id))
		return TRUE;
	if(data == NULL || len == 0)
		return FALSE;
	if(moq_loc_svc_parse_packet(video_codec_id, data, len, FALSE, &layer) < 0)
		return TRUE;
	svc_current_temporal_layer = layer.temporal_id;
	if(svc_max_temporal_layer >= 0 && layer.temporal_id > (uint8_t)svc_max_temporal_layer)
		return FALSE;
	if(svc_max_spatial_layer >= 0 && layer.spatial_id > (uint8_t)svc_max_spatial_layer)
		return FALSE;
	return TRUE;
}

static void roq_display_update_svc_abr(void) {
	double media_loss = 0.0;
	if(!moq_loc_svc_is_svc_codec(video_codec_id) || cfg.no_svc_adaptive || svc_max_temporal_cap >= 0)
		return;
	if(svc_abr == NULL) {
		svc_abr = moq_loc_svc_abr_create(svc_temporal_layers);
		svc_max_temporal_layer = moq_loc_svc_abr_get_max_temporal_layer(svc_abr);
		return;
	}
	imquic_mutex_lock(&stats_mutex);
	if(stats_conn != NULL) {
		if(rtp_packets_recv_display + rtp_packets_lost_display > 0)
			media_loss = (double)rtp_packets_lost_display /
				(double)(rtp_packets_recv_display + rtp_packets_lost_display);
		moq_loc_svc_abr_update(svc_abr, stats_conn, 0, 0, media_loss);
		svc_max_temporal_layer = moq_loc_svc_abr_get_max_temporal_layer(svc_abr);
		if(svc_max_temporal_cap >= 0 && svc_max_temporal_layer > svc_max_temporal_cap)
			svc_max_temporal_layer = svc_max_temporal_cap;
	}
	imquic_mutex_unlock(&stats_mutex);
}

static int roq_display_decode_access_unit(uint8_t *buffer, size_t length, gboolean keyframe) {
	if(videodec_ctx == NULL)
		return -1;
	if(!roq_display_svc_within_limits(buffer, length))
		return 0;
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
	video_bytes_window += length;
	video_frames_window++;
	imquic_mutex_unlock(&frame_mutex);
	return 0;
}

static void roq_display_feed_vp9(uint64_t flow_id, imquic_roq_rtp_header *header,
		uint8_t *rtp, size_t rtp_len) {
	size_t payload_len = 0;
	size_t payload_offset = roq_display_rtp_payload_offset(rtp, rtp_len, &payload_len);
	uint8_t *frame = NULL;
	size_t frame_len = 0;
	gboolean keyframe = FALSE;
	roq_display_track_video_rtp(header);
	if(payload_offset == 0 || payload_len == 0)
		return;
	const uint8_t *payload = rtp + payload_offset;
	if(!imquic_roq_rtp_depay_vp9(&vp9_depay, payload, payload_len,
			ntohs(header->seq_number), ntohl(header->timestamp), &frame, &frame_len))
		return;
	if(frame == NULL || frame_len == 0)
		return;
	if(frame_len > ROQ_DISPLAY_ANNEXB_MAX) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "Dropping oversized VP9 frame (%zu bytes)\n", frame_len);
		imquic_roq_vp9_depay_reset(&vp9_depay);
		return;
	}
	keyframe = imquic_demo_vp9_is_keyframe(frame, frame_len);
	roq_display_decode_access_unit(frame, frame_len, keyframe);
	imquic_roq_vp9_depay_reset(&vp9_depay);
}

static void roq_display_feed_video(uint64_t flow_id, imquic_roq_rtp_header *header,
		uint8_t *rtp, size_t rtp_len) {
	if(videodec_ctx == NULL)
		return;
	if(cfg.video_flow >= 0 && (int64_t)flow_id != cfg.video_flow)
		return;
	if(cfg.video_pt != 255 && header->type != cfg.video_pt)
		return;
	if(video_codec_id == DEMO_VP9 || video_codec_id == DEMO_VP9_SVC) {
		roq_display_feed_vp9(flow_id, header, rtp, rtp_len);
		return;
	}
	roq_display_track_video_rtp(header);
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

	video_codec_id = cfg.video_codec;
	if(video_codec_id == DEMO_UNKOWN)
		video_codec_id = DEMO_H264_ANNEXB;
	svc_max_temporal_cap = cfg.svc_max_temporal_layer;
	svc_max_spatial_layer = cfg.svc_max_spatial_layer;
	if(svc_max_temporal_cap >= 0)
		svc_max_temporal_layer = svc_max_temporal_cap;
	if(moq_loc_svc_is_svc_codec(video_codec_id)) {
		svc_temporal_layers = MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS;
		if(svc_max_temporal_cap >= 0 && svc_max_temporal_cap + 1 < svc_temporal_layers)
			svc_temporal_layers = svc_max_temporal_cap + 1;
		if(svc_temporal_layers < 2)
			svc_temporal_layers = 2;
	}

	if(cfg.play_video) {
		if(video_codec_id == DEMO_VP9 || video_codec_id == DEMO_VP9_SVC) {
			video_codec = avcodec_find_decoder_by_name("libvpx-vp9");
		} else {
			video_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		}
		if(video_codec == NULL) {
			IMQUIC_LOG(IMQUIC_LOG_ERR, "Video decoder for '%s' not available in libavcodec\n",
				imquic_demo_video_codec_str(video_codec_id));
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
		roq_display_init_overlay_font();
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

void roq_display_connection_gone(imquic_connection *conn) {
	imquic_mutex_lock(&stats_mutex);
	if(stats_conn == conn) {
		imquic_connection_unref(stats_conn);
		stats_conn = NULL;
	}
	imquic_mutex_unlock(&stats_mutex);
}

void roq_display_feed_rtp(imquic_connection *conn, uint64_t flow_id, uint8_t *rtp, size_t rtp_len) {
	if(rtp == NULL || rtp_len < 12)
		return;
	if(!imquic_roq_is_rtp(rtp, (guint)rtp_len))
		return;
	if(conn != NULL)
		roq_display_set_stats_conn(conn);
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
	roq_display_update_stats(ticks);

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
		if(texture != NULL)
			SDL_UpdateYUVTexture(texture, NULL,
				latest_frame->data[0], latest_frame->linesize[0],
				latest_frame->data[1], latest_frame->linesize[1],
				latest_frame->data[2], latest_frame->linesize[2]);
		imquic_mutex_unlock(&frame_mutex);
		if(texture != NULL) {
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
			roq_display_render_video_overlay(renderer);
		}
	} else {
		imquic_mutex_unlock(&frame_mutex);
	}
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
	roq_display_destroy_overlay_font();
	imquic_mutex_lock(&stats_mutex);
	if(stats_conn != NULL) {
		imquic_connection_unref(stats_conn);
		stats_conn = NULL;
	}
	imquic_mutex_unlock(&stats_mutex);
	if(depay.access_unit != NULL) {
		g_byte_array_free(depay.access_unit, TRUE);
		depay.access_unit = NULL;
	}
	if(depay.fu_buffer != NULL) {
		g_byte_array_free(depay.fu_buffer, TRUE);
		depay.fu_buffer = NULL;
	}
	if(vp9_depay.frame != NULL) {
		g_byte_array_free(vp9_depay.frame, TRUE);
		vp9_depay.frame = NULL;
	}
	if(svc_abr != NULL) {
		moq_loc_svc_abr_destroy(svc_abr);
		svc_abr = NULL;
	}
	if(sdl_video_inited || sdl_audio_inited)
		SDL_Quit();
	sdl_video_inited = FALSE;
	sdl_audio_inited = FALSE;
}
