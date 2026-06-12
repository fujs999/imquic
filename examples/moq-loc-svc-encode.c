/*
 * imquic
 *
 * SVC encoder configuration (requires libavcodec)
 *
 */

#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>

#include <imquic/debug.h>

#include "moq-loc-svc.h"

static gboolean moq_loc_svc_encoder_option_available(AVCodecContext *ctx, const char *name) {
	if(ctx == NULL || ctx->priv_data == NULL || name == NULL)
		return FALSE;
	return av_opt_find(ctx->priv_data, name, NULL, 0, AV_OPT_SEARCH_CHILDREN) != NULL;
}

static int moq_loc_svc_build_vp9_ts_parameters(char *buf, size_t buflen, int temporal_layers,
		int total_bitrate_bps) {
	char br[128], dec[64];
	int total_kbps = 0, i = 0, br_len = 0, dec_len = 0;
	if(buf == NULL || buflen == 0 || temporal_layers < 2)
		return -1;
	total_kbps = total_bitrate_bps / 1024;
	if(total_kbps < temporal_layers)
		total_kbps = temporal_layers;
	br[0] = '\0';
	dec[0] = '\0';
	for(i = 0; i < temporal_layers; i++) {
		int layer_br = total_kbps / (1 << (temporal_layers - 1 - i));
		int dec_val = 1 << (temporal_layers - 1 - i);
		br_len += g_snprintf(br + br_len, sizeof(br) - br_len, "%s%d", i ? "," : "", layer_br);
		dec_len += g_snprintf(dec + dec_len, sizeof(dec) - dec_len, "%s%d", i ? "," : "", dec_val);
	}
	return g_snprintf(buf, buflen,
		"ts_number_layers=%d:ts_target_bitrate=%s:ts_rate_decimator=%s:ts_layering_mode=3",
		temporal_layers, br, dec);
}

void moq_loc_svc_configure_encoder(AVCodecContext *ctx, const moq_loc_svc_config *cfg) {
	char layers[8], ts_buf[256];
	if(ctx == NULL || cfg == NULL || !cfg->enabled)
		return;
	if(cfg->codec == DEMO_H264_SVC) {
		av_opt_set(ctx->priv_data, "profile", "high", AV_OPT_SEARCH_CHILDREN);
		av_opt_set_int(ctx->priv_data, "allow_skip_frames", 1, AV_OPT_SEARCH_CHILDREN);
		g_snprintf(layers, sizeof(layers), "%d", cfg->temporal_layers);
		av_opt_set(ctx->priv_data, "slices", layers, AV_OPT_SEARCH_CHILDREN);
	} else if(cfg->codec == DEMO_VP9_SVC) {
		av_opt_set(ctx->priv_data, "lag-in-frames", "25", AV_OPT_SEARCH_CHILDREN);
		av_opt_set_int(ctx->priv_data, "auto-alt-ref", 1, AV_OPT_SEARCH_CHILDREN);
		av_opt_set(ctx->priv_data, "temporal-aq", "1", AV_OPT_SEARCH_CHILDREN);
		if(moq_loc_svc_encoder_option_available(ctx, "ts-parameters")) {
			if(moq_loc_svc_build_vp9_ts_parameters(ts_buf, sizeof(ts_buf),
					cfg->temporal_layers, (int)ctx->bit_rate) < 0 ||
					av_opt_set(ctx->priv_data, "ts-parameters", ts_buf, AV_OPT_SEARCH_CHILDREN) < 0) {
				IMQUIC_LOG(IMQUIC_LOG_WARN, "Failed to configure VP9 SVC via ts-parameters\n");
			} else {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "VP9 SVC temporal scaling: %s\n", ts_buf);
			}
		} else if(moq_loc_svc_encoder_option_available(ctx, "vp9-temporal-layers")) {
			IMQUIC_LOG(IMQUIC_LOG_WARN,
				"VP9 SVC: ts-parameters not available, falling back to legacy vp9-temporal-layers\n");
			g_snprintf(layers, sizeof(layers), "%d", cfg->temporal_layers);
			if(av_opt_set(ctx->priv_data, "vp9-temporal-layers", layers, AV_OPT_SEARCH_CHILDREN) < 0)
				IMQUIC_LOG(IMQUIC_LOG_WARN, "Failed to set vp9-temporal-layers for VP9 SVC\n");
		} else {
			IMQUIC_LOG(IMQUIC_LOG_WARN,
				"VP9 SVC: libvpx-vp9 lacks ts-parameters and vp9-temporal-layers, temporal scalability disabled\n");
		}
	}
}
