/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Command line options for imquic-roq-receiver
 *
 */

#ifndef ROQ_RECEIVER_OPTIONS
#define ROQ_RECEIVER_OPTIONS

#include <glib.h>

/*! \brief Struct containing the parsed command line options */
typedef struct demo_options {
	gboolean client;
	const char *ip;
	int port;
	const char *remote_host;
	int remote_port;
	const char *sni;
	gboolean raw_quic;
	gboolean webtransport;
	const char *path;
	const char *cert_pem;
	const char *cert_key;
	const char *ticket_file;
	const char *secrets_log;
	const char *qlog_path;
	const char **qlog_logging;
	gboolean qlog_sequential;
	gboolean qlog_roq_packets;
	gboolean quiet;
	gboolean echo;
#ifdef HAVE_ROQ_DISPLAY
	gboolean display;
	gboolean no_audio;
	int64_t audio_flow;
	int audio_pt;
	int64_t video_flow;
	int video_pt;
	const char *video_codec;
	int svc_max_temporal_layer;
	int svc_max_spatial_layer;
	gboolean no_svc_adaptive;
	int window_width;
	int window_height;
	gboolean debug_ffmpeg;
#endif
	int debug_level;
	gboolean debug_locks;
	gboolean debug_refcounts;
} demo_options;

/* Helper method to parse the command line options */
gboolean demo_options_parse(demo_options *opts, int argc, char *argv[]);

/* Helper method to show the application usage */
void demo_options_show_usage(void);

/*! Helper method to get rid of the options parser resources */
void demo_options_destroy(void);

#endif
