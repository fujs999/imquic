/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * RTP helpers for RoQ examples
 *
 */

#ifndef ROQ_UTILS
#define ROQ_UTILS

#if defined (__MACH__) || defined(__FreeBSD__)
#include <machine/endian.h>
#define __BYTE_ORDER BYTE_ORDER
#define __BIG_ENDIAN BIG_ENDIAN
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#else
#include <endian.h>
#endif

#include <glib.h>

/* RTP header (RFC 3550) */
typedef struct imquic_roq_rtp_header {
#if __BYTE_ORDER == __BIG_ENDIAN
	uint16_t version:2;
	uint16_t padding:1;
	uint16_t extension:1;
	uint16_t csrccount:4;
	uint16_t markerbit:1;
	uint16_t type:7;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	uint16_t csrccount:4;
	uint16_t extension:1;
	uint16_t padding:1;
	uint16_t version:2;
	uint16_t type:7;
	uint16_t markerbit:1;
#endif
	uint16_t seq_number;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t csrc[0];
} imquic_roq_rtp_header;

/* Per-stream RTP sequencing state */
typedef struct imquic_roq_rtp_state {
	uint8_t payload_type;
	uint32_t ssrc;
	uint16_t seq_number;
	uint32_t timestamp;
} imquic_roq_rtp_state;

typedef gboolean (*imquic_roq_rtp_packet_cb)(uint8_t *rtp, size_t rtp_len, void *user_data);

typedef struct imquic_roq_vp9_depay {
	GByteArray *frame;
	uint32_t last_ts;
	uint16_t last_seq;
	gboolean in_frame;
} imquic_roq_vp9_depay;

typedef struct imquic_roq_vp8_depay {
	GByteArray *frame;
	uint32_t last_ts;
	uint16_t last_seq;
	gboolean in_frame;
} imquic_roq_vp8_depay;

gboolean imquic_roq_is_rtp(uint8_t *buf, guint len);

void imquic_roq_rtp_state_init(imquic_roq_rtp_state *state, uint8_t payload_type, uint32_t ssrc);

size_t imquic_roq_rtp_build_packet(imquic_roq_rtp_state *state, uint8_t *buffer, size_t blen,
	const uint8_t *payload, size_t payload_len, gboolean marker, uint32_t timestamp_increment);

gboolean imquic_roq_rtp_packetize_h264_annexb(imquic_roq_rtp_state *state,
	const uint8_t *annexb, size_t len, int fps, imquic_roq_rtp_packet_cb cb, void *user_data);

gboolean imquic_roq_rtp_packetize_vp9(imquic_roq_rtp_state *state,
	const uint8_t *frame, size_t len, int fps, gboolean keyframe,
	imquic_roq_rtp_packet_cb cb, void *user_data);

gboolean imquic_roq_rtp_packetize_vp8(imquic_roq_rtp_state *state,
	const uint8_t *frame, size_t len, int fps, gboolean keyframe,
	imquic_roq_rtp_packet_cb cb, void *user_data);

void imquic_roq_vp9_depay_reset(imquic_roq_vp9_depay *depay);

void imquic_roq_vp8_depay_reset(imquic_roq_vp8_depay *depay);

gboolean imquic_roq_rtp_depay_vp8(imquic_roq_vp8_depay *depay,
	const uint8_t *payload, size_t payload_len, uint16_t seq, uint32_t timestamp,
	gboolean rtp_marker, uint8_t **frame, size_t *frame_len);

gboolean imquic_roq_rtp_depay_vp9(imquic_roq_vp9_depay *depay,
	const uint8_t *payload, size_t payload_len, uint16_t seq, uint32_t timestamp,
	gboolean rtp_marker, uint8_t **frame, size_t *frame_len);

/* RoQ SVC layer feedback (receiver -> sender) */
#define IMQUIC_ROQ_SVC_FEEDBACK_FLOW_ID 99
#define IMQUIC_ROQ_SVC_FEEDBACK_PAYLOAD_TYPE 127

#define IMQUIC_ROQ_SVC_FEEDBACK_VERSION_TEMPORAL 1
#define IMQUIC_ROQ_SVC_FEEDBACK_VERSION_LAYERS 2

#define IMQUIC_ROQ_SVC_SSRC_SPATIAL_MASK 0xFF

uint32_t imquic_roq_rtp_svc_spatial_ssrc(uint32_t base_ssrc, uint8_t spatial_id);

int imquic_roq_rtp_ssrc_spatial_id(uint32_t ssrc, int spatial_layers);

size_t imquic_roq_rtp_build_svc_feedback(imquic_roq_rtp_state *state, uint8_t *buffer, size_t blen,
	uint8_t max_temporal_layer, uint8_t max_spatial_layer);

gboolean imquic_roq_rtp_is_svc_feedback(uint64_t flow_id, uint8_t payload_type);

gboolean imquic_roq_rtp_parse_svc_feedback(uint8_t *rtp, size_t rtp_len, uint8_t *max_temporal_layer,
	uint8_t *max_spatial_layer);

#endif
