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
