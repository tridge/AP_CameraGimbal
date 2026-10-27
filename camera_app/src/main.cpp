#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apcam/target.h"
#include "apcam/APC_Timezone.h"
#include "camera_app/APC_CameraApp.h"
#include "apcam/APC_Resource.h"
#if APCAM_HAVE_XFROBOT
#include "camera_app/xfrobot_server.h"
#endif
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI && !defined(CAMERA_APP_SITL)
#include "backends/z1mini/native.h"
#endif
#ifdef CAMERA_APP_ZT30_RAM
#include "backends/zt30/ram_app.h"
#endif
#include "camera_app/backend.h"
#include "camera_app/manual_control.h"
#include "camera_app/config.h"
#include "camera_app/video_metadata.h"
#ifdef CAMERA_APP_ZT30_DEV
#include "apcam/APC_ZT30_Config.h"
#endif
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "camera_app/media.h"
#include "camera_app/metadata.h"
#include "camera_app/mavlink_server.h"
#include "camera_app/network.h"

#include "camera_app/siyi.h"
#include "camera_app/siyi_server.h"
#include "camera_app/unigcs.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT APCAM_VENDOR_PORT
#define DEFAULT_READY_PATH "/run/camera-app.ready"
static volatile sig_atomic_t stop_requested;

static const char *environment_string(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static unsigned environment_port(const char *name, unsigned fallback)
{
    const char *text = getenv(name);
    char *end;
    unsigned long value;

    if (text == NULL || text[0] == '\0') return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' && value > 0U && value <= 65535U
               ? (unsigned)value
               : fallback;
}

static unsigned environment_optional_port(const char *name, unsigned fallback)
{
    const char *text = getenv(name);
    char *end;
    unsigned long value;

    if (text == NULL || text[0] == '\0') return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' && value <= 65535U
               ? (unsigned)value
               : fallback;
}

static int set_recording(void *opaque, bool active)
{
    return ca_media_set_recording((struct ca_media*)(opaque), active);
}

static bool get_recording(void *opaque)
{
    return ca_media_recording((const struct ca_media*)(opaque));
}

static int set_zoom(void *opaque, float zoom)
{
    return ca_media_set_zoom((struct ca_media*)(opaque), zoom);
}

static float get_zoom(void *opaque)
{
    return ca_media_zoom((const struct ca_media*)(opaque));
}

static int set_lens(void *opaque, bool wide)
{
    return ca_media_set_lens((struct ca_media*)(opaque), wide ? CA_MEDIA_LENS_WIDE
                                          : CA_MEDIA_LENS_ZOOM);
}

static bool lens_is_wide(void *opaque)
{
    return ca_media_lens((const struct ca_media*)(opaque)) == CA_MEDIA_LENS_WIDE;
}

static int set_thermal_main(void *opaque, bool thermal_main)
{
    return ca_media_set_thermal_main((struct ca_media*)(opaque), thermal_main);
}

static bool thermal_is_main(void *opaque)
{
    return ca_media_thermal_main((const struct ca_media*)(opaque));
}

static int autofocus(void *opaque, uint16_t x, uint16_t y)
{
    return ca_media_autofocus((struct ca_media*)(opaque), x, y);
}

static int manual_focus(void *opaque, int direction)
{
    return ca_media_manual_focus((struct ca_media*)(opaque), direction);
}

static bool get_thermal_range(void *opaque, struct ca_thermal_range *range)
{
    return ca_media_thermal_range((struct ca_media*)(opaque), range);
}

static int capture_photo(void *opaque)
{
    auto *media = static_cast<ca_media *>(opaque);
    return ca_media_capture_photo(media, ca_media_settings(media)->photo_scope);
}

static int get_thermal_gain(void *opaque, uint8_t *gain)
{
    return ca_media_get_thermal_gain((struct ca_media*)(opaque), gain);
}

static int set_thermal_gain(void *opaque, uint8_t gain)
{
    return ca_media_set_thermal_gain((struct ca_media*)(opaque), gain);
}

static int get_thermal_palette(void *opaque, uint8_t *palette)
{
    return ca_media_get_thermal_palette((struct ca_media*)(opaque), palette);
}

static int set_thermal_palette(void *opaque, uint8_t palette)
{
    return ca_media_set_thermal_palette((struct ca_media*)(opaque), palette);
}

static int set_inverted(void *opaque, bool inverted)
{
    return ca_media_set_inverted((struct ca_media*)(opaque), inverted);
}

static const char *ready_path(void)
{
    const char *path = getenv("CAMERA_APP_READY_PATH");
    return path != NULL ? path : DEFAULT_READY_PATH;
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
}

/* vendor media libraries keep replacing the SIGTERM disposition from their
 * own threads (SigmaStar MI sets it to SIG_IGN once the ISP is running) */
static void restore_signal_handlers(void)
{
    struct sigaction current;

    if (sigaction(SIGTERM, NULL, &current) == 0 &&
        current.sa_handler != handle_signal) {
        install_signal_handlers();
    }
}

#if APCAM_HAVE_SIYI
static void log_unknown_siyi(const struct ca_siyi_packet *packet)
{
    const size_t payload_limit = 32U;
    size_t shown = packet->payload_length < payload_limit
                           ? packet->payload_length
                           : payload_limit;

    char payload_hex[payload_limit * 2U + 4U];
    size_t used = 0;
    if (shown == 0U) {
        memcpy(payload_hex, "<empty>", 8U);
        used = 7U;
    } else {
        static const char digits[] = "0123456789abcdef";
        for (size_t i = 0; i < shown; i++) {
            payload_hex[used++] = digits[packet->payload[i] >> 4];
            payload_hex[used++] = digits[packet->payload[i] & 0x0fU];
        }
        if (shown < packet->payload_length) {
            memcpy(payload_hex + used, "...", 3U);
            used += 3U;
        }
    }
    payload_hex[used] = '\0';
    ca_log("unknown SIYI command opcode=0x%02x control=0x%02x sequence=%u "
           "payload_length=%u payload=%s",
           packet->opcode, packet->control, packet->sequence,
           packet->payload_length, payload_hex);
}

struct siyi_request_context { ca_backend *backend; ca_media *media; ca_mavlink_server *mavlink; };
static int handle_siyi_request(void *opaque, const uint8_t *raw, size_t length)
{
    auto &context = *static_cast<siyi_request_context *>(opaque);
    auto *backend = context.backend;
    struct ca_siyi_packet packet;
    size_t consumed = 0U;
    if (ca_siyi_parse_one(raw, length, &packet, &consumed) != 1 ||
        consumed != length) {
        errno = EPROTO;
        return -1;
    }
    if (!ca_siyi_opcode_known(packet.opcode)) log_unknown_siyi(&packet);
    const bool survey = ca_mavlink_server_survey_owns_gimbal(context.mavlink);
    if (ca_backend_manual_blocked(&survey, nullptr, packet.opcode,
                                  packet.payload, packet.payload_length)) return 0;
    if (packet.opcode == 0x07 || packet.opcode == 0x08 || packet.opcode == 0x0e ||
        packet.opcode == 0x40 ||
        (packet.opcode == 0x0c && packet.payload_length == 1 &&
         packet.payload[0] >= 3 && packet.payload[0] <= 5)) {
        ca_media_tracking_stop(context.media);
        ca_mavlink_server_suspend_gimbal(context.mavlink);
    }
    if (ca_backend_handle_siyi(backend, raw, length) < 0) {
        ca_log("opcode 0x%02x failed: %s", packet.opcode, strerror(errno));
        return -1;
    }
    return 0;
}

#endif

static int write_ready(const struct ca_backend *backend, unsigned port,
                       unsigned mavlink_tcp_port, unsigned mavlink_udp_port,
                       enum ca_uart_protocol uart_protocol, unsigned manual_port)
{
    APC_FileDescriptor fd(open(ready_path(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644));
    char text[256];
    int length;

    if (fd.get() < 0) return -1;
    length = snprintf(text, sizeof(text),
                      "backend=%s\npid=%ld\nudp_port=%u\ntcp_port=%u\n"
                      "mavlink_tcp_port=%u\nmavlink_udp_port=%u\n"
                      "uart_protocol=%s\nrecording=%u\nmanual_port=%u\n",
                      ca_backend_name(backend), (long)getpid(), port,
#if APCAM_HAVE_XFROBOT
                      port == APCAM_VENDOR_PORT ? APCAM_VENDOR_TCP_PORT : port,
#else
                      port,
#endif
                      mavlink_tcp_port, mavlink_udp_port,
                      ca_uart_protocol_name(uart_protocol),
                      ca_backend_recording(backend) ? 1U : 0U, manual_port);
    if (length < 0 || (size_t)length >= sizeof(text) ||
        write(fd.get(), text, (size_t)length) != length) {
        int saved_errno = errno;
        errno = saved_errno != 0 ? saved_errno : EIO;
        return -1;
    }
    return 0;
}

static void usage(const char *program)
{
#if APCAM_TARGET == APCAM_TARGET_RPI_LIBCAM_CADDX
    ca_log("Usage: %s [--backend " APCAM_NAME "] [--uart DEVICE|udp://IPv4:PORT|tcp://IPv4:PORT] [--port PORT] "
           "[--config PATH]", program);
#else
    ca_log("Usage: %s [--backend mt11|a8|zr10|zt30|z1mini] [--uart DEVICE|udp://IPv4:PORT] [--port PORT] "
           "[--config PATH]", program);
#endif
}

int APC_CameraApp::run(int argc, char **argv)
{
    #ifdef CAMERA_APP_ZT30_RAM
    if(!ca_zt30::ram_app_prepare(argc,&stop_requested,install_signal_handlers)) { ca_log("ZT30 RAM activation refused");return 2; }
    #endif
    const char *backend_name = environment_string("CAMERA_APP_BACKEND", APCAM_NAME);
    /* NULL lets each backend pick its own gimbal UART */
    const char *uart_device = environment_string("CAMERA_APP_UART", NULL);
    const char *external_uart_device =
        environment_string("CAMERA_APP_EXTERNAL_UART",
                           APCAM_HAVE_EXTERNAL_UART ? "/dev/ttyAMA4" : NULL);
    const char *config_path =
        environment_string("CAMERA_APP_CONFIG", CA_CONFIG_DEFAULT_PATH);
    unsigned port = environment_port("CAMERA_APP_PORT", DEFAULT_PORT);
    struct ca_backend_config config = {};
    struct ca_config app_config;
    char config_error[192] = "";
    int i;
    int result = 1;
    unsigned mavlink_tcp_port;
    unsigned mavlink_udp_port;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            backend_name = argv[++i];
        } else if (strcmp(argv[i], "--uart") == 0 && i + 1 < argc) {
            uart_device = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char *end;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || value == 0 || value > 65535) {
                usage(argv[0]);
                return 2;
            }
            port = (unsigned)value;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    ca_config_defaults(&app_config);
    if (ca_config_load(&app_config, config_path, config_error,
                       sizeof(config_error)) < 0 && errno != ENOENT) {
        ca_log("cannot load config %s: %s", config_path,
               config_error[0] != '\0' ? config_error : strerror(errno));
        return 2;
    }
#if APCAM_HAVE_UART_DEVICE_OVERRIDE
    /* A configured override replaces --uart and CAMERA_APP_UART. */
    if (app_config.uart_device_override[0] != '\0')
        uart_device = app_config.uart_device_override;
#endif
    #ifdef CAMERA_APP_ZT30_RAM
    ca_zt30::ram_app_configure(app_config);
    #endif
    #ifdef CAMERA_APP_ZT30_DEV
    if (APC_ZT30_Config::effective(app_config)) {
        ca_log("native ZT30: unsupported saved settings clamped in memory; "
               "visible video is 1280x720 H264, recording/OSD/targeting disabled; INI preserved");
    }
    #endif
    ca_video_metadata_set_format(app_config.video_telemetry_format);
    if (APC_Timezone::apply(app_config.timezone) < 0) {
        ca_log("cannot apply timezone %s: %s", app_config.timezone,
               strerror(errno));
        return 2;
    }
    mavlink_tcp_port = environment_optional_port(
        "CAMERA_APP_MAVLINK_TCP_PORT", app_config.mavlink_tcp_port);
    mavlink_udp_port = environment_optional_port(
        "CAMERA_APP_MAVLINK_UDP_PORT", app_config.mavlink_udp_port);
    ca_log("configuration loaded path=%s timezone=%s photo_scope=%s "
           "orientation=%s uart=%s palette=%s autorecord=%u main=%s/%s sub=%s/%s "
           "recording=%s position_targeting=%u mavlink_tcp=%u mavlink_udp=%u",
           config_path, app_config.timezone,
           ca_photo_scope_name(app_config.photo_scope),
           ca_mount_orientation_name(app_config.orientation),
           ca_uart_protocol_name(app_config.uart_protocol),
           ca_thermal_palette_name(app_config.thermal_palette),
           (unsigned)app_config.autorecord,
           ca_video_resolution_name(app_config.main_resolution),
           ca_video_codec_name(app_config.main_codec),
           ca_video_resolution_name(app_config.sub_resolution),
           ca_video_codec_name(app_config.sub_codec),
           ca_video_resolution_name(app_config.recording_resolution),
           app_config.position_targeting ? 1U : 0U,
           mavlink_tcp_port, mavlink_udp_port);

    _started = true;
    install_signal_handlers();
    (void)unlink(ready_path());


#if APCAM_HAVE_SIYI
    const char *siyi_uart = app_config.uart_protocol == CA_UART_SIYI
                                ? external_uart_device
                                : NULL;
    if (ca_siyi_server_open(&_server, port, siyi_uart) < 0) {
        ca_log("cannot bind SIYI UDP/TCP %u: %s", port, strerror(errno));
        return result;
    }
#elif APCAM_HAVE_XFROBOT
    if (ca_xfrobot_server_open(&_xfrobot, port, app_config.orientation == CA_MOUNT_INVERTED) < 0) return result;
#endif
    const char *record_root = environment_string("CAMERA_APP_RECORD_ROOT",
                                                 "/mnt/DCIM/record");
    const char *capture_root = environment_string("CAMERA_APP_CAPTURE_ROOT",
                                                  "/mnt/DCIM/capture");
    const char *log_root = environment_string("CAMERA_APP_LOG_ROOT", APCAM_LOG_ROOT);
#ifdef CAMERA_APP_SITL
    /* Keep isolated tests/portable installs off the host /mnt filesystem. */
    char sitl_log_root[4096];
    if (!getenv("CAMERA_APP_LOG_ROOT")) {
        snprintf(sitl_log_root, sizeof(sitl_log_root), "%s/../logs", record_root);
        log_root = sitl_log_root;
    }
#endif
    if (ca_binlog_init(log_root) < 0) ca_log("cannot start BIN log writer: %s", strerror(errno));
    else if (app_config.log_disarmed) (void)ca_binlog_start(&app_config);
    unsigned rtsp_port = environment_port("CAMERA_APP_RTSP_PORT", 8554U);
    struct ca_media_config media_config = {
        .backend = backend_name,
        .record_root = record_root,
        .capture_root = capture_root,
        .rtsp_port = rtsp_port,
        .width = 1920,
        .height = 1080,
#if APCAM_HAVE_VIDEO_FRAME_RATE
        .frame_rate = app_config.main_frame_rate,
#else
        .frame_rate = APCAM_FRAME_RATE,
#endif
#if APCAM_HAVE_VIDEO_BITRATE
        .bit_rate_kbps = app_config.main_bitrate_kbps,
#else
        .bit_rate_kbps = APCAM_DEFAULT_MAIN_BITRATE_KBPS,
#endif
        .settings = app_config,
    };
    char network_error[256] = "";
#if defined(CAMERA_APP_SITL) || defined(CAMERA_APP_ZT30_RAM)
    if (app_config.network.primary_address[0] || app_config.network.secondary_address[0] || app_config.network.gateway[0])
        ca_log("network configuration skipped in SITL (uses host networking)");
#elif !APCAM_HAVE_NETWORK_CONFIG
    if (app_config.network.primary_address[0] || app_config.network.secondary_address[0] || app_config.network.gateway[0])
        ca_log("network configuration skipped (host OS owns networking)");
#else
    char network_state[PATH_MAX];
    if (snprintf(network_state, sizeof(network_state), "%s.network", ready_path()) >= (int)sizeof(network_state)) {
        snprintf(network_error, sizeof(network_error), "Network configuration failed: ready-file path is too long. Restart to retry.");
    } else if (ca_network_configure(&app_config.network, network_state) < 0) {
        snprintf(network_error, sizeof(network_error), "Network configuration failed on %s: %s. Restart to retry.", app_config.network.interface, strerror(errno));
    }
#endif
    if (network_error[0]) ca_log("%s", network_error);
    if (_network_capture.init(record_root, ready_path()) < 0)
        ca_log("cannot initialize network capture: %s", strerror(errno));
    _network_capture.configure(app_config.network_capture);
    if (ca_media_open(&_media, &media_config) < 0) {
        ca_log("cannot open media backend %s: %s", backend_name, strerror(errno));
        return result;
    }
    config.manual_control=&_manual.active;
#if APCAM_HAVE_SIYI
    // UniGCS assumes fixed ports and cannot distinguish several cameras at
    // one host IP. Secondary SITL instances retain their public SDK service;
    // isolated tests may explicitly allocate the two private service ports.
    if (port == 37260U || getenv("CAMERA_APP_UNIGCS_PORT")) {
        if (ca_unigcs_open(&_unigcs, _media,
                          environment_port("CAMERA_APP_UNIGCS_PORT", 37256U),
                          environment_port("CAMERA_APP_DISCOVERY_PORT", 37258U)) < 0) {
            ca_log("cannot open UniGCS service: %s", strerror(errno));
            return result;
        }
    }
    config.uart_emit = [](void *opaque, const uint8_t *data, size_t length) {
        ca_mavlink_server_feed_mcu(*static_cast<ca_mavlink_server **>(opaque), data, length);
    };
    config.uart_opaque = &_mavlink_server;
    config.private_emit = ca_unigcs_emit;
    config.private_opaque = _unigcs;
#endif
    config.manual_command=&_manual.executing;
    config.name = backend_name;
    config.uart_device = uart_device;
#if APCAM_HAVE_SIYI
    config.emit = ca_siyi_server_emit;
#else
    config.emit = NULL;
#endif
    config.emit_opaque = _server;
    config.recording_set = set_recording;
    config.recording_get = get_recording;
    config.recording_opaque = _media;
    config.zoom_set = set_zoom;
    config.zoom_get = get_zoom;
    config.zoom_opaque = _media;
    config.lens_set = set_lens;
    config.lens_wide = lens_is_wide;
    config.lens_opaque = _media;
    config.thermal_main_set = set_thermal_main;
    config.thermal_main_get = thermal_is_main;
    config.thermal_main_opaque = _media;
    config.autofocus = autofocus;
    config.autofocus_opaque = _media;
    config.manual_focus = manual_focus;
    config.manual_focus_opaque = _media;
    config.thermal_range = get_thermal_range;
    config.thermal_opaque = _media;
    config.photo_capture = capture_photo;
    config.photo_capture_opaque = _media;
    config.thermal_gain_get = get_thermal_gain;
    config.thermal_gain_set = set_thermal_gain;
    config.thermal_gain_opaque = _media;
    config.thermal_palette_get = get_thermal_palette;
    config.thermal_palette_set = set_thermal_palette;
    config.thermal_palette_opaque = _media;
    config.orientation = app_config.orientation;
    config.inverted_set = set_inverted;
    config.inverted_opaque = _media;
    if (ca_backend_open(&_backend, &config) < 0) {
        ca_log("cannot open backend %s: %s", backend_name, strerror(errno));
        return result;
    }
#ifdef CAMERA_APP_ZT30_DEV
    // One-shot photos can wait for hardware/storage. Stop the sole UART
    // owner first, so no nonzero rate depends on a blocked main-loop timer.
    ca_media_set_photo_prepare(_media,[](void *opaque) {
        return ca_backend_set_gimbal_rates(static_cast<ca_backend*>(opaque),0,0);
    },_backend);
#endif
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI
    /* Do not report ready before gimbal feedback is available. */
    bool gimbal_ready = false;
    for (unsigned attempt = 0; attempt < 150 && !stop_requested; attempt++) {
        struct pollfd item = {.fd = ca_backend_fd(_backend), .events = POLLIN};
        if (poll(&item, 1, 20) > 0 && ca_backend_handle_fd(_backend) < 0) break;
        ca_backend_periodic(_backend);
        struct ca_gimbal_attitude attitude;
        if (ca_backend_gimbal_attitude(_backend, &attitude)) { gimbal_ready = true; break; }
    }
    if (!gimbal_ready) { ca_log("Z1 gimbal feedback unavailable"); return result; }
#endif
    ca_metadata_set_model(APCAM_MODEL_NAME);
    struct ca_mavlink_server_config mavlink_config = {
        .manual_control = &_manual.active,
        .tcp_port = mavlink_tcp_port,
        .udp_port = mavlink_udp_port,
        .uart_device = app_config.uart_protocol == CA_UART_MAVLINK
                           ? external_uart_device
                           : NULL,
        .rtsp_port = rtsp_port,
        .record_root = record_root,
        .capture_root = capture_root,
        .log_root = log_root,
        .config_path = config_path,
        .network_error = network_error,
        .photo_scope = app_config.photo_scope,
        .settings = app_config,
        .backend = _backend,
        .media = _media,
    };
    if (ca_mavlink_server_open(&_mavlink_server, &mavlink_config) < 0) {
        ca_log("cannot initialize MAVLink: %s", strerror(errno));
        return result;
    }
    if (ca_manual_control_open(&_manual,_backend,_mavlink_server)<0)
        ca_log("web manual control unavailable: %s",strerror(errno));
#ifdef CAMERA_APP_ZT30_DEV
    if (app_config.autorecord == CA_AUTORECORD_ENABLED) ca_media_defer_autorecord(_media);
#else
    if (app_config.autorecord == CA_AUTORECORD_ENABLED &&
        ca_media_set_recording(_media, true) < 0) {
        ca_log("automatic recording could not start: %s", strerror(errno));
    }
#endif
    /* The vendor media libraries install signal handlers while starting ISP.
     * Restore application-owned shutdown handling once all of them are loaded. */
    install_signal_handlers();
    if (write_ready(_backend, port, mavlink_tcp_port, mavlink_udp_port,
                    app_config.uart_protocol, _manual.port) < 0) {
        ca_log("cannot publish readiness: %s", strerror(errno));
        return result;
    }
    ca_log("ready backend=%s vendor_port=%u configured MAVLink/TCP=%u MAVLink/UDP=%u "
           "UART4=%s/230400 PID=%ld",
           backend_name, port, mavlink_tcp_port, mavlink_udp_port,
           ca_uart_protocol_name(app_config.uart_protocol),
           (long)getpid());

    unsigned loops = 0;
    while (!stop_requested) {
#ifdef CAMERA_APP_ZT30_RAM
        if(ca_zt30::ram_media_finished())return ca_zt30::ram_media_result();
#endif
#ifdef CAMERA_APP_ZT30_DEV
        if (!ca_media_ready(_media)) {
            ca_log("ZT30 media stopped; exiting for AP supervisor restart");
            return result;
        }
#endif
#if APCAM_TARGET == APCAM_TARGET_Z1_MINI && !defined(CAMERA_APP_SITL)
        const char *native_helper = getenv("CAMERA_APP_Z1_NATIVE_HELPER");
        if (native_helper && *native_helper && !ca_media_ready(_media)) {
            ca_log("Z1 native capture stopped; exiting for supervisor recovery");
            return result;
        }
#endif
#if APCAM_HAVE_XFROBOT
        ca_xfrobot_server_update(_xfrobot, _backend, _media, _manual.active);
#endif
        if (++loops % 50U == 0U) restore_signal_handlers();
        struct pollfd items[4] = {
            {.fd = ca_siyi_server_fd(_server), .events = POLLIN},
            {.fd = ca_backend_fd(_backend), .events = POLLIN},
            {.fd = ca_mavlink_server_fd(_mavlink_server), .events = POLLIN},
            {.fd = _manual.fd, .events = POLLIN},
        };
        int ready = poll(items, 4, 20);
        if (ready < 0) {
            if (errno == EINTR) continue;
            ca_log("poll failed: %s", strerror(errno));
            return result;
        }
        ca_manual_control_update(&_manual);
#ifdef CAMERA_APP_SITL
        // A restarted UDP MCU simulator briefly has no listening socket.
        // Consume its asynchronous ICMP error without terminating video and
        // the camera services; periodic MCU requests reconnect naturally.
        if (items[1].revents & POLLERR) {
            int type = 0, error = 0;
            socklen_t length = sizeof(type);
            if (getsockopt(items[1].fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
                type == SOCK_DGRAM) {
                length = sizeof(error);
                if (getsockopt(items[1].fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                    (error == 0 || error == ECONNREFUSED)) {
                    items[1].revents &= ~POLLERR;
                }
            }
        }
#endif
        if ((items[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ca_log("backend descriptor failed");
            return result;
        }
        if ((items[1].revents & POLLIN) != 0 && ca_backend_handle_fd(_backend) < 0) {
            ca_log("backend read failed: %s", strerror(errno));
            return result;
        }
#if APCAM_HAVE_SIYI
        if ((items[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ca_log("SIYI server descriptor failed");
            return result;
        }
        siyi_request_context siyi_context {_backend, _media, _mavlink_server};
        if (((items[0].revents & POLLIN) != 0 || items[0].fd < 0) &&
            ca_siyi_server_handle(_server, handle_siyi_request, &siyi_context) < 0) {
            ca_log("SIYI server failed: %s", strerror(errno));
            return result;
        }
#endif
        if ((items[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            ca_log("MAVLink server descriptor failed");
            return result;
        }
        if (((items[2].revents & POLLIN) != 0 || items[2].fd < 0) &&
            ca_mavlink_server_handle(_mavlink_server) < 0) {
            ca_log("MAVLink server failed: %s", strerror(errno));
            return result;
        }
        ca_backend_periodic(_backend);
        ca_media_periodic(_media);
        ca_mavlink_server_periodic(_mavlink_server);
#if APCAM_HAVE_SIYI
        ca_unigcs_update(_unigcs, _backend, _manual.active, _mavlink_server);
#endif
        ca_media_tracking_update(_media, _backend, _manual.active);
        _network_capture.configure(ca_media_settings(_media)->network_capture);
    }
#ifdef CAMERA_APP_ZT30_RAM
    result = 24; // interruption is not a completed capture
#else
    result = 0;
#endif

    return result;
}

APC_CameraApp::~APC_CameraApp()
{
    if (!_started) { return; }
    (void)unlink(ready_path());
    _network_capture.close();
    ca_manual_control_close(&_manual);
    ca_mavlink_server_close(_mavlink_server);
    ca_media_tracking_stop(_media);
#if APCAM_HAVE_SIYI
    ca_unigcs_close(_unigcs);
#endif
    ca_backend_close(_backend);
    ca_media_close(_media);
    ca_binlog_close();
#if APCAM_HAVE_XFROBOT
    ca_xfrobot_server_close(_xfrobot);
#endif
    ca_siyi_server_close(_server);
    ca_log("stopped");
}

int main(int argc, char **argv)
{
#ifdef CAMERA_APP_ZT30_RAM
    setvbuf(stdout,nullptr,_IONBF,0);
    int result;
    { APC_CameraApp app;result=app.run(argc,argv); }
    if(!result && ca_zt30::ram_media_closed() && !ca_zt30::ram_media_result()) {
        puts("ENCODER PASS copied H264 payloads; independent host decode required");
        puts("FULL_APP PASS factory; frontend; joined teardown; RAM only; no UART/USB");
        printf("RESULT PASS owned %s RGB IQ H264 RTSP web capture and unwind; no UART/USB\n",ca_zt30::ram_media_wide()?"wide":"zoom");
        return 0;
    }
    return result?result:30;
#else
    APC_CameraApp app;
    return app.run(argc, argv);
#endif
}
