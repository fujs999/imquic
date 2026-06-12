/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Command line options for imquic-roq-sender
 *
 */

#ifndef ROQ_SENDER_OPTIONS
#define ROQ_SENDER_OPTIONS

#include <glib.h>

/*! \brief Struct containing the parsed command line options */
typedef struct demo_options {
	gboolean capture;
	int audio_flow;
	int audio_port;
	int audio_bitrate;
	int audio_pt;
	int video_flow;
	int video_port;
	int video_bitrate;
	int video_pt;
	int width;
	int height;
	int video_framerate;
	const char *video_format;
	const char *video_device;
	const char *video_encode_device;
	const char *video_resolution;
	const char *video_codec;
	int svc_temporal_layers;
	int svc_spatial_layers;
	const char *multiplexing;
	int timeout;
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
	char *cc_algo;
	char *cc_algo_option;
	const char *qlog_path;
	const char **qlog_logging;
	gboolean qlog_sequential;
	gboolean qlog_roq_packets;
	gboolean quiet;
	gboolean no_adaptive;
	gboolean no_adaptive_resolution;
	gboolean no_adaptive_bitrate;
	gboolean no_adaptive_framerate;
	gboolean debug_ffmpeg;
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
