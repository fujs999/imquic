/*
 * imquic
 *
 * Scalable Video Coding (SVC) helpers for MoQ LOC demos
 *
 */

#include <arpa/inet.h>
#include <string.h>

#include "moq-loc-svc.h"

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
	size_t bit_offset = 0, saved = 0;
	uint32_t frame_marker = 0, profile = 0, sync_code = 0;
	if(data == NULL || len < 3 || layer == NULL)
		return FALSE;
	layer->spatial_id = 0;
	layer->temporal_id = 0;
	layer->is_keyframe = FALSE;
	frame_marker = moq_loc_svc_read_bits(data, len, &bit_offset, 2);
	if(frame_marker != 0x02)
		return FALSE;
	profile = moq_loc_svc_read_bits(data, len, &bit_offset, 1);
	if(profile == 3)
		(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);
	saved = bit_offset;
	sync_code = moq_loc_svc_read_bits(data, len, &bit_offset, 24);
	if(sync_code == 0x498342) {
		layer->is_keyframe = TRUE;
		layer->temporal_id = 0;
		return TRUE;
	}
	bit_offset = saved;
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* show_frame */
	(void)moq_loc_svc_read_bits(data, len, &bit_offset, 1);	/* error_resilient */
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

void moq_loc_svc_config_init(moq_loc_svc_config *cfg) {
	if(cfg == NULL)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->temporal_layers = 2;
	cfg->spatial_layers = 1;
	cfg->max_send_temporal_layer = MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS - 1;
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
	if(!moq_loc_svc_is_svc_codec(cfg->codec))
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

gboolean moq_loc_svc_object_within_layer(imquic_demo_video_codec codec, imquic_moq_object *object,
		int max_temporal_layer, int max_spatial_layer) {
	moq_loc_svc_layer layer = { 0 };
	if(object == NULL)
		return FALSE;
	if(!moq_loc_svc_is_svc_codec(codec))
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
