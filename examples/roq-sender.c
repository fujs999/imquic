/*
 * imquic
 *
 * Author:  Lorenzo Miniero <lorenzo@meetecho.com>
 * License: MIT
 *
 * Basic RoQ sender (client or server)
 *
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#include <imquic/imquic.h>
#include <imquic/roq.h>

#include "roq-sender-options.h"
#include "roq-utils.h"
#ifdef HAVE_ROQ_CAPTURE
#include "roq-capture.h"
#include "moq-loc-abr.h"
#include "moq-loc-svc.h"
#include "moq-utils.h"
#endif

/* Command line options */
static demo_options options = { 0 };

/* Signal */
static volatile int stop = 0;
static void imquic_demo_handle_signal(int signum) {
	switch(g_atomic_int_get(&stop)) {
		case 0:
			IMQUIC_PRINT("Stopping sender, please wait...\n");
			break;
		case 1:
			IMQUIC_PRINT("In a hurry? I'm trying to free resources cleanly, here!\n");
			break;
		default:
			IMQUIC_PRINT("Ok, leaving immediately...\n");
			break;
	}
	g_atomic_int_inc(&stop);
	if(g_atomic_int_get(&stop) > 2)
		exit(1);
}

/* Handled connections */
static GHashTable *connections = NULL;
static imquic_mutex mutex = IMQUIC_MUTEX_INITIALIZER;
#ifdef HAVE_ROQ_CAPTURE
static imquic_mutex send_mutex = IMQUIC_MUTEX_INITIALIZER;
static moq_loc_abr *abr = NULL;
static moq_loc_svc_abr *svc_abr = NULL;
static imquic_demo_video_codec video_codec = DEMO_H264_ANNEXB;
static GThread *abr_thread = NULL;
static uint64_t send_ok_count = 0, send_fail_count = 0, video_bytes_sent = 0;
static int applied_audio_bitrate = 0;
static void *imquic_demo_abr_thread(void *user_data);
#endif

/* RoQ multiplexing (set during init) */
static imquic_roq_multiplexing multiplexing;
static const char *mode = NULL;
static gboolean one_stream_per_packet = FALSE;

#ifdef HAVE_ROQ_CAPTURE
static volatile int capture_active = 0;
#endif

static void imquic_demo_send_rtp_to_connections(uint64_t flow_id, uint8_t *buffer, size_t bytes) {
	GHashTableIter iter;
	gpointer value;
	size_t sent = 0;
	gboolean video = (options.video_flow >= 0 && flow_id == (uint64_t)options.video_flow);
	imquic_mutex_lock(&mutex);
	g_hash_table_iter_init(&iter, connections);
	while(g_hash_table_iter_next(&iter, NULL, &value)) {
		imquic_connection *roq_conn = (imquic_connection *)value;
		sent = imquic_roq_send_rtp(roq_conn, multiplexing, flow_id, buffer, bytes, one_stream_per_packet);
#ifdef HAVE_ROQ_CAPTURE
		imquic_mutex_lock(&send_mutex);
		if(sent > 0) {
			send_ok_count++;
			if(video)
				video_bytes_sent += bytes;
		} else {
			send_fail_count++;
		}
		imquic_mutex_unlock(&send_mutex);
#endif
		if(sent == 0) {
			IMQUIC_LOG(IMQUIC_LOG_WARN, "[%s] Couldn't send RTP packet...\n",
				imquic_get_connection_name(roq_conn));
		} else if(!options.quiet) {
			imquic_roq_rtp_header *rtp = (imquic_roq_rtp_header *)buffer;
			IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]  -- [%s][flow=%"SCNu64"][%zu] ssrc=%"SCNu32", pt=%"SCNu16", seq=%"SCNu16", ts=%"SCNu32"\n",
				imquic_get_connection_name(roq_conn),
				mode, flow_id, bytes,
				ntohl(rtp->ssrc), rtp->type, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
		}
	}
	imquic_mutex_unlock(&mutex);
}

#ifdef HAVE_ROQ_CAPTURE
static void imquic_demo_capture_rtp(uint64_t flow_id, uint8_t *rtp, size_t rtp_len, void *user_data) {
	if(!g_atomic_int_get(&capture_active))
		return;
	imquic_demo_send_rtp_to_connections(flow_id, rtp, rtp_len);
}

static imquic_connection *imquic_demo_get_abr_connection(void) {
	imquic_connection *conn = NULL;
	GHashTableIter iter;
	gpointer value;
	imquic_mutex_lock(&mutex);
	g_hash_table_iter_init(&iter, connections);
	if(g_hash_table_iter_next(&iter, NULL, &value))
		conn = (imquic_connection *)value;
	imquic_mutex_unlock(&mutex);
	return conn;
}

static void *imquic_demo_abr_thread(void *user_data) {
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Starting ABR control thread\n");
	while(!g_atomic_int_get(&stop)) {
		if((abr != NULL || svc_abr != NULL) && g_atomic_int_get(&capture_active)) {
			imquic_connection *conn = imquic_demo_get_abr_connection();
			if(conn != NULL) {
				uint64_t ok = 0, fail = 0, bytes = 0;
				imquic_mutex_lock(&send_mutex);
				ok = send_ok_count;
				fail = send_fail_count;
				bytes = video_bytes_sent;
				imquic_mutex_unlock(&send_mutex);
				if(abr != NULL)
					moq_loc_abr_update(abr, conn, ok, fail, bytes);
				if(svc_abr != NULL)
					moq_loc_svc_abr_update(svc_abr, conn, ok, fail, -1.0);
			}
		}
		g_usleep(500000);
	}
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Leaving ABR control thread\n");
	return NULL;
}
#endif

/* Callbacks */
static void imquic_demo_new_connection(imquic_connection *conn, void *user_data) {
	/* Got new connection */
	imquic_connection_ref(conn);
	imquic_mutex_lock(&mutex);
	g_hash_table_insert(connections, conn, conn);
	imquic_mutex_unlock(&mutex);
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] New connection\n", imquic_get_connection_name(conn));
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s]   -- %s (%s)\n", imquic_get_connection_name(conn),
		imquic_is_connection_webtransport(conn) ? "WebTransport" : "Raw QUIC",
		imquic_is_connection_webtransport(conn) ? imquic_get_connection_wt_protocol(conn) : imquic_get_connection_alpn(conn));
#ifdef HAVE_ROQ_CAPTURE
	if(!options.no_adaptive || (svc_abr != NULL))
		imquic_enable_connection_loss_feedback(conn);
	if(options.capture && !g_atomic_int_get(&capture_active)) {
		g_atomic_int_set(&capture_active, 1);
		roq_capture_start();
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Started local audio/video capture\n");
	}
#endif
}

static void imquic_demo_rtp_incoming(imquic_connection *conn, imquic_roq_multiplexing mplex,
		uint64_t flow_id, uint8_t *bytes, size_t blen) {
	if(!imquic_roq_is_rtp(bytes, (guint)blen)) {
		IMQUIC_LOG(IMQUIC_LOG_WARN, "[%s]  -- [flow=%"SCNu64"][%zu] Not an RTP packet\n",
			imquic_get_connection_name(conn), flow_id, blen);
		return;
	}
#ifdef HAVE_ROQ_CAPTURE
	{
		uint8_t remote_max_layer = 0;
		imquic_roq_rtp_header *rtp = (imquic_roq_rtp_header *)bytes;
		if(imquic_roq_rtp_is_svc_feedback(flow_id, rtp->type) &&
				imquic_roq_rtp_parse_svc_feedback(bytes, blen, &remote_max_layer)) {
			roq_capture_set_remote_max_temporal_layer((int)remote_max_layer);
			IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Remote SVC max temporal layer: %u\n",
				imquic_get_connection_name(conn), remote_max_layer);
			return;
		}
	}
#endif
	/* If this is called, it means the receiver we're sending RTP packets to
	 * sent us something back (e.g., the imquic RoQ receiver in echo mode) */
	imquic_roq_rtp_header *rtp = (imquic_roq_rtp_header *)bytes;
	if(!options.quiet) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s][recv]  -- [%s][flow=%"SCNu64"][%zu] ssrc=%"SCNu32", pt=%d, seq=%"SCNu16", ts=%"SCNu32"\n",
			imquic_get_connection_name(conn), imquic_roq_multiplexing_str(mplex), flow_id, blen,
			ntohl(rtp->ssrc), rtp->type, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
	}
}

static void imquic_demo_connection_failed(void *user_data) {
	/* Connection failed */
	IMQUIC_LOG(IMQUIC_LOG_INFO, "Connection failed\n");
	if(options.client) {
		/* Stop here */
		g_atomic_int_inc(&stop);
	}
}

static void imquic_demo_connection_gone(imquic_connection *conn, uint64_t error_code, const char *reason) {
	/* Connection was closed */
	IMQUIC_LOG(IMQUIC_LOG_INFO, "[%s] Connection gone\n", imquic_get_connection_name(conn));
	imquic_mutex_lock(&mutex);
	if(g_hash_table_remove(connections, conn))
		imquic_connection_unref(conn);
	gboolean no_connections = (g_hash_table_size(connections) == 0);
	imquic_mutex_unlock(&mutex);
#ifdef HAVE_ROQ_CAPTURE
	if(options.capture && no_connections && g_atomic_int_get(&capture_active)) {
		g_atomic_int_set(&capture_active, 0);
		roq_capture_pause();
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Paused local audio/video capture (no active connections)\n");
	}
#endif
	if(options.client) {
		/* Stop here */
		g_atomic_int_inc(&stop);
	}
}

int main(int argc, char *argv[]) {
	/* Handle SIGINT (CTRL-C), SIGTERM (from service managers) */
	signal(SIGINT, imquic_demo_handle_signal);
	signal(SIGTERM, imquic_demo_handle_signal);

	IMQUIC_PRINT("imquic version %s\n", imquic_get_version_string_full());
	IMQUIC_PRINT("  -- %s (commit hash)\n", imquic_get_build_sha());
	IMQUIC_PRINT("  -- %s (build time)\n\n", imquic_get_build_time());

	/* Initialize some command line options defaults */
	options.debug_level = IMQUIC_LOG_INFO;
	options.audio_flow = -1;
	options.audio_port = -1;
	options.video_flow = -1;
	options.video_port = -1;
	options.audio_bitrate = 32000;
	options.video_bitrate = 1048576;
	options.video_framerate = 25;
	options.audio_pt = 111;
	options.video_pt = 96;
	/* Let's call our cmdline parser */
	if(!demo_options_parse(&options, argc, argv)) {
		demo_options_show_usage();
		demo_options_destroy();
		exit(1);
	}
	/* Logging level */
	imquic_set_log_level(options.debug_level);
	/* Debugging */
	if(options.debug_locks)
		imquic_set_lock_debugging(TRUE);
	if(options.debug_refcounts)
		imquic_set_refcount_debugging(TRUE);

	int ret = 0, audio_fd = -1, video_fd = -1;
	IMQUIC_LOG(IMQUIC_LOG_INFO, "RoQ sender will act as a %s\n", options.client ? "client (single connection)" : "server (multiple connections)");
	if(options.client && (options.remote_host == NULL || options.remote_port == 0)) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid QUIC server address (required when acting as client)\n");
		ret = 1;
		goto done;
	}
	if(!options.client && (options.cert_pem == NULL || strlen(options.cert_pem) == 0 || options.cert_key == NULL || strlen(options.cert_key) == 0)) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Missing certificate/key (required when acting as server)\n");
		ret = 1;
		goto done;
	}
	if(options.port > 65535) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid local port\n");
		ret = 1;
		goto done;
	}
	if(!options.raw_quic && !options.webtransport) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "No raw QUIC or WebTransport enabled (enable at least one)\n");
		ret = 1;
		goto done;
	}
	if(options.ticket_file != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Early data support enabled\n");
		if(options.client)
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Ticket file '%s'\n", options.ticket_file);
	}

#ifdef HAVE_ROQ_CAPTURE
	gboolean capture_audio = FALSE, capture_video = FALSE;
	if(options.capture) {
		capture_audio = (options.audio_flow >= 0);
		capture_video = (options.video_flow >= 0);
		if(!capture_audio && !capture_video) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Capture mode requires at least one of --audio-flow or --video-flow\n");
			ret = 1;
			goto done;
		}
		if(options.video_resolution == NULL)
			options.video_resolution = "640x480";
		if(sscanf(options.video_resolution, "%dx%d", &options.width, &options.height) != 2 ||
				options.width <= 0 || options.height <= 0) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid video resolution\n");
			ret = 1;
			goto done;
		}
		if(options.video_format == NULL)
			options.video_format = "v4l2";
		if(options.video_device == NULL)
			options.video_device = "/dev/video0";
		if(options.video_framerate <= 0)
			options.video_framerate = 25;
		if(options.audio_pt < 0 || options.audio_pt > 127 || options.video_pt < 0 || options.video_pt > 127) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid RTP payload type\n");
			ret = 1;
			goto done;
		}
		if(capture_video) {
			if(options.video_codec == NULL)
				options.video_codec = "h264-annexb";
			video_codec = imquic_demo_video_codec_from_str(options.video_codec);
			if(video_codec == DEMO_UNKOWN) {
				IMQUIC_LOG(IMQUIC_LOG_FATAL, "Unsupported video codec '%s'\n", options.video_codec);
				ret = 1;
				goto done;
			}
		}
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Capture mode enabled\n");
		if(capture_audio) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Audio: flow ID %d, Opus at %d bps, PT=%d\n",
				options.audio_flow, options.audio_bitrate, options.audio_pt);
		}
		if(capture_video) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Video: flow ID %d, %dx%d@%d, %s at %d bps, PT=%d\n",
				options.video_flow, options.width, options.height, options.video_framerate,
				imquic_demo_video_codec_str(video_codec), options.video_bitrate, options.video_pt);
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Video device '%s' (%s)\n", options.video_device, options.video_format);
		}
		if(capture_video && !options.no_adaptive && !moq_loc_svc_is_svc_codec(video_codec) &&
				(!options.no_adaptive_resolution || !options.no_adaptive_bitrate || !options.no_adaptive_framerate)) {
			gboolean adapt_resolution = !options.no_adaptive_resolution;
			gboolean adapt_bitrate = !options.no_adaptive_bitrate;
			gboolean adapt_framerate = !options.no_adaptive_framerate;
			abr = moq_loc_abr_create(options.width, options.height, options.video_framerate,
				options.video_bitrate, options.audio_bitrate);
			moq_loc_abr_set_adapt_flags(abr, adapt_resolution, adapt_bitrate, adapt_framerate);
			applied_audio_bitrate = options.audio_bitrate;
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Adaptive streaming enabled (targets: RTT<=150ms, jitter<=50ms, loss<=50%%)\n");
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Max quality: %dx%d@%d, %d bps video, %d bps audio\n",
				options.width, options.height, options.video_framerate,
				options.video_bitrate, applied_audio_bitrate);
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Adapt: resolution=%s, bitrate=%s, framerate=%s\n",
				adapt_resolution ? "on" : "off", adapt_bitrate ? "on" : "off",
				adapt_framerate ? "on" : "off");
		} else if(capture_video && moq_loc_svc_is_svc_codec(video_codec) && !options.no_adaptive) {
			int temporal_layers = options.svc_temporal_layers > 0 ? options.svc_temporal_layers : 2;
			int spatial_layers = options.svc_spatial_layers > 0 ? options.svc_spatial_layers : 1;
			svc_abr = moq_loc_svc_abr_create(temporal_layers, spatial_layers);
			IMQUIC_LOG(IMQUIC_LOG_INFO,
				"SVC adaptive layer selection enabled (temporal + spatial, targets: RTT<=150ms, jitter<=50ms, loss<=50%%)\n");
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- End-to-end feedback on flow %d, pt %d\n",
				IMQUIC_ROQ_SVC_FEEDBACK_FLOW_ID, IMQUIC_ROQ_SVC_FEEDBACK_PAYLOAD_TYPE);
		}
	} else
#endif
	{
		if(options.audio_port <= 0 && options.video_port <= 0) {
			IMQUIC_LOG(IMQUIC_LOG_FATAL, "No local audio/video RTP port specified (or use --capture)\n");
			ret = 1;
			goto done;
		}
	}
#ifndef HAVE_ROQ_CAPTURE
	if(options.capture) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Capture support not compiled in (install FFmpeg, Opus, SDL2 and rebuild with --enable-roq-examples)\n");
		ret = 1;
		goto done;
	}
#endif
	if(options.audio_port > 0 && options.audio_flow < 0) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid audio flow ID\n");
		ret = 1;
		goto done;
	}
	if(options.video_port > 0 && options.video_flow < 0) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Invalid video flow ID\n");
		ret = 1;
		goto done;
	}
	if(options.audio_flow >= 0 && options.video_flow >= 0 && options.audio_flow == options.video_flow) {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Audio and video flow IDs must be different\n");
		ret = 1;
		goto done;
	}
	/* We support different multiplexing modes for RoQ */
	if(options.multiplexing == NULL || !strcasecmp(options.multiplexing, "datagram")) {
		multiplexing = IMQUIC_ROQ_DATAGRAM;
		mode = "DATAGRAM";
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Multiplexing: DATAGRAM\n");
	} else if(!strcasecmp(options.multiplexing, "stream")) {
		multiplexing = IMQUIC_ROQ_STREAM;
		mode = "STREAM";
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Multiplexing: one STREAM per flow\n");
	} else if(!strcasecmp(options.multiplexing, "streams")) {
		multiplexing = IMQUIC_ROQ_STREAM;
		mode = "STREAMS";
		one_stream_per_packet = TRUE;
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Multiplexing: one STREAM per RTP packet\n");
	} else {
		IMQUIC_LOG(IMQUIC_LOG_FATAL, "Unsupported multiplexing mode '%s'\n", options.multiplexing);
		ret = 1;
		goto done;
	}

#ifndef HAVE_ROQ_CAPTURE
	if(!options.capture)
#endif
	{
		/* Create the audio and/or video sockets */
		if(options.audio_port > 0) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Audio: port %d, flow ID %d\n", options.audio_port, options.audio_flow);
			audio_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			struct sockaddr_in address = { 0 };
			address.sin_family = AF_INET;
			address.sin_port = g_htons(options.audio_port);
			address.sin_addr.s_addr = INADDR_ANY;
			if(bind(audio_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
				IMQUIC_LOG(IMQUIC_LOG_ERR, "Bind to audio port '%"SCNu16" failed... %d (%s)\n",
					options.audio_port, errno, g_strerror(errno));
				ret = 1;
				goto done;
			}
		}
		if(options.video_port > 0) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "Video: port %d, flow ID %d\n", options.video_port, options.video_flow);
			video_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
			struct sockaddr_in address = { 0 };
			address.sin_family = AF_INET;
			address.sin_port = g_htons(options.video_port);
			address.sin_addr.s_addr = INADDR_ANY;
			if(bind(video_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
				IMQUIC_LOG(IMQUIC_LOG_ERR, "Bind to video port '%"SCNu16" failed... %d (%s)\n",
					options.video_port, errno, g_strerror(errno));
				ret = 1;
				goto done;
			}
		}
	}

	/* Check if we need to create a QLOG file */
	gboolean qlog_quic = FALSE, qlog_http3 = FALSE, qlog_roq = FALSE;
	if(options.qlog_path != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Creating QLOG file(s) in '%s'\n", options.qlog_path);
		if(options.qlog_sequential)
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Using sequential JSON\n");
		int i = 0;
		while(options.qlog_logging != NULL && options.qlog_logging[i] != NULL) {
			if(!strcasecmp(options.qlog_logging[i], "quic")) {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Logging QUIC events\n");
				qlog_quic = TRUE;
			} else if(!strcasecmp(options.qlog_logging[i], "http3") && options.webtransport) {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Logging HTTP/3 events\n");
				qlog_http3 = TRUE;
			} else if(!strcasecmp(options.qlog_logging[i], "roq")) {
				IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- Logging RoQ events\n");
				qlog_roq = TRUE;
				if(options.qlog_roq_packets)
					IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- -- Logging the payload of RoQ RTP packets\n");
			}
			i++;
		}
	}
	if(options.quiet)
		IMQUIC_LOG(IMQUIC_LOG_INFO, "Quiet mode (won't print RTP packets)\n");
	IMQUIC_LOG(IMQUIC_LOG_INFO, "\n");

#ifdef HAVE_ROQ_CAPTURE
	if(options.capture) {
		roq_capture_config capture_cfg = {
			.capture_audio = capture_audio,
			.audio_flow = options.audio_flow,
			.audio_bitrate = options.audio_bitrate,
			.audio_pt = (uint8_t)options.audio_pt,
			.capture_video = capture_video,
			.video_flow = options.video_flow,
			.video_bitrate = options.video_bitrate,
			.width = options.width,
			.height = options.height,
			.video_framerate = options.video_framerate,
			.video_codec = video_codec,
			.svc_temporal_layers = options.svc_temporal_layers,
			.svc_spatial_layers = options.svc_spatial_layers,
			.video_format = options.video_format,
			.video_device = options.video_device,
			.video_encode_device = options.video_encode_device,
			.video_resolution = options.video_resolution,
			.video_pt = (uint8_t)options.video_pt,
			.debug_ffmpeg = options.debug_ffmpeg
		};
		if(roq_capture_init(&capture_cfg, imquic_demo_capture_rtp, NULL) < 0) {
			ret = 1;
			goto done;
		}
		if(abr != NULL)
			roq_capture_set_abr(abr);
		if(svc_abr != NULL)
			roq_capture_set_svc_abr(svc_abr);
		if(abr != NULL || svc_abr != NULL) {
			GError *error = NULL;
			abr_thread = g_thread_try_new("roq-abr", &imquic_demo_abr_thread, NULL, &error);
			if(error != NULL) {
				IMQUIC_LOG(IMQUIC_LOG_FATAL, "Got error %d (%s) trying to start ABR thread\n",
					error->code, error->message ? error->message : "??");
				ret = 1;
				goto done;
			}
		}
	}
#endif

	/* Initialize the library and create a client or server endpoint */
	if(imquic_init(options.secrets_log) < 0) {
		ret = 1;
		goto done;
	}
	imquic_endpoint *endpoint = NULL;
	if(options.client) {
		endpoint = imquic_create_roq_client("roq-sender-client",
			IMQUIC_CONFIG_INIT,
			IMQUIC_CONFIG_TLS_CERT, options.cert_pem,
			IMQUIC_CONFIG_TLS_KEY, options.cert_key,
			IMQUIC_CONFIG_TLS_NO_VERIFY, TRUE,
			IMQUIC_CONFIG_LOCAL_BIND, options.ip,
			IMQUIC_CONFIG_LOCAL_PORT, options.port,
			IMQUIC_CONFIG_REMOTE_HOST, options.remote_host,
			IMQUIC_CONFIG_REMOTE_PORT, options.remote_port,
			IMQUIC_CONFIG_SNI, options.sni,
			IMQUIC_CONFIG_RAW_QUIC, options.raw_quic,
			IMQUIC_CONFIG_WEBTRANSPORT, options.webtransport,
			IMQUIC_CONFIG_EARLY_DATA, (options.ticket_file != NULL),
			IMQUIC_CONFIG_TICKET_FILE, options.ticket_file,
			IMQUIC_CONFIG_HTTP3_PATH, options.path,
			IMQUIC_CONFIG_QLOG_PATH, options.qlog_path,
			IMQUIC_CONFIG_QLOG_QUIC, qlog_quic,
			IMQUIC_CONFIG_QLOG_HTTP3, qlog_http3,
			IMQUIC_CONFIG_QLOG_ROQ, qlog_roq,
			IMQUIC_CONFIG_QLOG_ROQ_PACKETS, options.qlog_roq_packets,
			IMQUIC_CONFIG_QLOG_SEQUENTIAL, options.qlog_sequential,
			IMQUIC_CONFIG_CC_ALGO, options.cc_algo,
			IMQUIC_CONFIG_CC_OPTION, options.cc_algo_option,
			IMQUIC_CONFIG_DONE, NULL);
	} else {
		endpoint = imquic_create_roq_server("roq-sender-server",
			IMQUIC_CONFIG_INIT,
			IMQUIC_CONFIG_TLS_CERT, options.cert_pem,
			IMQUIC_CONFIG_TLS_KEY, options.cert_key,
			IMQUIC_CONFIG_TLS_NO_VERIFY, TRUE,
			IMQUIC_CONFIG_LOCAL_BIND, options.ip,
			IMQUIC_CONFIG_LOCAL_PORT, options.port,
			IMQUIC_CONFIG_RAW_QUIC, options.raw_quic,
			IMQUIC_CONFIG_WEBTRANSPORT, options.webtransport,
			IMQUIC_CONFIG_QLOG_PATH, options.qlog_path,
			IMQUIC_CONFIG_QLOG_QUIC, qlog_quic,
			IMQUIC_CONFIG_QLOG_HTTP3, qlog_http3,
			IMQUIC_CONFIG_QLOG_ROQ, qlog_roq,
			IMQUIC_CONFIG_QLOG_ROQ_PACKETS, options.qlog_roq_packets,
			IMQUIC_CONFIG_QLOG_SEQUENTIAL, options.qlog_sequential,
			IMQUIC_CONFIG_EARLY_DATA, (options.ticket_file != NULL),
			IMQUIC_CONFIG_CC_ALGO, options.cc_algo,
			IMQUIC_CONFIG_CC_OPTION, options.cc_algo_option,
			IMQUIC_CONFIG_DONE, NULL);
	}
	if(endpoint == NULL) {
		ret = 1;
		goto done;
	}
	if(options.raw_quic) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "ALPN(s):\n");
		int i = 0;
		const char **alpns = imquic_get_endpoint_alpns(endpoint);
		while(alpns[i] != NULL) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- %s\n", alpns[i]);
			i++;
		}
	}
	if(options.webtransport && imquic_get_endpoint_wt_protocols(endpoint) != NULL) {
		IMQUIC_LOG(IMQUIC_LOG_INFO, "WebTransport Protocol(s):\n");
		int i = 0;
		const char **wt_protocols = imquic_get_endpoint_wt_protocols(endpoint);
		while(wt_protocols[i] != NULL) {
			IMQUIC_LOG(IMQUIC_LOG_INFO, "  -- %s\n", wt_protocols[i]);
			i++;
		}
	}
	imquic_set_new_roq_connection_cb(endpoint, imquic_demo_new_connection);
	imquic_set_rtp_incoming_cb(endpoint, imquic_demo_rtp_incoming);
	imquic_set_connection_failed_cb(endpoint, imquic_demo_connection_failed);
	imquic_set_roq_connection_gone_cb(endpoint, imquic_demo_connection_gone);
	connections = g_hash_table_new(NULL, NULL);
	imquic_start_endpoint(endpoint);

#ifdef HAVE_ROQ_CAPTURE
	if(options.capture) {
		while(!g_atomic_int_get(&stop))
			g_usleep(100000);
	} else
#endif
	{
		/* Wait for incoming RTP packets */
		socklen_t addrlen;
		struct sockaddr_storage remote;
		int resfd = 0, bytes = 0, num = 0, i = 0;
		struct pollfd fds[2];
		uint8_t buffer[1500];
		int64_t now = g_get_monotonic_time(), before = now;
		uint64_t flow_id = 0;
		/* Loop */
		while(!g_atomic_int_get(&stop)) {
			now = g_get_monotonic_time();
			if(options.timeout > 0 && (now - before >= options.timeout*G_USEC_PER_SEC)) {
				IMQUIC_LOG(IMQUIC_LOG_WARN, "%d seconds with no RTP traffic, shutting down...\n",
					options.timeout);
				break;
			}
			num = 0;
			if(audio_fd != -1) {
				fds[num].fd = audio_fd;
				fds[num].events = POLLIN;
				fds[num].revents = 0;
				num++;
			}
			if(video_fd != -1) {
				fds[num].fd = video_fd;
				fds[num].events = POLLIN;
				fds[num].revents = 0;
				num++;
			}
			if(num == 0)
				break;
			/* Wait for some data */
			resfd = poll(fds, num, 100);
			if(resfd < 0) {
				if(errno == EINTR) {
					IMQUIC_LOG(IMQUIC_LOG_HUGE, "Got an EINTR (%s), ignoring...\n", g_strerror(errno));
					continue;
				}
				IMQUIC_LOG(IMQUIC_LOG_ERR, "Error polling... %d (%s)\n", errno, g_strerror(errno));
				break;
			}
			for(i=0; i<num; i++) {
				if(fds[i].revents & (POLLERR | POLLHUP)) {
					/* Socket error? */
					IMQUIC_LOG(IMQUIC_LOG_ERR, "Error polling %s RTP socket: %s... %d (%s)\n",
						fds[i].fd == audio_fd ? "audio" : "video",
						fds[i].revents & POLLERR ? "POLLERR" : "POLLHUP", errno, g_strerror(errno));
					if(fds[i].fd == audio_fd) {
						close(audio_fd);
						audio_fd = -1;
					} else {
						close(video_fd);
						video_fd = -1;
					}
					continue;
				} else if(fds[i].revents & POLLIN) {
					/* Got an RTP packet */
					addrlen = sizeof(remote);
					bytes = recvfrom(fds[i].fd, buffer, 1500, 0, (struct sockaddr *)&remote, &addrlen);
					if(bytes < 0 || !imquic_roq_is_rtp(buffer, bytes)) {
						/* Failed to read or not an RTP packet? */
						continue;
					}
					before = g_get_monotonic_time();
					/* Pick the right flow ID */
					flow_id = (fds[i].fd == audio_fd ? options.audio_flow : options.video_flow);
					imquic_demo_send_rtp_to_connections(flow_id, buffer, bytes);
				}
			}
		}
	}

	/* We're done */
	imquic_shutdown_endpoint(endpoint);

done:
#ifdef HAVE_ROQ_CAPTURE
	if(abr_thread != NULL)
		g_thread_join(abr_thread);
	abr_thread = NULL;
	if(abr != NULL)
		moq_loc_abr_destroy(abr);
	abr = NULL;
	if(svc_abr != NULL)
		moq_loc_svc_abr_destroy(svc_abr);
	svc_abr = NULL;
	if(options.capture)
		roq_capture_destroy();
#endif
	if(audio_fd > -1)
		close(audio_fd);
	if(video_fd > -1)
		close(video_fd);
	imquic_deinit();
	if(connections != NULL)
		g_hash_table_unref(connections);
	if(ret == 1)
		demo_options_show_usage();
	demo_options_destroy();

	/* Done */
	IMQUIC_PRINT("Bye!\n");
	exit(ret);
}
