/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * RTP helpers for RoQ examples
 *
 */

#include <arpa/inet.h>
#include <string.h>

#include "roq-utils.h"

#define IMQUIC_ROQ_RTP_MAX_PACKET 1200
#define IMQUIC_ROQ_H264_CLOCK 90000

gboolean imquic_roq_is_rtp(uint8_t *buf, guint len) {
	if(len < 12)
		return FALSE;
	imquic_roq_rtp_header *header = (imquic_roq_rtp_header *)buf;
	return ((header->type < 64) || (header->type >= 96));
}

void imquic_roq_rtp_state_init(imquic_roq_rtp_state *state, uint8_t payload_type, uint32_t ssrc) {
	if(state == NULL)
		return;
	state->payload_type = payload_type;
	state->ssrc = ssrc;
	state->seq_number = (uint16_t)g_random_int_range(0, G_MAXUINT16);
	state->timestamp = (uint32_t)g_random_int_range(0, G_MAXINT32);
}

size_t imquic_roq_rtp_build_packet(imquic_roq_rtp_state *state, uint8_t *buffer, size_t blen,
		const uint8_t *payload, size_t payload_len, gboolean marker, uint32_t timestamp_increment) {
	if(state == NULL || buffer == NULL || payload == NULL || payload_len == 0)
		return 0;
	if(blen < 12 + payload_len)
		return 0;
	imquic_roq_rtp_header *rtp = (imquic_roq_rtp_header *)buffer;
	memset(rtp, 0, 12);
	rtp->version = 2;
	rtp->type = state->payload_type;
	rtp->markerbit = marker ? 1 : 0;
	rtp->seq_number = htons(state->seq_number++);
	state->timestamp += timestamp_increment;
	rtp->timestamp = htonl(state->timestamp);
	rtp->ssrc = htonl(state->ssrc);
	memcpy(buffer + 12, payload, payload_len);
	return 12 + payload_len;
}

static size_t imquic_roq_h264_nal_start(const uint8_t *data, size_t len, size_t offset) {
	if(offset + 3 >= len)
		return len;
	if(data[offset] == 0x00 && data[offset + 1] == 0x00 && data[offset + 2] == 0x01)
		return offset + 3;
	if(offset + 4 < len && data[offset] == 0x00 && data[offset + 1] == 0x00 &&
			data[offset + 2] == 0x00 && data[offset + 3] == 0x01)
		return offset + 4;
	return offset + 1;
}

static size_t imquic_roq_h264_next_nal(const uint8_t *data, size_t len, size_t offset, size_t *nal_offset, size_t *nal_size) {
	size_t start = imquic_roq_h264_nal_start(data, len, offset);
	if(start >= len)
		return len;
	size_t end = start;
	while(end + 3 < len) {
		if((data[end] == 0x00 && data[end + 1] == 0x00 && data[end + 2] == 0x01) ||
				(data[end] == 0x00 && data[end + 1] == 0x00 && data[end + 2] == 0x00 && data[end + 3] == 0x01))
			break;
		end++;
	}
	if(end + 3 >= len)
		end = len;
	*nal_offset = start;
	*nal_size = end - start;
	return end;
}

static gboolean imquic_roq_h264_send_single_nal(imquic_roq_rtp_state *state, const uint8_t *nal, size_t nal_size,
		gboolean marker, uint32_t ts_increment, imquic_roq_rtp_packet_cb cb, void *user_data) {
	uint8_t packet[IMQUIC_ROQ_RTP_MAX_PACKET];
	size_t plen = imquic_roq_rtp_build_packet(state, packet, sizeof(packet),
		nal, nal_size, marker, ts_increment);
	if(plen == 0)
		return FALSE;
	return cb(packet, plen, user_data);
}

static gboolean imquic_roq_h264_send_fu_a(imquic_roq_rtp_state *state, const uint8_t *nal, size_t nal_size,
		gboolean marker, uint32_t ts_increment, imquic_roq_rtp_packet_cb cb, void *user_data) {
	if(nal_size < 2)
		return FALSE;
	uint8_t nal_type = nal[0] & 0x1F;
	uint8_t nri = nal[0] & 0x60;
	size_t max_payload = IMQUIC_ROQ_RTP_MAX_PACKET - 12 - 2;
	size_t offset = 1;
	gboolean first = TRUE;
	while(offset < nal_size) {
		size_t chunk = nal_size - offset;
		if(chunk > max_payload)
			chunk = max_payload;
		gboolean last = (offset + chunk >= nal_size);
		uint8_t payload[IMQUIC_ROQ_RTP_MAX_PACKET];
		payload[0] = (uint8_t)(28 | nri);
		payload[1] = (uint8_t)((first ? 0x80 : 0x00) | (last ? 0x40 : 0x00) | nal_type);
		memcpy(payload + 2, nal + offset, chunk);
		uint8_t packet[IMQUIC_ROQ_RTP_MAX_PACKET];
		size_t plen = imquic_roq_rtp_build_packet(state, packet, sizeof(packet),
			payload, chunk + 2, marker && last, first ? ts_increment : 0);
		if(plen == 0 || !cb(packet, plen, user_data))
			return FALSE;
		first = FALSE;
		offset += chunk;
	}
	return TRUE;
}

gboolean imquic_roq_rtp_packetize_h264_annexb(imquic_roq_rtp_state *state,
		const uint8_t *annexb, size_t len, int fps, imquic_roq_rtp_packet_cb cb, void *user_data) {
	if(state == NULL || annexb == NULL || len == 0 || cb == NULL || fps <= 0)
		return FALSE;
	uint32_t ts_increment = IMQUIC_ROQ_H264_CLOCK / (uint32_t)fps;
	size_t offset = 0, nal_offset = 0, nal_size = 0;
	size_t last_nal_offset = 0, last_nal_size = 0;
	/* Find the last NAL in the access unit to set the marker bit */
	while(offset < len) {
		size_t next = imquic_roq_h264_next_nal(annexb, len, offset, &nal_offset, &nal_size);
		if(nal_size == 0) {
			offset = next;
			continue;
		}
		last_nal_offset = nal_offset;
		last_nal_size = nal_size;
		offset = next;
	}
	offset = 0;
	while(offset < len) {
		size_t next = imquic_roq_h264_next_nal(annexb, len, offset, &nal_offset, &nal_size);
		if(nal_size == 0) {
			offset = next;
			continue;
		}
		gboolean marker = (nal_offset == last_nal_offset && nal_size == last_nal_size);
		const uint8_t *nal = annexb + nal_offset;
		gboolean ok = FALSE;
		if(nal_size + 12 <= IMQUIC_ROQ_RTP_MAX_PACKET) {
			ok = imquic_roq_h264_send_single_nal(state, nal, nal_size, marker, ts_increment, cb, user_data);
			ts_increment = 0;
		} else {
			ok = imquic_roq_h264_send_fu_a(state, nal, nal_size, marker, ts_increment, cb, user_data);
			ts_increment = 0;
		}
		if(!ok)
			return FALSE;
		offset = next;
	}
	return TRUE;
}

#define IMQUIC_ROQ_VP9_CLOCK 90000

static size_t imquic_roq_vp9_descriptor_len(const uint8_t *payload, size_t payload_len) {
	size_t offset = 1;
	if(payload == NULL || payload_len < 1)
		return 0;
	if(payload[0] & 0x80) {
		if(payload_len < 2)
			return 0;
		offset += (payload[1] & 0x80) ? 2 : 1;
	}
	if((payload[0] & 0x20) && !(payload[0] & 0x10))
		offset += 1;
	return offset <= payload_len ? offset : 0;
}

gboolean imquic_roq_rtp_packetize_vp9(imquic_roq_rtp_state *state,
		const uint8_t *frame, size_t len, int fps, gboolean keyframe,
		imquic_roq_rtp_packet_cb cb, void *user_data) {
	uint8_t packet[IMQUIC_ROQ_RTP_MAX_PACKET], payload[IMQUIC_ROQ_RTP_MAX_PACKET];
	size_t max_chunk = 0, offset = 0;
	uint32_t ts_increment = 0;
	uint8_t p_bit = 0;
	if(state == NULL || frame == NULL || len == 0 || cb == NULL || fps <= 0)
		return FALSE;
	ts_increment = IMQUIC_ROQ_VP9_CLOCK / (uint32_t)fps;
	p_bit = keyframe ? 0x00 : 0x40;
	max_chunk = IMQUIC_ROQ_RTP_MAX_PACKET - 12 - 1;
	if(len <= max_chunk) {
		payload[0] = (uint8_t)(0x0C | p_bit);
		memcpy(payload + 1, frame, len);
		size_t plen = imquic_roq_rtp_build_packet(state, packet, sizeof(packet),
			payload, len + 1, TRUE, ts_increment);
		return plen > 0 && cb(packet, plen, user_data);
	}
	while(offset < len) {
		size_t chunk = len - offset;
		gboolean first = (offset == 0);
		gboolean last = FALSE;
		uint8_t desc = p_bit;
		if(chunk > max_chunk)
			chunk = max_chunk;
		last = (offset + chunk >= len);
		if(first)
			desc |= 0x08;
		if(last)
			desc |= 0x04;
		payload[0] = desc;
		memcpy(payload + 1, frame + offset, chunk);
		size_t plen = imquic_roq_rtp_build_packet(state, packet, sizeof(packet),
			payload, chunk + 1, last, first ? ts_increment : 0);
		if(plen == 0 || !cb(packet, plen, user_data))
			return FALSE;
		offset += chunk;
	}
	return TRUE;
}

void imquic_roq_vp9_depay_reset(imquic_roq_vp9_depay *depay) {
	if(depay == NULL)
		return;
	if(depay->frame != NULL)
		g_byte_array_set_size(depay->frame, 0);
	depay->in_frame = FALSE;
}

gboolean imquic_roq_rtp_depay_vp9(imquic_roq_vp9_depay *depay,
		const uint8_t *payload, size_t payload_len, uint16_t seq, uint32_t timestamp,
		gboolean rtp_marker, uint8_t **frame, size_t *frame_len) {
	size_t hdr_len = 0, data_len = 0;
	gboolean start = FALSE, end = FALSE;
	if(depay == NULL || payload == NULL || payload_len == 0 || frame == NULL || frame_len == NULL)
		return FALSE;
	*frame = NULL;
	*frame_len = 0;
	if(depay->frame == NULL)
		depay->frame = g_byte_array_new();
	hdr_len = imquic_roq_vp9_descriptor_len(payload, payload_len);
	if(hdr_len == 0 || hdr_len >= payload_len)
		return FALSE;
	start = (payload[0] & 0x08) != 0;
	end = (payload[0] & 0x04) != 0;
	if(!end && rtp_marker && depay->in_frame)
		end = TRUE;
	if(depay->last_ts != timestamp) {
		imquic_roq_vp9_depay_reset(depay);
		depay->last_ts = timestamp;
	} else if(depay->in_frame && (uint16_t)(depay->last_seq + 1) != seq) {
		imquic_roq_vp9_depay_reset(depay);
	}
	depay->last_seq = seq;
	if(start || (end && depay->frame->len == 0))
		imquic_roq_vp9_depay_reset(depay);
	data_len = payload_len - hdr_len;
	g_byte_array_append(depay->frame, payload + hdr_len, (guint)data_len);
	depay->in_frame = TRUE;
	if(!end)
		return FALSE;
	*frame = depay->frame->data;
	*frame_len = depay->frame->len;
	depay->in_frame = FALSE;
	return TRUE;
}

#define IMQUIC_ROQ_SVC_FEEDBACK_MAGIC 0x46435653

size_t imquic_roq_rtp_build_svc_feedback(imquic_roq_rtp_state *state, uint8_t *buffer, size_t blen,
		uint8_t max_temporal_layer) {
	uint8_t payload[8];
	uint32_t magic = htonl(IMQUIC_ROQ_SVC_FEEDBACK_MAGIC);
	if(state == NULL || buffer == NULL)
		return 0;
	memcpy(payload, &magic, 4);
	payload[4] = 1;
	payload[5] = max_temporal_layer;
	payload[6] = 0;
	payload[7] = 0;
	return imquic_roq_rtp_build_packet(state, buffer, blen, payload, sizeof(payload), TRUE, 0);
}

gboolean imquic_roq_rtp_is_svc_feedback(uint64_t flow_id, uint8_t payload_type) {
	return flow_id == IMQUIC_ROQ_SVC_FEEDBACK_FLOW_ID &&
		payload_type == IMQUIC_ROQ_SVC_FEEDBACK_PAYLOAD_TYPE;
}

gboolean imquic_roq_rtp_parse_svc_feedback(uint8_t *rtp, size_t rtp_len, uint8_t *max_temporal_layer) {
	size_t payload_len = 0;
	size_t payload_offset = 0;
	uint32_t magic = 0;
	const uint8_t *payload = NULL;
	if(rtp == NULL || rtp_len < 12 + 8 || max_temporal_layer == NULL)
		return FALSE;
	if(!imquic_roq_is_rtp(rtp, (guint)rtp_len))
		return FALSE;
	payload_offset = 12;
	if(payload_offset > rtp_len)
		return FALSE;
	payload_len = rtp_len - payload_offset;
	if(payload_len < 8)
		return FALSE;
	payload = rtp + payload_offset;
	memcpy(&magic, payload, 4);
	magic = ntohl(magic);
	if(magic != IMQUIC_ROQ_SVC_FEEDBACK_MAGIC || payload[4] != 1)
		return FALSE;
	*max_temporal_layer = payload[5];
	return TRUE;
}
