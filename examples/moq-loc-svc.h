/*
 * imquic
 *
 * Scalable Video Coding (SVC) helpers for MoQ LOC demos
 *
 */

#ifndef MOQ_LOC_SVC_H
#define MOQ_LOC_SVC_H

#include <stdint.h>
#include <stddef.h>

#include <glib.h>
#include <imquic/moq.h>

#include "moq-utils.h"

#define MOQ_LOC_SVC_MAX_TEMPORAL_LAYERS 4
#define MOQ_LOC_SVC_MAX_SPATIAL_LAYERS   3

typedef struct moq_loc_svc_layer {
	uint8_t temporal_id;
	uint8_t spatial_id;
	gboolean is_keyframe;
} moq_loc_svc_layer;

typedef struct moq_loc_svc_config {
	gboolean enabled;
	imquic_demo_video_codec codec;
	int temporal_layers;
	int spatial_layers;
	int max_send_temporal_layer;
	int max_send_spatial_layer;
} moq_loc_svc_config;

void moq_loc_svc_config_init(moq_loc_svc_config *cfg);
gboolean moq_loc_svc_config_validate(const moq_loc_svc_config *cfg);

gboolean moq_loc_svc_use_multi_spatial_encode(const moq_loc_svc_config *cfg);

void moq_loc_svc_spatial_layer_dimensions(int full_width, int full_height, int spatial_layers,
	int spatial_id, int *width, int *height);
int moq_loc_svc_spatial_layer_bitrate(int total_bitrate, int full_width, int full_height,
	int spatial_layers, int spatial_id);

gboolean moq_loc_svc_layer_within_send_limits(const moq_loc_svc_config *cfg,
	const moq_loc_svc_layer *layer);

int moq_loc_svc_target_decode_spatial_layer(int spatial_layers, int max_spatial_layer);
gboolean moq_loc_svc_layer_should_decode(const moq_loc_svc_layer *layer,
	int spatial_layers, int max_spatial_layer);
gboolean moq_loc_svc_object_should_decode(imquic_moq_object *object,
	int spatial_layers, int max_spatial_layer);

uint64_t moq_loc_svc_subgroup_id(uint8_t spatial_id, uint8_t temporal_id);
void moq_loc_svc_unpack_subgroup(uint64_t subgroup_id, uint8_t *spatial_id, uint8_t *temporal_id);

uint64_t moq_loc_svc_frame_marking_value(const moq_loc_svc_layer *layer);
gboolean moq_loc_svc_layer_from_frame_marking(uint64_t value, moq_loc_svc_layer *layer);

gboolean moq_loc_svc_layer_from_properties(GList *properties, moq_loc_svc_layer *layer);
void moq_loc_svc_set_frame_marking(imquic_moq_property *prop, const moq_loc_svc_layer *layer);

int moq_loc_svc_parse_packet(imquic_demo_video_codec codec, const uint8_t *data, size_t len,
	gboolean avcc, moq_loc_svc_layer *layer);

const char *moq_loc_svc_catalog_codec(imquic_demo_video_codec codec);

gboolean moq_loc_svc_is_svc_codec(imquic_demo_video_codec codec);

gboolean moq_loc_svc_object_within_layer(imquic_demo_video_codec codec, imquic_moq_object *object,
	int max_temporal_layer, int max_spatial_layer);

int moq_loc_svc_object_temporal_layer(imquic_moq_object *object);
int moq_loc_svc_object_spatial_layer(imquic_moq_object *object);

/* Adaptive SVC layer selection for weak networks */
typedef struct moq_loc_svc_abr moq_loc_svc_abr;

moq_loc_svc_abr *moq_loc_svc_abr_create(int temporal_layers, int spatial_layers);
void moq_loc_svc_abr_destroy(moq_loc_svc_abr *abr);
void moq_loc_svc_abr_set_temporal_layers(moq_loc_svc_abr *abr, int temporal_layers);
void moq_loc_svc_abr_set_spatial_layers(moq_loc_svc_abr *abr, int spatial_layers);
void moq_loc_svc_abr_reconfigure(moq_loc_svc_abr *abr, int temporal_layers, int spatial_layers);
void moq_loc_svc_abr_update(moq_loc_svc_abr *abr, imquic_connection *conn,
	uint64_t send_ok, uint64_t send_fail, double media_loss_rate);
int moq_loc_svc_abr_get_temporal_layers(const moq_loc_svc_abr *abr);
int moq_loc_svc_abr_get_spatial_layers(const moq_loc_svc_abr *abr);
int moq_loc_svc_abr_get_max_temporal_layer(const moq_loc_svc_abr *abr);
int moq_loc_svc_abr_get_max_spatial_layer(const moq_loc_svc_abr *abr);
double moq_loc_svc_abr_get_stress(const moq_loc_svc_abr *abr);

gboolean moq_loc_svc_object_within_limits(imquic_moq_object *object,
	int max_temporal_layer, int max_spatial_layer);

struct AVCodecContext;
void moq_loc_svc_configure_encoder(struct AVCodecContext *ctx, const moq_loc_svc_config *cfg);

#endif
