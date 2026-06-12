/*
 * imquic
 *
 * Scalable Video Coding (SVC) helpers for MoQ LOC demos
 *
 */

#include <arpa/inet.h>
#include <math.h>
#include <string.h>

#include <imquic/debug.h>
#include <imquic/imquic.h>
#include <imquic/mutex.h>

#include "moq-loc-svc.h"

#define MOQ_LOC_SVC_ABR_RTT_TARGET_US     150000
#define MOQ_LOC_SVC_ABR_JITTER_TARGET_US  50000
#define MOQ_LOC_SVC_ABR_LOSS_TARGET       0.50

struct moq_loc_svc_abr {
	int temporal_layers;
	int spatial_layers;
	int max_temporal_layer;
	int max_spatial_layer;
	int temporal_upgrade_holdoff;
	int spatial_upgrade_holdoff;
	double stress;
	double loss_rate;
	uint64_t prev_packets_sent;
	uint64_t prev_packets_lost;
	uint64_t prev_send_ok;
	uint64_t prev_send_fail;
	imquic_mutex mutex;
};

static uint32_t moq_loc_svc_read_bits(const uint8_t *data, size_t len, size_t *bit_offset, int count) {
	uint32_t value = 0;
	int i = 0;
	for(i = 0; i < count; i++) {
		size_t byte_idx = (*bit_offset) / 8;
		int bit_idx = 7 - (int)((*bit_offset) % 8);
		if(byte_idx >= len)
			break;
		value = (value << 1) | ((data[byte_idx] >> bit_idx) & 0x01);
		(*bit_offset)++;
	}
	return value;
}

static gboolean moq_loc_svc_h264_first_nal(const uint8_t *data, size_t len, gboolean avcc,
		uint8_t **nal, size_t *nal_len) {
	if(data == NULL || len < 1)
		return FALSE;
	if(avcc) {
		if(len < 4)
			return FALSE;
		uint32_t size = 0;
		memcpy(&size, data, 4);
		size = ntohl(size);
		if(size == 0 || (size_t)size + 4 > len)
			return FALSE;
		*nal = (uint8_t *)(data + 4);
		*nal_len = size;
		return TRUE;
	}
	if(len >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 && data[3] == 0x01) {
		*nal = (uint8_t *)(data + 4);
		*nal_len = len - 4;
		return TRUE;
	}
	if(len >= 3 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x01) {
		*nal = (uint8_t *)(data + 3);
		*nal_len = len - 3;
		return TRUE;
	}
	*nal = (uint8_t *)data;
	*nal_len = len;
	return TRUE;
}

static gboolean moq_loc_svc_parse_h264_nal(const uint8_t *nal, size_t nal_len, moq_loc_svc_layer *layer) {
	uint8_t nal_type = 0;
	size_t bit_offset = 0;
	if(nal == NULL || nal_len < 1 || layer == NULL)
		return FALSE;
	nal_type = nal[0] & 0x1F;
	layer->spatial_id = 0;
	layer->temporal_id = 0;
	layer->is_keyframe = (nal_type == 5 || nal_type == 7 || nal_type == 8 || nal_type == 15);
	if(nal_type == 14 || nal_type == 20 || nal_type == 21) {
		if(nal_len < 3)
			return FALSE;
		bit_offset = 8;
		(void)moq_loc_svc_read_bits(nal, nal_len, &bit_offset, 1);	/* idr_flag */
		(void)moq_loc_svc_read_bits(nal, nal_len, &bit_offset, 6);	/* priority_id */
		(void)moq_loc_svc_read_bits(nal, nal_len, &bit_offset, 1);	/* no_inter_layer_pred_flag */
		layer->spatial_id = (uint8_t)moq_loc_svc_read_bits(nal, nal_len, &bit_offset, 3);
		(void)moq_loc_svc_read_bits(nal, nal_len, &bit_offset, 4);	/* quality_id */
		layer->temporal_id = (uint8_t)moq_loc_svc_read_bits(nal, nal_len, &bit_offset, 3);
		if(nal_type == 20 || nal_type == 21)
			layer->is_keyframe = TRUE;
		return TRUE;
	}
	if(nal_type == 1 || nal_type == 5)
		return TRUE;
	return TRUE;
}

static gboolean moq_loc_svc_parse_vp9(const uint8_t *data, size_t len, moq_loc_svc_layer *layer) {
	size_t bit_offset = 0;
	uint32_t frame_marker = 0, profile = 0, show_existing = 0, frame_type = 0;
	uint32_t sync_code = 0, error_resilient = 0;
	if(data == NULL || len < 3 || layer == NULL)
		return FALSE;
	layer->spatial_id = 0;
	layer->temporal_id = 0;
	layer->is_keyframe = FALSE;
	frame_marker = moq_loc_svc_read_bits(data, len, &bit_offset, 2);
	if(frame_marker != 0x02)
		return FALSE;
	profile = moq_loc_svc_read_bits(data, len, &bit_offset, 1);
	if(profile)
		profile = (moq_loc_svc_read_bits(data, len, &bit_offset, 1) << 1) | 1;
	show_existing = moq_loc_svc_read_bits(data, len, &bit_offset, 1);
	if(show_existing)
		return FALSE;
	frame_type = moq_loc_svc_read_bits(data, len, &bit_offset, 1);
	if(frame_type == 0) {
		(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* show_frame */
		(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* error_resilient_mode */
		sync_code = moq_loc_svc_read_bits(data, len, &bit_offset, 24);
		if(sync_code != 0x498342)
			return FALSE;
		layer->is_keyframe = TRUE;
		layer->temporal_id = 0;
		return TRUE;
	}
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* show_frame */
	error_resilient = moq_loc_svc_read_bits(data, len, &bit_offset, 1);
	if(!error_resilient)
		(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* intra_only */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 2);	/* reset_frame_context */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* allow_high_precision_mv */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* mcomp_filter_type */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* reference_mode */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* reference_select */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* refresh_frame_context */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 2);
	layer->temporal_id = (uint8_t)moq_loc_svc_read_bits(data, len, &bit_offset, 3);
	return TRUE;
}

static int moq_loc_svc_even_dim(int value) {
	if(value <= 0)
		return 2;
	return (value & 1) ? value + 1 : value;
}

static void moq_loc_svc_fit_dimensions(int max_width, int max_height, int target_width,
		int *width, int *height) {
	double aspect = 0.0, factor = 0.0;
	int w = 0, h = 0, min_width = 0;
	if(max_width <= 0 || max_height <= 0 || target_width <= 0 || width == NULL || height == NULL)
		return;
	aspect = (double)max_width / (double)max_height;
	factor = (double)target_width / (double)max_width;
	if(factor > 1.0)
		factor = 1.0;
	w = (int)(max_width * factor);
	min_width = moq_loc_svc_even_dim(max_width / 4);
	if(w < min_width)
		w = min_width;
	w = moq_loc_svc_even_dim(w);
	h = moq_loc_svc_even_dim((int)(w / aspect + 0.5));
	if(w > max_width) {
		w = max_width;
		h = moq_loc_svc_even_dim((int)(w / aspect + 0.5));
	}
	if(h > max_height) {
		h = max_height;
		w = moq_loc_svc_even_dim((int)(h * aspect + 0.5));
	}
	*width = w;
	*height = h;
}

void moq_loc_svc_config_init(moq_loc_svc_config *cfg) {
	if(cfg == NULL)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->temporal_layers = 2;
	cfg->spatial_layers = 1;
	cfg->max_send_temporal_layer = MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS - 1;
	cfg->max_send_spatial_layer = MOQ_LOC_SVC_MAX_SPATIAL_LAYERS - 1;
}

gboolean moq_loc_svc_config_validate(const moq_loc_svc_config *cfg) {
	if(cfg == NULL || !cfg->enabled)
		return TRUE;
	if(cfg->temporal_layers < 2 || cfg->temporal_layers > MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS)
		return FALSE;
	if(cfg->spatial_layers < 1 || cfg->spatial_layers > MOQ_LOC_SVC_MAX_SPATIAL_LAYERS)
		return FALSE;
	if(cfg->max_send_temporal_layer < 0 ||
			cfg->max_send_temporal_layer >= cfg->temporal_layers)
		return FALSE;
	if(cfg->max_send_spatial_layer < 0 ||
			cfg->max_send_spatial_layer >= cfg->spatial_layers)
		return FALSE;
	if(!moq_loc_svc_is_svc_codec(cfg->codec))
		return FALSE;
	return TRUE;
}

gboolean moq_loc_svc_use_multi_spatial_encode(const moq_loc_svc_config *cfg) {
	if(cfg == NULL || !cfg->enabled || cfg->spatial_layers <= 1)
		return FALSE;
	return moq_loc_svc_is_svc_codec(cfg->codec);
}

void moq_loc_svc_spatial_layer_dimensions(int full_width, int full_height, int spatial_layers,
		int spatial_id, int *width, int *height) {
	int divisor = 1, target_width = 0;
	if(width == NULL || height == NULL || full_width <= 0 || full_height <= 0)
		return;
	if(spatial_layers < 1)
		spatial_layers = 1;
	if(spatial_id < 0)
		spatial_id = 0;
	if(spatial_id >= spatial_layers)
		spatial_id = spatial_layers - 1;
	if(spatial_layers <= 1) {
		*width = full_width;
		*height = full_height;
		return;
	}
	divisor = 1 << (spatial_layers - 1 - spatial_id);
	target_width = full_width / divisor;
	if(target_width < 1)
		target_width = 1;
	moq_loc_svc_fit_dimensions(full_width, full_height, target_width, width, height);
}

int moq_loc_svc_spatial_layer_bitrate(int total_bitrate, int full_width, int full_height,
		int spatial_layers, int spatial_id) {
	int total_pixels = 0, layer_pixels = 0, w = 0, h = 0, s = 0;
	if(total_bitrate <= 0 || spatial_layers < 1)
		return total_bitrate;
	if(spatial_id < 0)
		spatial_id = 0;
	if(spatial_id >= spatial_layers)
		spatial_id = spatial_layers - 1;
	for(s = 0; s < spatial_layers; s++) {
		moq_loc_svc_spatial_layer_dimensions(full_width, full_height, spatial_layers, s, &w, &h);
		total_pixels += w * h;
	}
	moq_loc_svc_spatial_layer_dimensions(full_width, full_height, spatial_layers, spatial_id, &w, &h);
	layer_pixels = w * h;
	if(total_pixels <= 0 || layer_pixels <= 0)
		return total_bitrate / spatial_layers;
	return (int)((double)total_bitrate * (double)layer_pixels / (double)total_pixels);
}

gboolean moq_loc_svc_layer_within_send_limits(const moq_loc_svc_config *cfg,
		const moq_loc_svc_layer *layer) {
	if(cfg == NULL || !cfg->enabled || layer == NULL)
		return TRUE;
	if(layer->temporal_id > (uint8_t)cfg->max_send_temporal_layer)
		return FALSE;
	if(layer->spatial_id > (uint8_t)cfg->max_send_spatial_layer)
		return FALSE;
	return TRUE;
}

uint64_t moq_loc_svc_subgroup_id(uint8_t spatial_id, uint8_t temporal_id) {
	return ((uint64_t)spatial_id << 8) | temporal_id;
}

void moq_loc_svc_unpack_subgroup(uint64_t subgroup_id, uint8_t *spatial_id, uint8_t *temporal_id) {
	if(spatial_id != NULL)
		*spatial_id = (uint8_t)((subgroup_id >> 8) & 0xFF);
	if(temporal_id != NULL)
		*temporal_id = (uint8_t)(subgroup_id & 0xFF);
}

uint64_t moq_loc_svc_frame_marking_value(const moq_loc_svc_layer *layer) {
	if(layer == NULL)
		return 0;
	return ((uint64_t)(layer->is_keyframe ? 1 : 0) << 16) |
		((uint64_t)layer->spatial_id << 8) |
		layer->temporal_id;
}

gboolean moq_loc_svc_layer_from_frame_marking(uint64_t value, moq_loc_svc_layer *layer) {
	if(layer == NULL)
		return FALSE;
	layer->is_keyframe = ((value >> 16) & 0x01) ? TRUE : FALSE;
	layer->spatial_id = (uint8_t)((value >> 8) & 0xFF);
	layer->temporal_id = (uint8_t)(value & 0xFF);
	return TRUE;
}

gboolean moq_loc_svc_layer_from_properties(GList *properties, moq_loc_svc_layer *layer) {
	GList *temp = properties;
	if(layer == NULL)
		return FALSE;
	while(temp) {
		imquic_moq_property *prop = (imquic_moq_property *)temp->data;
		if(prop != NULL && prop->id == IMQUIC_MOQ_LOC_VIDEO_FRAME_MARKING) {
			return moq_loc_svc_layer_from_frame_marking(prop->value.number, layer);
		}
		temp = temp->next;
	}
	return FALSE;
}

void moq_loc_svc_set_frame_marking(imquic_moq_property *prop, const moq_loc_svc_layer *layer) {
	if(prop == NULL || layer == NULL)
		return;
	memset(prop, 0, sizeof(*prop));
	prop->id = IMQUIC_MOQ_LOC_VIDEO_FRAME_MARKING;
	prop->value.number = moq_loc_svc_frame_marking_value(layer);
}

int moq_loc_svc_parse_packet(imquic_demo_video_codec codec, const uint8_t *data, size_t len,
		gboolean avcc, moq_loc_svc_layer *layer) {
	uint8_t *nal = NULL;
	size_t nal_len = 0;
	if(data == NULL || layer == NULL)
		return -1;
	memset(layer, 0, sizeof(*layer));
	if(codec == DEMO_H264_SVC || codec == DEMO_H264_AVCC || codec == DEMO_H264_ANNEXB) {
		if(!moq_loc_svc_h264_first_nal(data, len, avcc, &nal, &nal_len))
			return -1;
		if(!moq_loc_svc_parse_h264_nal(nal, nal_len, layer))
			return -1;
		return 0;
	}
	if(codec == DEMO_VP9_SVC || codec == DEMO_VP9) {
		if(!moq_loc_svc_parse_vp9(data, len, layer))
			return -1;
		return 0;
	}
	return -1;
}

const char *moq_loc_svc_catalog_codec(imquic_demo_video_codec codec) {
	switch(codec) {
		case DEMO_H264_SVC:
			return "avc1.534015";
		case DEMO_VP9_SVC:
			return "vp9.svc";
		default:
			return NULL;
	}
}

gboolean moq_loc_svc_is_svc_codec(imquic_demo_video_codec codec) {
	return codec == DEMO_H264_SVC || codec == DEMO_VP9_SVC;
}

int moq_loc_svc_object_temporal_layer(imquic_moq_object *object) {
	moq_loc_svc_layer layer = { 0 };
	uint8_t spatial = 0, temporal = 0;
	if(object == NULL)
		return 0;
	if(moq_loc_svc_layer_from_properties(object->properties, &layer))
		return layer.temporal_id;
	moq_loc_svc_unpack_subgroup(object->subgroup_id, &spatial, &temporal);
	return temporal;
}

int moq_loc_svc_object_spatial_layer(imquic_moq_object *object) {
	moq_loc_svc_layer layer = { 0 };
	uint8_t spatial = 0, temporal = 0;
	if(object == NULL)
		return 0;
	if(moq_loc_svc_layer_from_properties(object->properties, &layer))
		return layer.spatial_id;
	moq_loc_svc_unpack_subgroup(object->subgroup_id, &spatial, &temporal);
	return spatial;
}

gboolean moq_loc_svc_object_within_layer(imquic_demo_video_codec codec, imquic_moq_object *object,
		int max_temporal_layer, int max_spatial_layer) {
	if(object == NULL)
		return FALSE;
	if(!moq_loc_svc_is_svc_codec(codec))
		return TRUE;
	return moq_loc_svc_object_within_limits(object, max_temporal_layer, max_spatial_layer);
}

gboolean moq_loc_svc_object_within_limits(imquic_moq_object *object,
		int max_temporal_layer, int max_spatial_layer) {
	moq_loc_svc_layer layer = { 0 };
	if(object == NULL)
		return FALSE;
	if(max_temporal_layer < 0 && max_spatial_layer < 0)
		return TRUE;
	if(moq_loc_svc_layer_from_properties(object->properties, &layer)) {
		if(max_temporal_layer >= 0 && layer.temporal_id > (uint8_t)max_temporal_layer)
			return FALSE;
		if(max_spatial_layer >= 0 && layer.spatial_id > (uint8_t)max_spatial_layer)
			return FALSE;
		return TRUE;
	}
	moq_loc_svc_unpack_subgroup(object->subgroup_id, &layer.spatial_id, &layer.temporal_id);
	if(max_temporal_layer >= 0 && layer.temporal_id > (uint8_t)max_temporal_layer)
		return FALSE;
	if(max_spatial_layer >= 0 && layer.spatial_id > (uint8_t)max_spatial_layer)
		return FALSE;
	return TRUE;
}

static double moq_loc_svc_abr_clamp01(double value) {
	if(value < 0.0)
		return 0.0;
	if(value > 1.0)
		return 1.0;
	return value;
}

static int moq_loc_svc_abr_target_max_layer(int layer_count, double stress) {
	int max_idx = 0, target_max = 0;
	if(layer_count < 1)
		return 0;
	max_idx = layer_count - 1;
	target_max = max_idx - (int)(stress * max_idx + 0.49);
	if(target_max < 0)
		target_max = 0;
	if(target_max > max_idx)
		target_max = max_idx;
	return target_max;
}

moq_loc_svc_abr *moq_loc_svc_abr_create(int temporal_layers, int spatial_layers) {
	moq_loc_svc_abr *abr = g_malloc0(sizeof(moq_loc_svc_abr));
	if(temporal_layers < 2)
		temporal_layers = 2;
	if(temporal_layers > MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS)
		temporal_layers = MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS;
	if(spatial_layers < 1)
		spatial_layers = 1;
	if(spatial_layers > MOQ_LOC_SVC_MAX_SPATIAL_LAYERS)
		spatial_layers = MOQ_LOC_SVC_MAX_SPATIAL_LAYERS;
	abr->temporal_layers = temporal_layers;
	abr->spatial_layers = spatial_layers;
	abr->max_temporal_layer = temporal_layers - 1;
	abr->max_spatial_layer = spatial_layers - 1;
	abr->temporal_upgrade_holdoff = 0;
	abr->spatial_upgrade_holdoff = 0;
	imquic_mutex_init(&abr->mutex);
	return abr;
}

void moq_loc_svc_abr_destroy(moq_loc_svc_abr *abr) {
	if(abr == NULL)
		return;
	imquic_mutex_destroy(&abr->mutex);
	g_free(abr);
}

void moq_loc_svc_abr_set_temporal_layers(moq_loc_svc_abr *abr, int temporal_layers) {
	if(abr == NULL)
		return;
	if(temporal_layers < 2)
		temporal_layers = 2;
	if(temporal_layers > MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS)
		temporal_layers = MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS;
	imquic_mutex_lock(&abr->mutex);
	abr->temporal_layers = temporal_layers;
	if(abr->max_temporal_layer >= temporal_layers)
		abr->max_temporal_layer = temporal_layers - 1;
	imquic_mutex_unlock(&abr->mutex);
}

void moq_loc_svc_abr_set_spatial_layers(moq_loc_svc_abr *abr, int spatial_layers) {
	if(abr == NULL)
		return;
	if(spatial_layers < 1)
		spatial_layers = 1;
	if(spatial_layers > MOQ_LOC_SVC_MAX_SPATIAL_LAYERS)
		spatial_layers = MOQ_LOC_SVC_MAX_SPATIAL_LAYERS;
	imquic_mutex_lock(&abr->mutex);
	abr->spatial_layers = spatial_layers;
	if(abr->max_spatial_layer >= spatial_layers)
		abr->max_spatial_layer = spatial_layers - 1;
	imquic_mutex_unlock(&abr->mutex);
}

void moq_loc_svc_abr_update(moq_loc_svc_abr *abr, imquic_connection *conn,
		uint64_t send_ok, uint64_t send_fail, double media_loss_rate) {
	imquic_path_quality pq = { 0 };
	double loss_rate = 0.0, stress = 0.0;
	uint64_t delta_sent = 0, delta_lost = 0, delta_ok = 0, delta_fail = 0;
	int prev_temporal = 0, prev_spatial = 0;

	if(abr == NULL)
		return;
	if(conn != NULL)
		imquic_get_connection_path_quality(conn, &pq);

	imquic_mutex_lock(&abr->mutex);
	delta_sent = pq.packets_sent - abr->prev_packets_sent;
	delta_lost = pq.packets_lost - abr->prev_packets_lost;
	delta_ok = send_ok - abr->prev_send_ok;
	delta_fail = send_fail - abr->prev_send_fail;
	abr->prev_packets_sent = pq.packets_sent;
	abr->prev_packets_lost = pq.packets_lost;
	abr->prev_send_ok = send_ok;
	abr->prev_send_fail = send_fail;

	if(delta_sent > 0)
		loss_rate = (double)delta_lost / (double)delta_sent;
	else if((delta_ok + delta_fail) > 0)
		loss_rate = (double)delta_fail / (double)(delta_ok + delta_fail);
	if(media_loss_rate >= 0.0)
		loss_rate = (loss_rate > media_loss_rate) ? loss_rate : media_loss_rate;

	stress = 0.40 * moq_loc_svc_abr_clamp01(loss_rate / MOQ_LOC_SVC_ABR_LOSS_TARGET);
	stress += 0.25 * moq_loc_svc_abr_clamp01((double)pq.rtt_us / (double)MOQ_LOC_SVC_ABR_RTT_TARGET_US);
	stress += 0.20 * moq_loc_svc_abr_clamp01((double)pq.rtt_jitter_us / (double)MOQ_LOC_SVC_ABR_JITTER_TARGET_US);
	if(pq.cwin > 0)
		stress += 0.15 * moq_loc_svc_abr_clamp01((double)pq.bytes_in_transit / (double)pq.cwin);
	if(stress > 1.0)
		stress = 1.0;

	abr->stress = stress;
	abr->loss_rate = loss_rate;

	prev_temporal = abr->max_temporal_layer;
	if(abr->temporal_layers >= 2) {
		int target_max = moq_loc_svc_abr_target_max_layer(abr->temporal_layers, stress);
		if(target_max < abr->max_temporal_layer) {
			abr->max_temporal_layer = target_max;
			abr->temporal_upgrade_holdoff = 4;
		} else if(target_max > abr->max_temporal_layer) {
			if(abr->temporal_upgrade_holdoff > 0)
				abr->temporal_upgrade_holdoff--;
			else {
				abr->max_temporal_layer = target_max;
				abr->temporal_upgrade_holdoff = 4;
			}
		} else if(abr->temporal_upgrade_holdoff > 0) {
			abr->temporal_upgrade_holdoff--;
		}
		if(prev_temporal != abr->max_temporal_layer) {
			IMQUIC_LOG(IMQUIC_LOG_INFO,
				"SVC ABR max temporal layer %d -> %d (stress=%.2f, loss=%.1f%%, RTT=%"SCNu64"ms, jitter=%"SCNu64"ms)\n",
				prev_temporal, abr->max_temporal_layer, stress, loss_rate * 100.0,
				pq.rtt_us / 1000, pq.rtt_jitter_us / 1000);
		}
	}

	prev_spatial = abr->max_spatial_layer;
	if(abr->spatial_layers >= 2) {
		int target_max = moq_loc_svc_abr_target_max_layer(abr->spatial_layers, stress);
		if(target_max < abr->max_spatial_layer) {
			abr->max_spatial_layer = target_max;
			abr->spatial_upgrade_holdoff = 4;
		} else if(target_max > abr->max_spatial_layer) {
			if(abr->spatial_upgrade_holdoff > 0)
				abr->spatial_upgrade_holdoff--;
			else {
				abr->max_spatial_layer = target_max;
				abr->spatial_upgrade_holdoff = 4;
			}
		} else if(abr->spatial_upgrade_holdoff > 0) {
			abr->spatial_upgrade_holdoff--;
		}
		if(prev_spatial != abr->max_spatial_layer) {
			IMQUIC_LOG(IMQUIC_LOG_INFO,
				"SVC ABR max spatial layer %d -> %d (stress=%.2f, loss=%.1f%%, RTT=%"SCNu64"ms, jitter=%"SCNu64"ms)\n",
				prev_spatial, abr->max_spatial_layer, stress, loss_rate * 100.0,
				pq.rtt_us / 1000, pq.rtt_jitter_us / 1000);
		}
	}

	imquic_mutex_unlock(&abr->mutex);
}

int moq_loc_svc_abr_get_max_temporal_layer(const moq_loc_svc_abr *abr) {
	int max_layer = 0;
	if(abr == NULL)
		return 0;
	imquic_mutex_lock((imquic_mutex *)&abr->mutex);
	max_layer = abr->max_temporal_layer;
	imquic_mutex_unlock((imquic_mutex *)&abr->mutex);
	return max_layer;
}

int moq_loc_svc_abr_get_max_spatial_layer(const moq_loc_svc_abr *abr) {
	int max_layer = 0;
	if(abr == NULL)
		return 0;
	imquic_mutex_lock((imquic_mutex *)&abr->mutex);
	max_layer = abr->max_spatial_layer;
	imquic_mutex_unlock((imquic_mutex *)&abr->mutex);
	return max_layer;
}

double moq_loc_svc_abr_get_stress(const moq_loc_svc_abr *abr) {
	double stress = 0.0;
	if(abr == NULL)
		return 0.0;
	imquic_mutex_lock((imquic_mutex *)&abr->mutex);
	stress = abr->stress;
	imquic_mutex_unlock((imquic_mutex *)&abr->mutex);
	return stress;
}
