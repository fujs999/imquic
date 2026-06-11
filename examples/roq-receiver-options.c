/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Command line options for imquic-roq-receiver
 *
 */

#include "roq-receiver-options.h"

static GOptionContext *opts = NULL;

gboolean demo_options_parse(demo_options *options, int argc, char *argv[]) {
	options->svc_max_temporal_layer = -1;
	options->svc_max_spatial_layer = -1;
	/* Supported command-line arguments */
	GOptionEntry opt_entries[] = {
		{ "client", 'o', 0, G_OPTION_ARG_NONE, &options->client, "Act as a QUIC client, not as a server (default=server)", NULL },
		{ "bind", 'b', 0, G_OPTION_ARG_STRING, &options->ip, "Local IP address to bind to (default=all interfaces)", "IP" },
		{ "port", 'p', 0, G_OPTION_ARG_INT, &options->port, "Local port to bind to (default=0, random)", "port" },
		{ "remote-host", 'r', 0, G_OPTION_ARG_STRING, &options->remote_host, "When acting as a client, QUIC server to connect to (default=none)", "IP" },
		{ "remote-port", 'R', 0, G_OPTION_ARG_INT, &options->remote_port, "When acting as a client, port of the QUIC server (default=none)", "port" },
		{ "sni", 'S', 0, G_OPTION_ARG_STRING, &options->sni, "SNI to use (default=localhost)", "sni" },
		{ "raw-quic", 'q', 0, G_OPTION_ARG_NONE, &options->raw_quic, "Whether raw QUIC should be offered for RoQ connections or not (default=no)", NULL },
		{ "webtransport", 'w', 0, G_OPTION_ARG_NONE, &options->webtransport, "Whether WebTransport should be offered for the RoQ connection or not (default=no)", NULL },
		{ "path", 'H', 0, G_OPTION_ARG_STRING, &options->path, "In case WebTransport is used, path to use for the HTTP/3 request (default=/)", "HTTP/3 path" },
		{ "cert-pem", 'c', 0, G_OPTION_ARG_STRING, &options->cert_pem, "Certificate to use (default=none)", "path" },
		{ "cert-key", 'k', 0, G_OPTION_ARG_STRING, &options->cert_key, "Certificate key to use (default=none)", "path" },
		{ "zero-rtt", '0', 0, G_OPTION_ARG_STRING, &options->ticket_file, "Whether early data via 0-RTT should be supported, and, when acting as client, what file to use for writing/reading the session ticket (default=none)", "path" },
		{ "secrets-log", 's', 0, G_OPTION_ARG_STRING, &options->secrets_log, "Save the exchanged secrets to a file compatible with Wireshark (default=none)", "path" },
		{ "qlog-path", 'Q', 0, G_OPTION_ARG_STRING, &options->qlog_path, "Path to a folder where to save QLOG files for all connections (default=none)", "path" },
		{ "qlog-logging", 'l', 0, G_OPTION_ARG_STRING_ARRAY, &options->qlog_logging, "Save these events to QLOG (can be called multiple times to save multiple things; default=none)", "quic|http3|roq" },
		{ "qlog-sequential", 'J', 0, G_OPTION_ARG_NONE, &options->qlog_sequential, "Whether sequential JSON should be used for the QLOG file, instead of regular JSON (default=no)", NULL },
		{ "qlog-rtp-packets", 'y', 0, G_OPTION_ARG_NONE, &options->qlog_roq_packets, "Whether the payload of RoQ RTP packets should be saved to QLOG file (default=no)", NULL },
		{ "quiet", 'Z', 0, G_OPTION_ARG_NONE, &options->quiet, "If set, don't print about incoming/outgoing packets on stdout (default=no)", NULL },
		{ "echo", 'e', 0, G_OPTION_ARG_NONE, &options->echo, "If set, the receiver will echo incoming RTP packets back to the sender (default=no)", NULL },
#ifdef HAVE_ROQ_DISPLAY
		{ "display", 'g', 0, G_OPTION_ARG_NONE, &options->display, "Decode and play incoming audio/video (default=no)", NULL },
		{ "no-audio", 'N', 0, G_OPTION_ARG_NONE, &options->no_audio, "When displaying, disable audio playback (default=no)", NULL },
		{ "audio-flow", 'A', 0, G_OPTION_ARG_INT64, &options->audio_flow, "When displaying, flow ID of the audio RTP stream (default=0)", "number" },
		{ "audio-pt", 'P', 0, G_OPTION_ARG_INT, &options->audio_pt, "When displaying, RTP payload type for Opus audio (default=111)", "pt" },
		{ "video-flow", 'V', 0, G_OPTION_ARG_INT64, &options->video_flow, "When displaying, flow ID of the video RTP stream (default=1)", "number" },
		{ "video-pt", 'T', 0, G_OPTION_ARG_INT, &options->video_pt, "When displaying, RTP payload type for video (default=96)", "pt" },
		{ "video-codec", 'e', 0, G_OPTION_ARG_STRING, &options->video_codec, "When displaying, video codec to use (default=h264-annexb)", "h264-annexb|h264-svc|vp9|vp9-svc" },
		{ "svc-temporal-layers", 0, 0, G_OPTION_ARG_INT, &options->svc_temporal_layers, "Expected SVC temporal layers from sender (default=2, max=4)", "2-4" },
		{ "svc-max-temporal-layer", 0, 0, G_OPTION_ARG_INT, &options->svc_max_temporal_layer, "Maximum SVC temporal layer to decode (-1=all, default=-1)", "0-3" },
		{ "svc-max-spatial-layer", 0, 0, G_OPTION_ARG_INT, &options->svc_max_spatial_layer, "Maximum SVC spatial layer to decode (-1=all, default=-1)", "0-2" },
		{ "no-svc-adaptive", 0, 0, G_OPTION_ARG_NONE, &options->no_svc_adaptive, "Disable automatic SVC layer selection in weak networks (default=adaptive when --svc-max-temporal-layer is not set)", NULL },
		{ "window-width", 'X', 0, G_OPTION_ARG_INT, &options->window_width, "When displaying, window width in pixels (default=1280)", "pixels" },
		{ "window-height", 'Y', 0, G_OPTION_ARG_INT, &options->window_height, "When displaying, window height in pixels (default=720)", "pixels" },
		{ "debug-ffmpeg", 'D', 0, G_OPTION_ARG_NONE, &options->debug_ffmpeg, "When displaying, verbosely debug FFmpeg (default=no)", NULL },
#endif
		{ "debug-level", 'd', 0, G_OPTION_ARG_INT, &options->debug_level, "Debug/logging level (0=disable debugging, 7=maximum debug level; default=4)", "1-7" },
		{ "debug-locks", 'L', 0, G_OPTION_ARG_NONE, &options->debug_locks, "Whether to verbosely debug mutex/lock accesses (default=no)", NULL },
		{ "debug-refcounts", 'C', 0, G_OPTION_ARG_NONE, &options->debug_refcounts, "Whether to verbosely debug reference counting (default=no)", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL },
	};

	/* Parse the command-line arguments */
	GError *error = NULL;
	opts = g_option_context_new("");
	g_option_context_set_help_enabled(opts, TRUE);
	g_option_context_add_main_entries(opts, opt_entries, NULL);
	if(!g_option_context_parse(opts, &argc, &argv, &error)) {
		g_print("%s\n", error->message);
		g_error_free(error);
		demo_options_destroy();
		return FALSE;
	}

	/* Done */
	return TRUE;
}

void demo_options_show_usage(void) {
	if(opts == NULL)
		return;
	char *help = g_option_context_get_help(opts, TRUE, NULL);
	g_print("\n%s", help);
	g_free(help);
}

void demo_options_destroy(void) {
	if(opts != NULL)
		g_option_context_free(opts);
	opts = NULL;
}
