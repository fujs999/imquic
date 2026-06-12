/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Command line options for imquic-roq-sender
 *
 */

#include "roq-sender-options.h"

static GOptionContext *opts = NULL;

gboolean demo_options_parse(demo_options *options, int argc, char *argv[]) {
	/* Supported command-line arguments */
	GOptionEntry opt_entries[] = {
#ifdef HAVE_ROQ_CAPTURE
		{ "capture", 0, 0, G_OPTION_ARG_NONE, &options->capture, "Capture audio/video locally, encode and send as RTP (default=no)", NULL },
		{ "audio-bitrate", 'B', 0, G_OPTION_ARG_INT, &options->audio_bitrate, "When capturing, audio encoding bitrate in bits per second (default=32000)", "bps" },
		{ "video-bitrate", 'E', 0, G_OPTION_ARG_INT, &options->video_bitrate, "When capturing, video encoding bitrate in bits per second (default=1048576)", "bps" },
		{ "video-format", 'f', 0, G_OPTION_ARG_STRING, &options->video_format, "When capturing, video input format using FFmpeg names (default=v4l2)", "format" },
		{ "video-device", 'i', 0, G_OPTION_ARG_STRING, &options->video_device, "When capturing, video device to capture from (default=/dev/video0)", "device" },
		{ "video-encode-device", 0, 0, G_OPTION_ARG_STRING, &options->video_encode_device, "When capturing, V4L2 M2M encoder device (RK3588: /dev/video-enc0)", "device" },
		{ "video-resolution", 'W', 0, G_OPTION_ARG_STRING, &options->video_resolution, "When capturing, video resolution to capture (default=640x480)", "resolution" },
		{ "video-framerate", 'F', 0, G_OPTION_ARG_INT, &options->video_framerate, "When capturing, video framerate in frames per second (default=25)", "fps" },
		{ "video-codec", 'e', 0, G_OPTION_ARG_STRING, &options->video_codec, "When capturing, video codec to use (default=h264-annexb)", "h264-annexb|h264-svc|vp9|vp9-svc" },
		{ "svc-temporal-layers", 0, 0, G_OPTION_ARG_INT, &options->svc_temporal_layers, "Number of SVC temporal layers for h264-svc/vp9-svc (default=2, max=4)", "2-4" },
		{ "svc-spatial-layers", 0, 0, G_OPTION_ARG_INT, &options->svc_spatial_layers, "Number of SVC spatial layers for h264-svc/vp9-svc (default=1, max=3)", "1-3" },
		{ "audio-pt", 'P', 0, G_OPTION_ARG_INT, &options->audio_pt, "When capturing, RTP payload type for Opus audio (default=111)", "pt" },
		{ "video-pt", 'T', 0, G_OPTION_ARG_INT, &options->video_pt, "When capturing, RTP payload type for video (default=96)", "pt" },
		{ "debug-ffmpeg", 'D', 0, G_OPTION_ARG_NONE, &options->debug_ffmpeg, "When capturing, verbosely debug FFmpeg (default=no)", NULL },
#endif
		{ "audio-port", 'a', 0, G_OPTION_ARG_INT, &options->audio_port, "Port to bind to for incoming audio RTP packets (default=none)", "port" },
		{ "audio-flow", 'A', 0, G_OPTION_ARG_INT, &options->audio_flow, "Flow ID of the audio RTP stream (default=none)", "number" },
		{ "video-port", 'v', 0, G_OPTION_ARG_INT, &options->video_port, "Port to bind to for incoming video RTP packets (default=none)", "port" },
		{ "video-flow", 'V', 0, G_OPTION_ARG_INT, &options->video_flow, "Flow ID of the video RTP stream (default=none)", "number" },
		{ "multiplexing", 'm', 0, G_OPTION_ARG_STRING, &options->multiplexing, "RTP multiplexing (datagram, stream or streams; default=datagram)", "mode" },
		{ "timeout", 't', 0, G_OPTION_ARG_INT, &options->timeout, "Automatically shutdown the endpoint if no RTP packets arrive for this amount of seconds (default=no timeout)", "seconds" },
		{ "client", 'o', 0, G_OPTION_ARG_NONE, &options->client, "Act as a QUIC client, not as a server (default=server)", NULL },
		{ "bind", 'b', 0, G_OPTION_ARG_STRING, &options->ip, "Local IP address to bind to (default=all interfaces)", "IP" },
		{ "port", 'p', 0, G_OPTION_ARG_INT, &options->port, "Local port to bind to (default=0, random)", "port" },
		{ "remote-host", 'r', 0, G_OPTION_ARG_STRING, &options->remote_host, "When acting as a client, QUIC server to connect to (default=none)", "IP" },
		{ "remote-port", 'R', 0, G_OPTION_ARG_INT, &options->remote_port, "When acting as a client, port of the QUIC server (default=none)", "port" },
		{ "sni", 'S', 0, G_OPTION_ARG_STRING, &options->sni, "SNI to use (default=localhost)", "sni" },
		{ "raw-quic", 'q', 0, G_OPTION_ARG_NONE, &options->raw_quic, "Whether raw QUIC should be offered for the RoQ connection or not (default=no)", NULL },
		{ "webtransport", 'w', 0, G_OPTION_ARG_NONE, &options->webtransport, "Whether WebTransport should be offered for the RoQ connection or not (default=no)", NULL },
		{ "path", 'H', 0, G_OPTION_ARG_STRING, &options->path, "In case WebTransport is used, path to use for the HTTP/3 request (default=/)", "HTTP/3 path" },
		{ "cert-pem", 'c', 0, G_OPTION_ARG_STRING, &options->cert_pem, "Certificate to use (default=none)", "path" },
		{ "cert-key", 'k', 0, G_OPTION_ARG_STRING, &options->cert_key, "Certificate key to use (default=none)", "path" },
		{ "zero-rtt", '0', 0, G_OPTION_ARG_STRING, &options->ticket_file, "Whether early data via 0-RTT should be supported, and, when acting as client, what file to use for writing/reading the session ticket (default=none)", "path" },
		{ "secrets-log", 's', 0, G_OPTION_ARG_STRING, &options->secrets_log, "Save the exchanged secrets to a file compatible with Wireshark (default=none)", "path" },
		{ "cc-algo", 'G', 0, G_OPTION_ARG_STRING, &options->cc_algo, "Congestion control algorithm to use (default=picoquic default, usually bbr)", "bbr|cubic|dcubic|prague|newreno|..." },
		{ "cc-option", 'O', 0, G_OPTION_ARG_STRING, &options->cc_algo_option, "Optional congestion control algorithm options string (default=none)", "options" },
		{ "qlog-path", 'Q', 0, G_OPTION_ARG_STRING, &options->qlog_path, "Path to a folder where to save QLOG files for this connection (default=none)", "path" },
		{ "qlog-logging", 'l', 0, G_OPTION_ARG_STRING_ARRAY, &options->qlog_logging, "Save these events to QLOG (can be called multiple times to save multiple things; default=none)", "quic|http3|roq" },
		{ "qlog-sequential", 'J', 0, G_OPTION_ARG_NONE, &options->qlog_sequential, "Whether sequential JSON should be used for the QLOG file, instead of regular JSON (default=no)", NULL },
		{ "qlog-rtp-packets", 'y', 0, G_OPTION_ARG_NONE, &options->qlog_roq_packets, "Whether the payload of RoQ RTP packets should be saved to QLOG file (default=no)", NULL },
		{ "quiet", 'Z', 0, G_OPTION_ARG_NONE, &options->quiet, "If set, don't print about incoming/outgoing RTP packets on stdout (default=no)", NULL },
		{ "no-adaptive", 0, 0, G_OPTION_ARG_NONE, &options->no_adaptive, "When capturing, disable adaptive bitrate/framerate/resolution control (default=adaptive enabled)", NULL },
		{ "no-adaptive-resolution", 0, 0, G_OPTION_ARG_NONE, &options->no_adaptive_resolution, "When capturing, disable adaptive resolution only (default=enabled when adaptive is on)", NULL },
		{ "no-adaptive-bitrate", 0, 0, G_OPTION_ARG_NONE, &options->no_adaptive_bitrate, "When capturing, disable adaptive video bitrate only (default=enabled when adaptive is on)", NULL },
		{ "no-adaptive-framerate", 0, 0, G_OPTION_ARG_NONE, &options->no_adaptive_framerate, "When capturing, disable adaptive framerate only (default=enabled when adaptive is on)", NULL },
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
