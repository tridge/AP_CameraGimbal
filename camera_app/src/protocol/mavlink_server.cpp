#include "camera_app/terrain.h"
#include <string>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/mavlink_server.h"
#include "apcam/lens.h"
#include "apcam/APC_Timezone.h"
#include "apcam/config_status.h"
#ifdef CAMERA_APP_ZT30_DEV
#include "apcam/APC_ZT30_Config.h"
#endif

#include "camera_app/backend.h"
#include "camera_app/camera_definition.h"
#include "camera_app/camera_ftp.h"
#include "camera_app/siyi.h"
#include "camera_app/external_uart.h"
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "camera_app/mavlink.h"
#include "camera_app/support_proxy.h"
#include "camera_app/media.h"
#include "camera_app/metadata.h"
#include "camera_app/targeting.h"
#include "camera_app/survey.h"
#include "camera_app/telemetry_time.h"
#include "camera_app/thermal_stream.h"
#include "camera_app/video_fov.h"

#include <arpa/inet.h>
#ifdef CAMERA_APP_ZT30_RAM
#include "camera_app/zt30_ram_peer.h"
#include "../backends/zt30/ram_protocol.h"
#endif
#include <algorithm>
#include <errno.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_app/event_poll.h"
#ifdef __linux__
#include <linux/sockios.h>
#endif
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CA_MAVLINK_CLIENTS 4U
#define CA_EVENT_UDP UINT64_C(1)
#define CA_EVENT_LISTENER UINT64_C(2)
#define CA_EVENT_UART UINT64_C(3)
#define CA_EVENT_CLIENT_BASE UINT64_C(0x100)
#define CA_MAVLINK_UART_OUTPUT (MAVLINK_MAX_PACKET_LEN * 16U)
#define VEHICLE_ATTITUDE_TIMEOUT_MS ca_yaw_history::hold_ms
#define VEHICLE_POSITION_TIMEOUT_MS 1500U
#define VEHICLE_PREDICTION_MS ca_yaw_history::prediction_ms
#define TARGET_LOCATION_INTERVAL_MS 100U
#define TELEMETRY_INTERVAL_REQUEST_MS 5000U
#define THERMAL_DEFAULT_INTERVAL_MS 200U
#define PI_F 3.14159265358979323846f

struct mavlink_client {
    int fd;
    struct ca_mavlink_parser parser;
};

enum route_kind { ROUTE_TCP, ROUTE_UDP, ROUTE_UART, ROUTE_MCU, ROUTE_PROXY };

struct route {
    enum route_kind kind;
    unsigned client;
    struct sockaddr_in address;
    socklen_t address_length;
};

struct ext_parameter_list {
    bool active;
    uint8_t system, component;
    struct route route;
    size_t next;
};

struct ca_mavlink_server {
    const bool *manual_control;
    struct ca_support_mavlink *support;
    struct route flight_controller_route;
    bool have_flight_controller_route;
    struct ca_pollset *pollset;
    int udp_fd;
    int listener_fd;
    int uart_fd;
    struct ca_mavlink_parser uart_parser;
    struct ca_mavlink_parser mcu_parser;
    bool have_mcu_peer;
    uint8_t mcu_output[CA_MAVLINK_UART_OUTPUT];
    size_t mcu_output_length;
    uint64_t mcu_last_write_ms;
    uint8_t uart_output[CA_MAVLINK_UART_OUTPUT];
    size_t uart_output_offset;
    size_t uart_output_length;
    struct mavlink_client clients[CA_MAVLINK_CLIENTS];
    struct ca_mavlink_parser udp_parser;
    struct route last_udp;
    bool have_udp_peer;
    struct ca_backend *backend;
    struct ca_media *media;
    struct ca_config settings;
    struct ca_config parameters;
    char *definition_xml;
    size_t definition_length;
    uint16_t definition_version;
    struct ca_camera_ftp ftp;
    struct ext_parameter_list ext_lists[CA_MAVLINK_CLIENTS];
    char *config_path;
    uint8_t system_id;
    uint8_t camera_component_id;
    uint8_t gimbal_component_id;
    size_t next_parameter;
    bool parameter_list_active;
    enum ca_photo_scope photo_scope;
    char capture_root[512];
    unsigned rtsp_port;
    mavlink_status_t encode_status;
    mavlink_status_t camera_tx;
    mavlink_status_t gimbal_tx;
    uint8_t autopilot_system_id;
    uint8_t autopilot_component_id;
    uint8_t camera_mode;
    struct survey_runtime *survey;
    bool stream_enabled[APCAM_NUM_STREAMS];
    bool misb_stream_enabled[APCAM_NUM_STREAMS];
#if APCAM_HAVE_THERMAL
    uint32_t thermal_interval_ms[APCAM_NUM_STREAMS];
    uint64_t last_thermal_ms[APCAM_NUM_STREAMS];
#endif
    float zoom_rate;
    float zoom_target; /* Integrate requests without discarding sub-step motion. */
    float zoom_observed;
    enum ca_media_lens zoom_lens;
    uint64_t zoom_updated_ms;
    float focus_percent;
    uint64_t started_ms;
    uint64_t last_heartbeat_ms;
    uint64_t last_attitude_request_ms;
    uint64_t last_attitude_status_ms;
    uint64_t last_image_tracking_ms;
    uint32_t image_tracking_interval_ms;
    uint64_t last_telemetry_request_ms;
    uint64_t last_target_location_ms;
    uint64_t vehicle_attitude_updated_ms;
    uint64_t vehicle_position_updated_ms;
    uint64_t primary_attitude_ms;
    bool vehicle_attitude_primary;
    ca_telemetry_clock attitude_clock, fallback_clock, position_clock;
    ca_yaw_history vehicle_yaw_history;
    uint64_t recording_started_ms;
    uint64_t next_capture_ms;
    uint32_t image_count;
    int32_t next_image_index;
    int32_t captures_remaining;
    float capture_interval_s;
    int32_t vehicle_lat_e7;
    int32_t vehicle_lon_e7;
    float vehicle_alt_amsl_m;
    float vehicle_vn_m_s, vehicle_ve_m_s, vehicle_vd_m_s;
    int32_t target_lat_e7;
    int32_t target_lon_e7;
    float target_alt_amsl_m;
    bool have_vehicle_armed;
    bool vehicle_armed;
    bool have_vehicle_attitude;
    bool have_vehicle_position;
    bool gimbal_information_announced;
    bool target_location_active;
    bool tracking_rate_active;
    float tracking_error[2];
    float tracking_integral[2];
    float tracking_rate[2];
    uint64_t config_check_ms;
    struct stat config_stat;
    bool config_seen, config_pending;
    char config_status_path[4096];
    char network_error[256];
    char config_last_status[512];
    bool yaw_lock;
    bool reported_recording;
    uint32_t flight_mode;
    uint64_t log_retry_ms, log_snapshot_ms, log_status_ms;
    bool log_snapshot_valid;
    int logged_saved[128], logged_applied[128];
    float logged_camera[128];
    struct ca_log_mode logged_mode;
};

static bool survey_running(const ca_mavlink_server *);
static bool survey_acquiring(ca_mavlink_server *,float,float,const ca_gimbal_attitude &);
static uint8_t set_camera_mode(ca_mavlink_server *, unsigned);
static void survey_pause(ca_mavlink_server *, const char *);
static void survey_message(ca_mavlink_server *,const mavlink_message_t *);
static uint8_t survey_capture_start(ca_mavlink_server *, int32_t, float);

static void update_binlog(struct ca_mavlink_server *server, bool allow_stop);


static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0U;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static uint64_t realtime_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) return 0U;
    return (uint64_t)now.tv_sec * UINT64_C(1000000) +
           (uint64_t)now.tv_nsec / 1000U;
}

static uint32_t boot_ms(const struct ca_mavlink_server *server)
{
    return (uint32_t)(monotonic_ms() - server->started_ms);
}

/* Messages are encoded with the scratch encode_status; each transmission
 * then takes the next sequence number of its component in send_message(). */
static mavlink_status_t *tx_status(struct ca_mavlink_server *server,
                                   uint8_t component_id)
{
    return component_id == server->gimbal_component_id
               ? &server->gimbal_tx : &server->camera_tx;
}

/* FTP bursts queue many packets at once; small default buffers would cut
 * them short on the camera's kernel */
static void set_send_buffer(int fd)
{
    int size = 256 * 1024;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

/* Bytes a route can still queue without blocking or dropping. */
static size_t route_send_room(struct ca_mavlink_server *server, const struct route *route)
{
    if (route->kind == ROUTE_PROXY) return SIZE_MAX;
    if (route->kind == ROUTE_UART) return sizeof(server->uart_output) - server->uart_output_length;
    if (route->kind == ROUTE_MCU) return sizeof(server->mcu_output) - server->mcu_output_length;
#ifdef SIOCOUTQ
    int fd = route->kind == ROUTE_UDP ? server->udp_fd : server->clients[route->client].fd;
    int buffer = 0, queued = 0;
    socklen_t length = sizeof(buffer);
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, &length) < 0 ||
        ioctl(fd, SIOCOUTQ, &queued) < 0 || queued < 0) return SIZE_MAX;
    return queued >= buffer ? 0 : (size_t)(buffer - queued);
#else
    /* Cygwin has no socket send-queue query. Retain bounded bursts and the
     * nonblocking send path, as when the ioctl is unsupported at runtime. */
    return SIZE_MAX;
#endif
}

static int bind_socket(int type, unsigned port)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port), .sin_addr = { .s_addr = htonl(INADDR_ANY) },
    };
    int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (type == SOCK_DGRAM) set_send_buffer(fd);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int open_listener(struct ca_mavlink_server *server, int type,
                         unsigned port, uint64_t tag)
{
    if (port == 0U) return -1;
    const char *transport = type == SOCK_DGRAM ? "UDP" : "TCP";
    int fd = bind_socket(type, port);
    ca_poll_event event = {.events = CA_POLL_IN, .data = { .u64 = tag }};
    if (fd < 0 || (type == SOCK_STREAM && listen(fd, 4) < 0) ||
        ca_poll_change(server->pollset, CA_POLL_ADD, fd, &event) < 0) {
        int saved_errno = errno;
        if (fd >= 0) close(fd);
        ca_log("MAVLink %s port %u unavailable: %s; continuing without %s",
               transport, port, strerror(saved_errno), transport);
        return -1;
    }
    ca_log("MAVLink 2 listening %s=%u", transport, port);
    return fd;
}

static bool route_valid(const struct ca_mavlink_server *server,
                        const struct route *route)
{
    if (route->kind == ROUTE_UDP) return server->udp_fd >= 0;
    if (route->kind == ROUTE_UART) return server->uart_fd >= 0;
    if (route->kind == ROUTE_MCU) return server->have_mcu_peer;
    if (route->kind == ROUTE_PROXY) return false;
    return route->client < CA_MAVLINK_CLIENTS &&
           server->clients[route->client].fd >= 0;
}

static bool have_peer(const struct ca_mavlink_server *server)
{
    if (server->support || server->have_udp_peer || server->uart_fd >= 0 || server->have_mcu_peer) return true;
    for (unsigned i = 0; i < CA_MAVLINK_CLIENTS; i++) {
        if (server->clients[i].fd >= 0) return true;
    }
    return false;
}

static void disable_uart(struct ca_mavlink_server *server, int error)
{
    if (server->uart_fd < 0) return;
    if (server->have_flight_controller_route && server->flight_controller_route.kind == ROUTE_UART)
        server->have_flight_controller_route = false;
    (void)ca_poll_change(server->pollset, CA_POLL_DEL, server->uart_fd, NULL);
    close(server->uart_fd);
    server->uart_fd = -1;
    server->uart_output_offset = 0U;
    server->uart_output_length = 0U;
    ca_log("MAVLink UART disabled after I/O failure: %s", strerror(error));
}

static int update_uart_events(struct ca_mavlink_server *server)
{
    ca_poll_event event = {
        .events = CA_POLL_IN |
                  (server->uart_output_length != 0U ? CA_POLL_OUT : 0U),
        .data = { .u64 = CA_EVENT_UART },
    };
    return ca_poll_change(server->pollset, CA_POLL_MOD, server->uart_fd, &event);
}

static int flush_uart(struct ca_mavlink_server *server)
{
    while (server->uart_output_length != 0U) {
        ssize_t sent = write(server->uart_fd,
                             server->uart_output + server->uart_output_offset,
                             server->uart_output_length);
        if (sent > 0) {
            server->uart_output_offset += (size_t)sent;
            server->uart_output_length -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1;
    }
    if (server->uart_output_length == 0U) server->uart_output_offset = 0U;
    return update_uart_events(server);
}

static int queue_uart(struct ca_mavlink_server *server, const uint8_t *data,
                      size_t length)
{
    if (length > sizeof(server->uart_output) - server->uart_output_length) {
        errno = ENOBUFS;
        return -1;
    }
    if (server->uart_output_offset != 0U &&
        server->uart_output_offset + server->uart_output_length + length >
            sizeof(server->uart_output)) {
        memmove(server->uart_output,
                server->uart_output + server->uart_output_offset,
                server->uart_output_length);
        server->uart_output_offset = 0U;
    }
    memcpy(server->uart_output + server->uart_output_offset +
               server->uart_output_length,
           data, length);
    server->uart_output_length += length;
    int result = flush_uart(server);
    if (result < 0) {
        int saved_errno = errno;
        disable_uart(server, saved_errno);
        errno = saved_errno;
    }
    return result;
}

/* Keep complete MAVLink packets queued separately. The MCU also transmits
 * its own MAVLink messages; pausing halfway through ours lets those messages
 * interleave on the external UART. */
static int queue_mcu(struct ca_mavlink_server *server, const uint8_t *data, size_t length)
{
    if (length + 2U > sizeof(server->mcu_output) - server->mcu_output_length) {
        errno = ENOBUFS;
        return -1;
    }
    server->mcu_output[server->mcu_output_length++] = uint8_t(length);
    server->mcu_output[server->mcu_output_length++] = uint8_t(length >> 8U);
    memcpy(server->mcu_output + server->mcu_output_length, data, length);
    server->mcu_output_length += length;
    return 0;
}

static void flush_mcu(struct ca_mavlink_server *server, uint64_t now)
{
    // 255 bytes take just over 22 ms at 115200 baud, 8N1.
    if (!server->mcu_output_length || now - server->mcu_last_write_ms < 25U) return;
    size_t length = server->mcu_output[0] | (size_t(server->mcu_output[1]) << 8U);
    server->mcu_last_write_ms = now;
    if (ca_backend_write_external_uart(server->backend, server->mcu_output + 2U, length) < 0) {
        // Do not continue a potentially partially written MAVLink stream.
        server->mcu_output_length = 0;
        return;
    }
    server->mcu_output_length -= length + 2U;
    memmove(server->mcu_output, server->mcu_output + length + 2U, server->mcu_output_length);
}

static void close_client(struct ca_mavlink_server *server, unsigned slot);

static int send_route(struct ca_mavlink_server *server,
                      const struct route *route, const uint8_t *data,
                      size_t length)
{
    ssize_t sent;
    if (!route_valid(server, route)) {
        errno = ENOTCONN;
        return -1;
    }
    if (route->kind == ROUTE_MCU)
        return queue_mcu(server, data, length);
    if (route->kind == ROUTE_UDP) {
        do {
            sent = sendto(server->udp_fd, data, length, 0,
                          (const struct sockaddr *)&route->address,
                          route->address_length);
        } while (sent < 0 && errno == EINTR);
    } else if (route->kind == ROUTE_TCP) {
        /* A frame must not be cut short: a partial write would corrupt the
         * stream for this client, so finish it with a bounded wait and drop
         * the client if the socket stays full. */
        int fd = server->clients[route->client].fd;
        size_t done = 0;
        for (;;) {
            sent = send(fd, data + done, length - done, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent < 0 && errno == EINTR) continue;
            if (sent > 0) done += (size_t)sent;
            if (done == length) break;
            if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
            if (done == 0) { errno = EAGAIN; return -1; }
            struct pollfd wait = {.fd = fd, .events = POLLOUT};
            if (poll(&wait, 1, 200) <= 0) {
                ca_log("MAVLink TCP client %u stalled mid-frame; disconnecting", route->client);
                close_client(server, route->client);
                errno = EIO;
                return -1;
            }
        }
        sent = (ssize_t)length;
    } else {
        return queue_uart(server, data, length);
    }
    if (sent == (ssize_t)length) return 0;
    if (sent >= 0) errno = EIO;
    return -1;
}

static int send_message(struct ca_mavlink_server *server,
                        const struct route *route, mavlink_message_t *message)
{
    uint8_t packet[MAVLINK_MAX_PACKET_LEN];
    size_t length;
    if (server->system_id == 0U) return 0;
    /* The MCU already provides the UART gimbal component. Only publish our
     * camera service here, avoiding competing identities and duplicate ACKs. */
    if (route->kind == ROUTE_MCU && message->compid == server->gimbal_component_id) return 0;
    if (route->kind == ROUTE_PROXY) {
        ca_mavlink_restamp(message, tx_status(server, message->compid));
        ca_support_mavlink_send(server->support, message);
        return 0;
    }
    ca_mavlink_restamp(message, tx_status(server, message->compid));
    length = ca_mavlink_to_wire(packet, sizeof(packet), message);
    if (length == 0U) {
        errno = EINVAL;
        return -1;
    }
    return send_route(server, route, packet, length);
}

static void broadcast_message(struct ca_mavlink_server *server,
                              mavlink_message_t *message)
{
    if (server->support) {
        const struct route proxy_route = {.kind = ROUTE_PROXY};
        (void)send_message(server, &proxy_route, message);
    }
    for (unsigned i = 0; i < CA_MAVLINK_CLIENTS; i++) {
        if (server->clients[i].fd < 0) continue;
        struct route route = {.kind = ROUTE_TCP, .client = i};
        (void)send_message(server, &route, message);
    }
    if (server->have_udp_peer) {
        (void)send_message(server, &server->last_udp, message);
    }
    if (server->have_mcu_peer) {
        const struct route route = {.kind = ROUTE_MCU};
        (void)send_message(server, &route, message);
    }
    if (server->uart_fd >= 0) {
        const struct route route = {.kind = ROUTE_UART};
        (void)send_message(server, &route, message);
    }
}

static void pack_heartbeat(struct ca_mavlink_server *server,
                           uint8_t component_id, uint8_t type,
                           mavlink_message_t *message)
{
    mavlink_heartbeat_t heartbeat = {};
    heartbeat.autopilot = MAV_AUTOPILOT_INVALID;
    heartbeat.system_status = MAV_STATE_ACTIVE;
    heartbeat.type = type;
    (void)mavlink_msg_heartbeat_encode_status(server->system_id, component_id,
                                              &server->encode_status,
                                              message, &heartbeat);
}

static void send_heartbeat(struct ca_mavlink_server *server,
                           const struct route *route, uint8_t component_id,
                           uint8_t type)
{
    mavlink_message_t message;
    pack_heartbeat(server, component_id, type, &message);
    (void)send_message(server, route, &message);
}

#if APCAM_GIMBAL_OPTIONAL
/* Without a gimbal no gimbal component is advertised: no heartbeat, device
 * information or command replies. Other targets always have a gimbal and
 * compile the original code, unchanged. */
static bool have_gimbal(const struct ca_mavlink_server *server)
{
    return ca_backend_has_gimbal(server->backend);
}
#endif

static void broadcast_heartbeats(struct ca_mavlink_server *server)
{
    mavlink_message_t message;
    pack_heartbeat(server, server->camera_component_id, MAV_TYPE_CAMERA, &message);
    broadcast_message(server, &message);
#if APCAM_GIMBAL_OPTIONAL
    if (!have_gimbal(server)) return;
#endif
    pack_heartbeat(server, server->gimbal_component_id, MAV_TYPE_GIMBAL, &message);
    broadcast_message(server, &message);
}

static uint8_t send_ack(struct ca_mavlink_server *server, const struct route *route,
                     uint8_t component_id, uint16_t command, uint8_t result,
                     const mavlink_message_t *request)
{
    mavlink_message_t message;
    mavlink_command_ack_t ack = {};
    ack.command = command;
    ack.result = result;
    ack.progress = 255U;
    ack.target_system = request->sysid;
    ack.target_component = request->compid;
    (void)mavlink_msg_command_ack_encode_status(server->system_id, component_id,
                                                &server->encode_status,
                                                &message, &ack);
    (void)send_message(server, route, &message);
    return result;
}

/* Fill a fixed-size string field; a full field carries no terminator. */
static void put_text(char *destination, size_t size, const char *text)
{
    size_t length = strlen(text);
    if (length > size) length = size;
    memcpy(destination, text, length);
    if (length < size) memset(destination+length, 0, size-length);
}

static bool interface_ipv4(char address[INET_ADDRSTRLEN])
{
    struct ifaddrs *interfaces = NULL;
    struct in_addr selected = {};
    unsigned selected_score = 0U;

    if (getifaddrs(&interfaces) < 0) return false;
    for (const struct ifaddrs *entry = interfaces; entry != NULL;
         entry = entry->ifa_next) {
        if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_UP) == 0U ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0U) {
            continue;
        }
        struct in_addr candidate =
            ((const struct sockaddr_in *)entry->ifa_addr)->sin_addr;
        uint32_t host = ntohl(candidate.s_addr);
        unsigned score = (host & UINT32_C(0xffffff00)) ==
                                 UINT32_C(0xc0a89000)
                             ? 3U
                             : ((host & UINT32_C(0xffff0000)) ==
                                        UINT32_C(0xa9fe0000)
                                    ? 1U
                                    : 2U);
        /* Prefer the MT11 control subnet, then make equal-score selection
         * independent of getifaddrs() enumeration order. */
        if (score > selected_score ||
            (score == selected_score &&
             ntohl(candidate.s_addr) < ntohl(selected.s_addr))) {
            selected = candidate;
            selected_score = score;
        }
    }
    freeifaddrs(interfaces);
    return selected_score != 0U &&
           inet_ntop(AF_INET, &selected, address, INET_ADDRSTRLEN) != NULL;
}

static bool route_local_ipv4(const struct ca_mavlink_server *server,
                             const struct route *route,
                             char address[INET_ADDRSTRLEN])
{
    struct sockaddr_in local = {};
    socklen_t length = sizeof(local);
    int probe = -1;
    int fd;

    // Serial routes have no IP peer from which to infer a local address.
    if (route->kind == ROUTE_UART || route->kind == ROUTE_MCU) {
        return interface_ipv4(address);
    }
    if (route->kind == ROUTE_TCP) {
        if (route->client >= CA_MAVLINK_CLIENTS) return false;
        fd = server->clients[route->client].fd;
    } else {
        probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (probe < 0 ||
            connect(probe, (const struct sockaddr *)&route->address,
                    route->address_length) < 0) {
            if (probe >= 0) close(probe);
            return false;
        }
        fd = probe;
    }
    bool ok = fd >= 0 &&
              getsockname(fd, (struct sockaddr *)&local, &length) == 0 &&
              local.sin_family == AF_INET &&
              local.sin_addr.s_addr != htonl(INADDR_ANY) &&
              inet_ntop(AF_INET, &local.sin_addr, address,
                        INET_ADDRSTRLEN) != NULL;
    if (probe >= 0) close(probe);
#if !APCAM_HAVE_NETWORK_CONFIG
    if (ok && (ntohl(local.sin_addr.s_addr) >> 24) == 127U) return interface_ipv4(address);
#endif
    return ok;
}

static void send_camera_information(struct ca_mavlink_server *server,
                                    const struct route *route)
{
    mavlink_message_t message;
    mavlink_camera_information_t info = {};
    info.time_boot_ms = boot_ms(server);
    info.firmware_version = 1U;
    info.focal_length = NAN;
    info.sensor_size_h = NAN;
    info.sensor_size_v = NAN;
    info.flags = CAMERA_CAP_FLAGS_CAPTURE_VIDEO |
                 CAMERA_CAP_FLAGS_CAPTURE_IMAGE | CAMERA_CAP_FLAGS_HAS_MODES |
                 CAMERA_CAP_FLAGS_CAN_CAPTURE_IMAGE_IN_VIDEO_MODE |
                 CAMERA_CAP_FLAGS_CAN_CAPTURE_VIDEO_IN_IMAGE_MODE |
                 CAMERA_CAP_FLAGS_HAS_BASIC_ZOOM |
                 CAMERA_CAP_FLAGS_HAS_BASIC_FOCUS |
                 CAMERA_CAP_FLAGS_HAS_VIDEO_STREAM;
    if(ca_media_survey_available(server->media,server->settings.survey.lens)) info.flags |= CAMERA_CAP_FLAGS_HAS_IMAGE_SURVEY_MODE;
#ifdef CAMERA_APP_ZT30_RAM
    info.flags=CAMERA_CAP_FLAGS_HAS_VIDEO_STREAM;
#endif
    info.cam_definition_version = server->definition_version;
    if(ca_media_tracking_available(server->media))
        info.flags |= CAMERA_CAP_FLAGS_HAS_TRACKING_RECTANGLE | CAMERA_CAP_FLAGS_HAS_TRACKING_POINT;
    info.gimbal_device_id = server->gimbal_component_id;
#if APCAM_GIMBAL_OPTIONAL
    if (!have_gimbal(server)) info.gimbal_device_id = 0U;
#endif
#if APCAM_HAVE_THERMAL
    info.flags |= CAMERA_CAP_FLAGS_HAS_THERMAL_RANGE;
#endif
#if !APCAM_HAVE_FOCUS
    info.flags &= ~CAMERA_CAP_FLAGS_HAS_BASIC_FOCUS;
#endif
#if !APCAM_HAVE_ZOOM
    info.flags &= ~CAMERA_CAP_FLAGS_HAS_BASIC_ZOOM;
#endif
#if !APCAM_HAVE_PHOTO
    info.flags &= ~(CAMERA_CAP_FLAGS_CAPTURE_IMAGE | CAMERA_CAP_FLAGS_HAS_MODES |
                    CAMERA_CAP_FLAGS_CAN_CAPTURE_IMAGE_IN_VIDEO_MODE |
                    CAMERA_CAP_FLAGS_CAN_CAPTURE_VIDEO_IN_IMAGE_MODE);
#endif
#ifdef CAMERA_APP_ZT30_DEV
    // Native video recording and photos use independently owned encoders.
    // Optical setters remain separate from the qualified stream controls.
    info.flags = CAMERA_CAP_FLAGS_HAS_VIDEO_STREAM | CAMERA_CAP_FLAGS_CAPTURE_IMAGE |
                 CAMERA_CAP_FLAGS_CAPTURE_VIDEO | CAMERA_CAP_FLAGS_HAS_MODES |
                 CAMERA_CAP_FLAGS_CAN_CAPTURE_IMAGE_IN_VIDEO_MODE |
                 CAMERA_CAP_FLAGS_CAN_CAPTURE_VIDEO_IN_IMAGE_MODE;
    if(ca_media_survey_available(server->media,server->settings.survey.lens))
        info.flags |= CAMERA_CAP_FLAGS_HAS_IMAGE_SURVEY_MODE;
    // Optional gimbal identity above is based on actual fresh MCU feedback.
#endif
    // Capabilities are static: autopilots may cache the first reply before
    // thermal startup completes. Individual capture requests enforce freshness.
    snprintf(info.cam_definition_uri, sizeof(info.cam_definition_uri),
             "mftp://[;comp=%u]" CA_CAMERA_DEFINITION_PATH, server->camera_component_id);
    unsigned width, height;
    ca_video_resolution_size(server->settings.main_resolution, &width, &height);
    info.resolution_h = (uint16_t)width;
    info.resolution_v = (uint16_t)height;
#ifdef CAMERA_APP_ZT30_DEV
    // CAMERA_INFORMATION describes photographs; streaming dimensions belong
    // to VIDEO_STREAM_INFORMATION. Full-scope native capture includes zoom 4K.
    info.resolution_h=server->photo_scope==CA_PHOTO_SCOPE_THERMAL?640:3840;
    info.resolution_v=server->photo_scope==CA_PHOTO_SCOPE_THERMAL?512:2160;
#endif
    put_text((char *)info.vendor_name, sizeof(info.vendor_name), "ArduPilot");
    put_text((char *)info.model_name, sizeof(info.model_name),
             APCAM_MODEL_NAME);
    (void)mavlink_msg_camera_information_encode_status(
        server->system_id, server->camera_component_id,
        &server->encode_status, &message, &info);
    (void)send_message(server, route, &message);
}

static float selected_zoom_max(const struct ca_mavlink_server *server)
{
    if (!APCAM_ZOOM_SELECTS_LENS) {
        if (ca_media_thermal_main(server->media)) return 1;
        const auto *lens = APC_Camera::get_singleton().lens(ca_media_lens(server->media));
        return lens ? lens->optical_zoom_max() * lens->digital_zoom_max() : 1;
    }
    return APCAM_HAVE_ZOOM_LENS && ca_media_lens(server->media) == CA_MEDIA_LENS_ZOOM ?
        APCAM_ZOOM_LENS_OPTICAL_MAX : APCAM_ZOOM_CONTROL_MAX;
}

static float selected_zoom(const struct ca_mavlink_server *server)
{
    if (!APCAM_ZOOM_SELECTS_LENS && ca_media_thermal_main(server->media)) return 1;
    return ca_media_lens_zoom(server->media, ca_media_lens(server->media));
}

static int set_selected_zoom(struct ca_mavlink_server *server, float zoom)
{
#if APCAM_HAVE_ZOOM_LENS
    if (!APCAM_ZOOM_SELECTS_LENS && ca_media_thermal_main(server->media)) { errno=ENOTSUP; return -1; }
    enum ca_media_lens lens = ca_media_lens(server->media);
    if (lens == CA_MEDIA_LENS_ZOOM) {
        /* Range/step arithmetic can land a float ULP above a calibration
         * boundary (e.g. 2.2f - 0.1f), incorrectly selecting the next row.
         * Snap only rounding noise; retain meaningful fractional requests. */
        float tenth = roundf(zoom * 10.0f) / 10.0f;
        if (fabsf(zoom - tenth) < 0.000001f) zoom = tenth;
    }
    return ca_media_set_lens_zoom(server->media, lens, zoom);
#else
    return ca_backend_set_zoom(server->backend, zoom);
#endif
}

static float zoom_percent(const struct ca_mavlink_server *server)
{
    float zoom = selected_zoom(server);
    float maximum = selected_zoom_max(server);
    return maximum > 1 ? (zoom - 1.0f) * (100.0f / (maximum - 1.0f)) : 0;
}

static void send_camera_settings(struct ca_mavlink_server *server,
                                 const struct route *route)
{
    mavlink_message_t message;
    mavlink_camera_settings_t settings = {
        .time_boot_ms = boot_ms(server),
        .mode_id = server->camera_mode,
        .zoomLevel = zoom_percent(server),
        .focusLevel = server->focus_percent,
    };
    (void)mavlink_msg_camera_settings_encode_status(
        server->system_id, server->camera_component_id,
        &server->encode_status, &message, &settings);
    if(route) (void)send_message(server, route, &message);
    else broadcast_message(server,&message);
}

static float storage_available(const struct ca_mavlink_server *server,
                               float *total, float *used, uint8_t *status)
{
    struct statvfs info;
    if (statvfs(server->capture_root, &info) < 0) {
        *total = NAN;
        *used = NAN;
        *status = STORAGE_STATUS_NOT_SUPPORTED;
        return NAN;
    }
    double scale = (double)info.f_frsize / (1024.0 * 1024.0);
    *total = (float)((double)info.f_blocks * scale);
    float available = (float)((double)info.f_bavail * scale);
    *used = *total - available;
    *status = STORAGE_STATUS_READY;
    return available;
}

static void send_storage_information(struct ca_mavlink_server *server,
                                     const struct route *route)
{
    mavlink_message_t message;
    mavlink_storage_information_t storage = {};
    storage.time_boot_ms = boot_ms(server);
    storage.read_speed = NAN;
    storage.write_speed = NAN;
    storage.storage_id = 1U;
    storage.storage_count = 1U;
    storage.type = STORAGE_TYPE_MICROSD;
    storage.available_capacity = storage_available(
        server, &storage.total_capacity, &storage.used_capacity,
        &storage.status);
    put_text(storage.name, sizeof(storage.name), "Camera microSD");
    (void)mavlink_msg_storage_information_encode_status(
        server->system_id, server->camera_component_id,
        &server->encode_status, &message, &storage);
    (void)send_message(server, route, &message);
}

static uint32_t recording_time_ms(const struct ca_mavlink_server *server)
{
    if (!ca_media_recording(server->media) || server->recording_started_ms == 0U) {
        return 0U;
    }
    return (uint32_t)(monotonic_ms() - server->recording_started_ms);
}

static void pack_capture_status(struct ca_mavlink_server *server,
                                mavlink_message_t *message)
{
    float total, used;
    uint8_t storage_status;
    mavlink_camera_capture_status_t status = {
        .time_boot_ms = boot_ms(server),
        .image_interval = server->capture_interval_s,
        .recording_time_ms = recording_time_ms(server),
        .available_capacity =
            storage_available(server, &total, &used, &storage_status),
        .image_status = (uint8_t)(server->captures_remaining != 0 || survey_running(server) ? 1U : 0U),
        .video_status = (uint8_t)(ca_media_recording(server->media) ? 1U : 0U),
        .image_count = (int32_t)server->image_count,
    };
    (void)mavlink_msg_camera_capture_status_encode_status(
        server->system_id, server->camera_component_id,
        &server->encode_status, message, &status);
}

static void send_capture_status(struct ca_mavlink_server *server,
                                const struct route *route)
{
    mavlink_message_t message;
    pack_capture_status(server, &message);
    (void)send_message(server, route, &message);
}

static void broadcast_capture_status(struct ca_mavlink_server *server)
{
    mavlink_message_t message;
    pack_capture_status(server, &message);
    broadcast_message(server, &message);
}

static void euler_to_quaternion(float roll, float pitch, float yaw, float q[4]);

static void broadcast_image_captured(struct ca_mavlink_server *server,
                                     int32_t image_index, bool success)
{
    mavlink_message_t message;
    struct ca_metadata metadata;
    float q[4] = {NAN, NAN, NAN, NAN};
    mavlink_camera_image_captured_t captured = {};
    captured.time_utc = realtime_us();
    captured.time_boot_ms = boot_ms(server);
    captured.capture_result = (int8_t)(success ? 1 : -1);
    captured.image_index = image_index;
    ca_metadata_snapshot(&metadata);
    if (metadata.have_position) {
        captured.lat = metadata.lat_e7;
        captured.lon = metadata.lon_e7;
        captured.alt = (int32_t)(metadata.alt_amsl_m * 1000.0f);
        captured.relative_alt = (int32_t)(metadata.alt_relative_m * 1000.0f);
    }
    if (metadata.have_gimbal_attitude) {
        euler_to_quaternion(metadata.gimbal_roll_rad, metadata.gimbal_pitch_rad,
                            metadata.have_vehicle_attitude
                                ? metadata.vehicle_yaw_rad +
                                      metadata.gimbal_yaw_rad
                                : metadata.gimbal_yaw_rad,
                            q);
    }
    for (unsigned i = 0; i < 4U; i++) captured.q[i] = q[i];
    (void)mavlink_msg_camera_image_captured_encode_status(
        server->system_id, server->camera_component_id,
        &server->encode_status, &message, &captured);
    broadcast_message(server, &message);
}

/* Whether a stream is streaming; false for a stream the target does not have. */
static bool stream_running(const struct ca_mavlink_server *server, unsigned index, bool misb=false)
{
    return index < APCAM_NUM_STREAMS &&
        (misb ? server->misb_stream_enabled[index] : server->stream_enabled[index]);
}

static unsigned base_stream_count()
{
    // Reserve the raw stream ID even while its source is temporarily stale.
    return APCAM_NUM_STREAMS + (APCAM_HAVE_RAW_THERMAL ? 1U : 0U);
}

/* Primary IDs use the selected format; compatibility previews follow raw
 * thermal (ID 3). SupportProxy relays only the APCG preview format. */
static unsigned stream_count(const ca_mavlink_server *server, const route *destination=nullptr)
{
    return base_stream_count() +
        (server->settings.video_telemetry_format==CA_VIDEO_TELEMETRY_MISB &&
         (!destination || destination->kind!=ROUTE_PROXY) ? APCAM_NUM_STREAMS : 0U);
}

static unsigned stream_source(const ca_mavlink_server *server, unsigned id)
{
    if(id>=1 && id<=APCAM_NUM_STREAMS) return id;
    const unsigned base=base_stream_count();
    return id>base && id<=stream_count(server) ? id-base : 0U;
}

static bool stream_uses_misb(const ca_mavlink_server *server, unsigned id,
                              const route *destination=nullptr)
{
    return server->settings.video_telemetry_format==CA_VIDEO_TELEMETRY_MISB &&
        id>=1 && id<=APCAM_NUM_STREAMS && (!destination || destination->kind!=ROUTE_PROXY);
}

static bool raw_thermal_stream(unsigned id)
{
    return APCAM_HAVE_RAW_THERMAL && id==CA_RAW_THERMAL_STREAM_ID;
}

static void stream_properties(const struct ca_mavlink_server *server,
                              unsigned stream_id, unsigned *width,
                              unsigned *height, enum ca_video_codec *codec,
                              uint16_t *flags, const route *destination=nullptr)
{
    const bool misb=stream_uses_misb(server,stream_id,destination);
    const unsigned source=stream_source(server,stream_id);
    if(source) stream_id=source;
    bool thermal = APCAM_HAVE_THERMAL_VIDEO &&
        (ca_media_thermal_main(server->media) ? stream_id == 1U
                                             : stream_id == 2U);
    #if APCAM_NUM_STREAMS > 1
    if (stream_id == 2U) {
        ca_video_resolution_size(server->settings.sub_resolution, width, height);
        *codec = server->settings.sub_codec;
        *flags = stream_running(server, 1U, misb) ? (unsigned)VIDEO_STREAM_STATUS_FLAGS_RUNNING
                                            : 0U;
    } else
    #endif
    {
        ca_video_resolution_size(server->settings.main_resolution, width, height);
        *codec = server->settings.main_codec;
        *flags = stream_running(server, 0U, misb) ? (unsigned)VIDEO_STREAM_STATUS_FLAGS_RUNNING
                                            : 0U;
    }
    if (thermal) {
        *width = APCAM_THERMAL_STREAM_WIDTH;
        *height = APCAM_THERMAL_STREAM_HEIGHT;
        *flags |= VIDEO_STREAM_STATUS_FLAGS_THERMAL;
#if APCAM_HAVE_THERMAL
        *flags |= VIDEO_STREAM_STATUS_FLAGS_THERMAL_RANGE_ENABLED;
#endif
    }
#ifdef CAMERA_APP_ZT30_DEV
    if(!ca_media_frame_rate(server->media,thermal))
        *flags &= ~VIDEO_STREAM_STATUS_FLAGS_RUNNING;
#endif
}

// CAMERA_FOV_STATUS describes the current main camera source. Unlike the
// stream messages it carries both angles as floats, without degree rounding.
static void send_camera_fov(struct ca_mavlink_server *server, const struct route *route)
{
    mavlink_camera_fov_status_t status = {};
    status.time_boot_ms = boot_ms(server);
    status.lat_camera = status.lon_camera = status.alt_camera = INT32_MAX;
    status.lat_image = status.lon_image = status.alt_image = INT32_MAX;
    for (float &v : status.q) v = NAN;
    ca_metadata metadata;
    ca_metadata_snapshot(&metadata);
    if (metadata.have_position) {
        status.lat_camera = metadata.lat_e7;
        status.lon_camera = metadata.lon_e7;
        const double alt_mm = double(metadata.alt_amsl_m) * 1000;
        if (isfinite(alt_mm) && alt_mm > INT32_MIN && alt_mm < INT32_MAX)
            status.alt_camera = (int32_t)llround(alt_mm);
    }
    if (metadata.have_gimbal_attitude && metadata.have_vehicle_attitude) {
        euler_to_quaternion(metadata.gimbal_roll_rad, metadata.gimbal_pitch_rad,
                            metadata.vehicle_yaw_rad + metadata.gimbal_yaw_rad, status.q);
    }
    const bool thermal = ca_media_thermal_main(server->media);
    unsigned width, height;
    ca_video_resolution_size(server->settings.main_resolution, &width, &height);
#if APCAM_HAVE_THERMAL_VIDEO
    // Use the native thermal sensor aspect, before encoder stretching/padding.
    if (thermal) { width = APCAM_LENS3_WIDTH; height = APCAM_LENS3_HEIGHT; }
#endif
    status.hfov = ca_media_hfov(server->media, thermal);
    status.vfov = ca_video_vfov(status.hfov, width, height);
    if (ca_media_side_by_side(server->media) || !(status.hfov > 0 && status.hfov < 180))
        status.hfov = status.vfov = NAN;
    else if (!(status.vfov > 0)) status.vfov = NAN;
    mavlink_message_t message;
    mavlink_msg_camera_fov_status_encode_status(server->system_id, server->camera_component_id,
                                               &server->encode_status, &message, &status);
    (void)send_message(server, route, &message);
}

/* The main stream bitrate in bits per second, as MAVLink reports it. A target
 * without the setting, or with it set to automatic (0), reports the nominal
 * 4096 kbps. */
static uint32_t main_bitrate_bps(const struct ca_mavlink_server *server)
{
#if APCAM_HAVE_VIDEO_BITRATE
    if (server->settings.main_bitrate_kbps) return server->settings.main_bitrate_kbps * 1000U;
#else
    (void)server;
#endif
    return 4096000U;
}

static void send_stream_information(struct ca_mavlink_server *server,
                                    const struct route *route,
                                    unsigned stream_id)
{
    mavlink_message_t message;
    mavlink_video_stream_information_t info = {};
#if defined(CAMERA_APP_ZT30_RAM) || defined(CAMERA_APP_ZT30_DEV)
    info.bitrate=1570000U;
#else
    info.bitrate = main_bitrate_bps(server);
#endif
    info.count = stream_count(server,route);
    info.type = VIDEO_STREAM_TYPE_RTSP;
    unsigned width, height;
    enum ca_video_codec codec;
    if (stream_id < 1U || stream_id > stream_count(server,route)) stream_id = 1U;
    const unsigned source=stream_source(server,stream_id);
    const bool misb=stream_uses_misb(server,stream_id,route);
    stream_properties(server, stream_id, &width, &height, &codec, &info.flags,route);
    bool thermal = (info.flags & VIDEO_STREAM_STATUS_FLAGS_THERMAL) != 0U;
    info.framerate = ca_media_frame_rate(server->media, thermal);
#ifdef CAMERA_APP_ZT30_DEV
    info.bitrate=thermal?1000000U:1570000U;
#endif
    info.resolution_h = (uint16_t)width;
    info.resolution_v = (uint16_t)height;
    info.hfov = (uint16_t)lroundf(ca_media_hfov(server->media, thermal));
    info.stream_id = (uint8_t)stream_id;
    put_text(info.name, sizeof(info.name), misb ?
        (thermal ? "Thermal MISB KLV" : "Visible MISB KLV") :
        (stream_id>base_stream_count() ? (thermal ? "Thermal APCG JSON" : "Visible APCG JSON") :
                                       (thermal ? "Thermal" : "Visible")));
    char uri[160];
    int length = 0;
    if (route->kind == ROUTE_PROXY) {
        const struct ca_support_config *support = &server->settings.support;
        unsigned port = stream_id == 1U ? support->video1_port : support->video2_port;
        put_text(info.name, sizeof(info.name), stream_id == 1U ? support->video1_name : support->video2_name);
        info.type = VIDEO_STREAM_TYPE_MPEG_TS;
        if (port) length = snprintf(uri, sizeof(uri), "http://%s:%u/v%u.ts", support->host, port, stream_id);
        else info.flags &= ~VIDEO_STREAM_STATUS_FLAGS_RUNNING;
    } else {
        char host[INET_ADDRSTRLEN] = "0.0.0.0";
#if !APCAM_HAVE_NETWORK_CONFIG
        // A configured URI (validated when loaded) replaces the one built here.
        if (stream_id == 1U && server->settings.video1_uri[0]) {
            length = snprintf(uri, sizeof(uri), "%s", server->settings.video1_uri);
        } else
#endif
        {
            (void)route_local_ipv4(server, route, host);
            length = snprintf(uri, sizeof(uri), "rtsp://%s:%u/video%u%s", host,
                              server->rtsp_port, source ? source : stream_id, misb ? ".misb" : "");
        }
    }
    if (length > 0 && (size_t)length < sizeof(uri)) put_text(info.uri, sizeof(info.uri), uri);
    if (raw_thermal_stream(stream_id)) {
        info.type = CA_VIDEO_STREAM_TYPE_HTTP_MATROSKA;
        info.bitrate = 0; // lossless content-dependent bitrate
        info.framerate = ca_thermal_stream_fps();
        info.resolution_h = 640; info.resolution_v = 512;
        info.flags = VIDEO_STREAM_STATUS_FLAGS_THERMAL |
            (ca_thermal_stream_available() && ca_thermal_stream_enabled() ? VIDEO_STREAM_STATUS_FLAGS_RUNNING : 0);
        put_text(info.name, sizeof(info.name), "Raw Thermal (16-bit)");
        char host[INET_ADDRSTRLEN] = "0.0.0.0";
        (void)route_local_ipv4(server, route, host);
        snprintf(uri,sizeof(uri),"http://%s:%u/thermal.mkv",host,ca_thermal_stream_port());
        if (!ca_thermal_stream_port()) { uri[0] = 0; info.flags &= ~VIDEO_STREAM_STATUS_FLAGS_RUNNING; }
        if (route->kind == ROUTE_PROXY) {
            const auto &support = server->settings.support;
            put_text(info.name, sizeof(info.name), support.video3_name);
            if (support.enabled && support.video3_port) {
                snprintf(uri, sizeof(uri), "http://%s:%u/v3.mkv", support.host, support.video3_port);
                if (ca_thermal_stream_available() && ca_thermal_stream_enabled()) info.flags |= VIDEO_STREAM_STATUS_FLAGS_RUNNING;
            }
            else { uri[0] = 0; info.flags &= ~VIDEO_STREAM_STATUS_FLAGS_RUNNING; }
        }
        put_text(info.uri,sizeof(info.uri),uri);
        info.encoding = VIDEO_STREAM_ENCODING_UNKNOWN; // FFV1 is identified by Matroska CodecID
        info.hfov = (uint16_t)lroundf(ca_media_hfov(server->media, true));
    } else info.encoding = codec == CA_VIDEO_H265 ? VIDEO_STREAM_ENCODING_H265
                                           : VIDEO_STREAM_ENCODING_H264;
    (void)mavlink_msg_video_stream_information_encode_status(
        server->system_id, server->camera_component_id,
        &server->encode_status, &message, &info);
    (void)send_message(server, route, &message);
}

static void send_stream_status(struct ca_mavlink_server *server,
                               const struct route *route, unsigned stream_id)
{
    mavlink_message_t message;
    mavlink_video_stream_status_t status = {};
    status.bitrate = main_bitrate_bps(server);
    unsigned width, height;
    enum ca_video_codec codec;
    if (stream_id < 1U || stream_id > stream_count(server,route)) stream_id = 1U;
    stream_properties(server, stream_id, &width, &height, &codec,
                      &status.flags,route);
    (void)codec;
    status.framerate = ca_media_frame_rate(server->media,
        (status.flags & VIDEO_STREAM_STATUS_FLAGS_THERMAL) != 0U);
#ifdef CAMERA_APP_ZT30_DEV
    status.bitrate=(status.flags & VIDEO_STREAM_STATUS_FLAGS_THERMAL)?1000000U:1570000U;
#endif
    status.resolution_h = (uint16_t)width;
    status.resolution_v = (uint16_t)height;
    status.hfov = (uint16_t)lroundf(ca_media_hfov(server->media,
        (status.flags & VIDEO_STREAM_STATUS_FLAGS_THERMAL) != 0U));
    if (raw_thermal_stream(stream_id)) {
        status.bitrate = 0; status.framerate = ca_thermal_stream_fps();
        status.resolution_h = 640; status.resolution_v = 512;
        status.hfov = (uint16_t)lroundf(ca_media_hfov(server->media,true));
        status.flags = VIDEO_STREAM_STATUS_FLAGS_THERMAL |
            (ca_thermal_stream_available() && ca_thermal_stream_enabled() ? VIDEO_STREAM_STATUS_FLAGS_RUNNING : 0);
        const bool reachable = route->kind == ROUTE_PROXY ?
            server->settings.support.enabled && server->settings.support.video3_port : ca_thermal_stream_port() != 0;
        if (!reachable) status.flags &= ~VIDEO_STREAM_STATUS_FLAGS_RUNNING;
    }
    status.stream_id = (uint8_t)stream_id;
    (void)mavlink_msg_video_stream_status_encode_status(
        server->system_id, server->camera_component_id,
        &server->encode_status, &message, &status);
    (void)send_message(server, route, &message);
}

static bool send_stream_selection(struct ca_mavlink_server *server,
                                  const struct route *route,
                                  uint32_t message_id, unsigned stream_id)
{
    if (stream_id > stream_count(server,route)) return false;
    unsigned first = stream_id == 0U ? 1U : stream_id;
    unsigned last = stream_id == 0U ? stream_count(server,route) : stream_id;
    for (unsigned stream = first; stream <= last; stream++) {
        if (message_id == MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION) {
            send_stream_information(server, route, stream);
        } else {
            send_stream_status(server, route, stream);
        }
    }
    return true;
}

#if APCAM_HAVE_THERMAL
static unsigned thermal_stream_id(const struct ca_mavlink_server *server)
{
    return ca_media_thermal_main(server->media) ? 1U : 2U;
}

static bool thermal_selection_valid(float stream, float camera)
{
    return isfinite(stream) && stream >= 0 && stream <= APCAM_NUM_STREAMS &&
           floorf(stream) == stream && camera == 0;
}

static float thermal_coordinate(unsigned pixel, unsigned size, bool rotated)
{
    pixel = ca_thermal_display_pixel(pixel, size, rotated);
    return size > 1U && pixel < size ? (float)pixel / (size - 1U) : NAN;
}

static uint8_t send_thermal_range(struct ca_mavlink_server *server,
                                 const struct route *route, unsigned selection)
{
    unsigned stream = thermal_stream_id(server);
    if (selection != 0U && selection != stream) return MAV_RESULT_UNSUPPORTED;
    struct ca_thermal_range range;
    if (!ca_media_thermal_range(server->media, &range)) {
        return MAV_RESULT_TEMPORARILY_REJECTED;
    }
    // Normalize sensor pixels after the same rotation as the displayed image.
    mavlink_camera_thermal_range_t report = {
        .time_boot_ms = boot_ms(server),
        .max = range.maximum_centi_c * 0.01f,
        .max_point_x = thermal_coordinate(range.maximum_x, APCAM_LENS3_WIDTH, range.rotated_180),
        .max_point_y = thermal_coordinate(range.maximum_y, APCAM_LENS3_HEIGHT, range.rotated_180),
        .min = range.minimum_centi_c * 0.01f,
        .min_point_x = thermal_coordinate(range.minimum_x, APCAM_LENS3_WIDTH, range.rotated_180),
        .min_point_y = thermal_coordinate(range.minimum_y, APCAM_LENS3_HEIGHT, range.rotated_180),
        .stream_id = (uint8_t)stream,
        .camera_device_id = 0,
    };
    mavlink_message_t message;
    (void)mavlink_msg_camera_thermal_range_encode_status(server->system_id,
        server->camera_component_id, &server->encode_status, &message, &report);
    if (route != NULL) (void)send_message(server, route, &message);
    else broadcast_message(server, &message);
    return MAV_RESULT_ACCEPTED;
}

static uint8_t set_thermal_interval(struct ca_mavlink_server *server,
                                   const float params[7])
{
    if (!thermal_selection_valid(params[2], params[3]) ||
        !isfinite(params[1]) || params[1] < -1.0f ||
        (params[1] < 0.0f && params[1] != -1.0f) ||
        (double)params[1] > INT32_MAX) return MAV_RESULT_DENIED;
    unsigned selection = (unsigned)params[2];
    if (selection != 0U && selection != thermal_stream_id(server) && params[1] >= 0) {
        return MAV_RESULT_UNSUPPORTED;
    }
    uint32_t interval = THERMAL_DEFAULT_INTERVAL_MS;
    if (params[1] == -1.0f) interval = 0U;
    else if (params[1] > 0.0f) {
        // No useful data faster than the thermal sensor's frame rate.
        float minimum = 1000.0f / APCAM_THERMAL_FRAME_RATE;
        interval = (uint32_t)ceilf(fmaxf(minimum, params[1] / 1000.0f));
    }
    for (unsigned i = 0; i < APCAM_NUM_STREAMS; i++) {
        if (selection != 0U && selection != i + 1U) continue;
        server->thermal_interval_ms[i] = interval;
        server->last_thermal_ms[i] = 0;
    }
    return MAV_RESULT_ACCEPTED;
}
#endif

static void send_gimbal_information(struct ca_mavlink_server *server,
                                    const struct route *route)
{
#if APCAM_GIMBAL_OPTIONAL
    if (!have_gimbal(server)) return;
#endif
    mavlink_message_t message;
    uint32_t flags = GIMBAL_DEVICE_CAP_FLAGS_HAS_NEUTRAL |
                     GIMBAL_DEVICE_CAP_FLAGS_HAS_PITCH_AXIS |
                     GIMBAL_DEVICE_CAP_FLAGS_HAS_PITCH_FOLLOW |
                     GIMBAL_DEVICE_CAP_FLAGS_HAS_YAW_AXIS |
                     GIMBAL_DEVICE_CAP_FLAGS_HAS_YAW_FOLLOW |
                     GIMBAL_DEVICE_CAP_FLAGS_SUPPORTS_YAW_IN_EARTH_FRAME;
#ifdef CAMERA_APP_ZT30_DEV
    flags &= ~GIMBAL_DEVICE_CAP_FLAGS_HAS_NEUTRAL;
#endif
    if (server->settings.position_targeting) {
        flags |= GIMBAL_DEVICE_CAP_FLAGS_CAN_POINT_LOCATION_GLOBAL;
    }
    mavlink_gimbal_device_information_t info = {};
    info.time_boot_ms = boot_ms(server);
    info.firmware_version = 1U;
    info.hardware_version = 1U;
    info.roll_min = NAN;
    info.roll_max = NAN;
    info.pitch_min = APCAM_GIMBAL_PITCH_MIN * PI_F / 180.0f;
    info.pitch_max = APCAM_GIMBAL_PITCH_MAX * PI_F / 180.0f;
    info.yaw_min = APCAM_GIMBAL_YAW_MIN * PI_F / 180.0f;
    info.yaw_max = APCAM_GIMBAL_YAW_MAX * PI_F / 180.0f;
    info.cap_flags = (uint16_t)flags;
    info.cap_flags2 = flags;
    put_text(info.vendor_name, sizeof(info.vendor_name), "ArduPilot");
    put_text(info.model_name, sizeof(info.model_name),
             APCAM_MODEL_NAME);
    put_text(info.custom_name, sizeof(info.custom_name), "AP CameraGimbal");
    (void)mavlink_msg_gimbal_device_information_encode_status(
        server->system_id, server->gimbal_component_id,
        &server->encode_status, &message, &info);
    if (route) (void)send_message(server, route, &message);
    else broadcast_message(server, &message);
}

static void euler_to_quaternion(float roll, float pitch, float yaw, float q[4])
{
    float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
    q[0] = cr * cp * cy + sr * sp * sy;
    q[1] = sr * cp * cy - cr * sp * sy;
    q[2] = cr * sp * cy + sr * cp * sy;
    q[3] = cr * cp * sy - sr * sp * cy;
}

static float wrap_pi_f(float angle)
{
    while (angle > PI_F) angle -= 2.0f * PI_F;
    while (angle < -PI_F) angle += 2.0f * PI_F;
    return angle;
}

static bool current_vehicle_attitude(const struct ca_mavlink_server *server,
                                     float *yaw, float *yaw_rate)
{
    uint64_t now = monotonic_ms();
    if (!server->have_vehicle_attitude ||
        now - server->vehicle_attitude_updated_ms >
            VEHICLE_ATTITUDE_TIMEOUT_MS) {
        return false;
    }
    float aligned_yaw, aligned_rate;
    if (!server->vehicle_yaw_history.at(now, aligned_yaw, aligned_rate)) return false;
    if (yaw != NULL) *yaw = aligned_yaw;
    if (yaw_rate != NULL) *yaw_rate = aligned_rate;
    return true;
}

static bool current_vehicle_position(const struct ca_mavlink_server *server,
                                     int32_t *lat_e7, int32_t *lon_e7,
                                     float *alt_amsl_m)
{
    uint64_t now = monotonic_ms();
    if (!server->have_vehicle_position ||
        now - server->vehicle_position_updated_ms > VEHICLE_POSITION_TIMEOUT_MS) {
        return false;
    }
    int32_t lat = server->vehicle_lat_e7, lon = server->vehicle_lon_e7;
    float alt = server->vehicle_alt_amsl_m;
    float elapsed = fminf((float)(now - server->vehicle_position_updated_ms),
                          VEHICLE_PREDICTION_MS) * 0.001f;
    /* ROI bearing and vehicle yaw must refer to the same instant. Reusing a
     * stationary position between packets while extrapolating yaw makes the
     * gimbal slew backwards, then jump forwards on the next position packet.
     * Both predictions stop at VEHICLE_PREDICTION_MS and are then held. */
    if (!ca_targeting_predict_position(&lat, &lon, &alt, server->vehicle_vn_m_s,
                                       server->vehicle_ve_m_s, server->vehicle_vd_m_s,
                                       elapsed)) return false;
    if (lat_e7 != NULL) *lat_e7 = lat;
    if (lon_e7 != NULL) *lon_e7 = lon;
    if (alt_amsl_m != NULL) *alt_amsl_m = alt;
    return true;
}

static void request_telemetry_intervals(struct ca_mavlink_server *server,
                                        const struct route *route)
{
    uint64_t now = monotonic_ms();
    if (server->last_telemetry_request_ms != 0U &&
        now - server->last_telemetry_request_ms <
        TELEMETRY_INTERVAL_REQUEST_MS) {
        return;
    }
    const struct {
        uint32_t id;
        uint32_t interval_us;
    } messages[] = {
        {MAVLINK_MSG_ID_GLOBAL_POSITION_INT, 100000U},
        {MAVLINK_MSG_ID_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE, 100000U},
        /* UTC date for recordings, including when GPS time becomes valid later. */
        {MAVLINK_MSG_ID_SYSTEM_TIME, 1000000U},
        {MAVLINK_MSG_ID_TERRAIN_REPORT, 500000U},
        {MAVLINK_MSG_ID_GIMBAL_MANAGER_STATUS, 1000000U},
        {MAVLINK_MSG_ID_NAV_CONTROLLER_OUTPUT, 200000U},
        {MAVLINK_MSG_ID_MISSION_CURRENT, 500000U},
        {MAVLINK_MSG_ID_EXTENDED_SYS_STATE, 1000000U},
    };
    server->last_telemetry_request_ms = now;
    for (unsigned i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
        mavlink_message_t message;
        mavlink_command_long_t command = {};
        command.param1 = (float)messages[i].id;
        command.param2 = (float)messages[i].interval_us;
        command.command = MAV_CMD_SET_MESSAGE_INTERVAL;
        command.target_system = server->autopilot_system_id;
        command.target_component = server->autopilot_component_id;
#if APCAM_GIMBAL_OPTIONAL
        (void)mavlink_msg_command_long_encode_status(
            server->system_id, route->kind == ROUTE_MCU || !have_gimbal(server)
                ? server->camera_component_id : server->gimbal_component_id,
            &server->encode_status, &message, &command);
#else
        (void)mavlink_msg_command_long_encode_status(
            server->system_id, route->kind == ROUTE_MCU ? server->camera_component_id : server->gimbal_component_id,
            &server->encode_status, &message, &command);
#endif
        (void)send_message(server, route, &message);
    }
}

static void stop_tracking_rate(struct ca_mavlink_server *server)
{
    if (server->tracking_rate_active)
        (void)ca_backend_set_gimbal_rates(server->backend, 0, 0);
    if (server->tracking_rate_active) ca_binlog_message("Tracking rate stopped");
    server->tracking_rate_active = false;
    memset(server->tracking_error, 0, sizeof(server->tracking_error));
    memset(server->tracking_integral, 0, sizeof(server->tracking_integral));
    memset(server->tracking_rate, 0, sizeof(server->tracking_rate));
}

static float bounded(float value, float limit)
{
    return fminf(limit, fmaxf(-limit, value));
}

static void update_target_location(struct ca_mavlink_server *server, uint64_t now)
{
    const auto image=ca_media_tracking_status(server->media);
    if(image.state==CA_TRACK_ACQUIRING || image.state==CA_TRACK_ACTIVE || image.state==CA_TRACK_COASTING) {
        server->target_location_active=false;
        stop_tracking_rate(server);
        return;
    }
    if (server->manual_control && *server->manual_control) return;
    bool rate = survey_running(server) || server->settings.tracking_method == CA_TRACK_RATE;
    unsigned interval = rate ? 50U : TARGET_LOCATION_INTERVAL_MS;
    if (!server->target_location_active || !server->settings.position_targeting) {
        stop_tracking_rate(server);
        return;
    }
    if (now - server->last_target_location_ms < interval) return;
    int32_t lat, lon;
    float alt, vehicle_yaw, vehicle_yaw_rate, pitch, yaw_earth;
    float dt = fminf((now - server->last_target_location_ms) * .001f, .1f);
    server->last_target_location_ms = now;
    if (!current_vehicle_position(server, &lat, &lon, &alt) ||
        !current_vehicle_attitude(server, &vehicle_yaw, &vehicle_yaw_rate) ||
        !ca_targeting_global_angles(lat, lon, alt, server->target_lat_e7,
            server->target_lon_e7, server->target_alt_amsl_m, &pitch, &yaw_earth)) {
        stop_tracking_rate(server);
        return;
    }
    float yaw = wrap_pi_f(yaw_earth - vehicle_yaw);
    server->yaw_lock = true;
    if (!rate) {
        stop_tracking_rate(server);
        (void)ca_backend_set_gimbal_angles(server->backend, pitch, yaw);
        return;
    }
    struct ca_gimbal_attitude attitude;
    if (!ca_backend_gimbal_attitude(server->backend, &attitude) ||
        now < attitude.timestamp_ms || now - attitude.timestamp_ms > 500U) {
        stop_tracking_rate(server);
        return;
    }
    if(survey_acquiring(server,pitch,yaw,attitude)) {
        stop_tracking_rate(server);
        (void)ca_backend_set_gimbal_angles(server->backend,pitch,yaw);
        return;
    }
    const float radians = PI_F / 180.0f;
    float pitch_rate, yaw_rate;
    if (!ca_targeting_global_rates(lat, lon, alt, server->target_lat_e7,
            server->target_lon_e7, server->target_alt_amsl_m,
            server->vehicle_vn_m_s, server->vehicle_ve_m_s, server->vehicle_vd_m_s,
            &pitch_rate, &yaw_rate)) {
        stop_tracking_rate(server);
        return;
    }
    float desired[2] = {pitch, yaw};
    float feedback[2] = {attitude.pitch_rad, attitude.yaw_rad};
    // LOS motion is earth-frame. The yaw motor must also cancel vehicle yaw.
    float feed_forward[2] = {pitch_rate, yaw_rate - vehicle_yaw_rate};
    const float minimum[2] = {APCAM_GIMBAL_PITCH_MIN * radians, APCAM_GIMBAL_YAW_MIN * radians};
    const float maximum[2] = {APCAM_GIMBAL_PITCH_MAX * radians, APCAM_GIMBAL_YAW_MAX * radians};
    float output[2];
    for (unsigned i = 0; i < 2; i++) {
        if (desired[i] < minimum[i] || desired[i] > maximum[i]) feed_forward[i] = 0;
        desired[i] = fminf(maximum[i], fmaxf(minimum[i], desired[i]));
        /* MT11 yaw rotates through +/-180: the seam is a coordinate wrap,
         * not a 360-degree pointing error. Limited-travel targets must still
         * traverse their allowed joint range rather than cross a hard stop. */
        float measured = feedback[i] + server->tracking_rate[i] *
            fminf((now - attitude.timestamp_ms) * .001f, .2f);
        float error = desired[i] - measured;
        bool circular = i == 1 && APCAM_GIMBAL_YAW_CONTINUOUS;
        if (circular) error = wrap_pi_f(error);
        float alpha = dt / (.05f + dt);
        server->tracking_error[i] += alpha * (error - server->tracking_error[i]);
        float correction = copysignf(fmaxf(0, fabsf(server->tracking_error[i]) - .2f * radians),
                                     server->tracking_error[i]);
        /* Integrate only near the target, where quantisation and the motor
         * dead zone cause static error. Reset during acquisition or when a
         * target is outside the joint range; never wind up against a stop. */
        if (fabsf(error) < 5 * radians && desired[i] > minimum[i] && desired[i] < maximum[i]) {
            server->tracking_integral[i] = bounded(server->tracking_integral[i] +
                APCAM_TRACKING_RATE_I * correction * dt, 6 * radians);
        } else {
            server->tracking_integral[i] = 0;
        }
        float requested = bounded(feed_forward[i] + 1.2f * correction +
                                  server->tracking_integral[i], APCAM_GIMBAL_RATE_MAX * radians);
        output[i] = server->tracking_rate[i] + bounded(requested - server->tracking_rate[i],
                                                       APCAM_GIMBAL_RATE_MAX * radians * dt);
        if (!circular && ((feedback[i] <= minimum[i] && output[i] < 0) ||
            (feedback[i] >= maximum[i] && output[i] > 0))) output[i] = 0;
        CA_BINLOG(i ? CA_LOG_PIDY : CA_LOG_PIDP, ca_log_pid,
            .target=desired[i]/radians, .actual=feedback[i]/radians,
            .rate=(i ? attitude.yaw_rate_rad_s : attitude.pitch_rate_rad_s)/radians,
            .ff=feed_forward[i]/radians, .error=error/radians,
            .p=1.2f*correction/radians, .i=server->tracking_integral[i]/radians,
            .d=0, .output=output[i]/radians,
            .dt=dt, .age=(now-attitude.timestamp_ms)*.001f);
    }
    if (ca_backend_set_gimbal_rates(server->backend, output[0], output[1]) == 0) {
        server->tracking_rate_active = true;
        memcpy(server->tracking_rate, output, sizeof(output));
    } else {
        stop_tracking_rate(server);
    }
}

static void pack_gimbal_status(struct ca_mavlink_server *server,
                               mavlink_message_t *message,
                               const struct ca_gimbal_attitude *attitude)
{
    float q[4];
    float yaw = attitude->yaw_rad;
    float yaw_rate = attitude->yaw_rate_rad_s;
    float vehicle_yaw;
    float vehicle_yaw_rate;
    uint16_t flags = GIMBAL_DEVICE_FLAGS_ROLL_LOCK |
                     GIMBAL_DEVICE_FLAGS_PITCH_LOCK |
                     GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME;
    // Prefer vehicle yaw aligned with the feedback sample; otherwise the
    // held current yaw keeps the frame stable while history is sparse.
    bool have_vehicle = server->vehicle_yaw_history.at(
        attitude->timestamp_ms, vehicle_yaw, vehicle_yaw_rate) ||
        current_vehicle_attitude(server, &vehicle_yaw, &vehicle_yaw_rate);
    if (have_vehicle) {
        flags |= GIMBAL_DEVICE_FLAGS_ACCEPTS_YAW_IN_EARTH_FRAME;
    }
    if (server->yaw_lock && have_vehicle) {
        yaw = wrap_pi_f(yaw + vehicle_yaw);
        yaw_rate += vehicle_yaw_rate;
        flags &= (uint16_t)~GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME;
        flags |= GIMBAL_DEVICE_FLAGS_YAW_LOCK |
                 GIMBAL_DEVICE_FLAGS_YAW_IN_EARTH_FRAME;
    }
    euler_to_quaternion(attitude->roll_rad, attitude->pitch_rad,
                        yaw, q);
    mavlink_gimbal_device_attitude_status_t status = {};
    status.time_boot_ms = attitude->timestamp_ms >= server->started_ms ?
        uint32_t(attitude->timestamp_ms - server->started_ms) : 0;
    memcpy(status.q, q, sizeof(q));
    status.angular_velocity_x = attitude->roll_rate_rad_s;
    status.angular_velocity_y = attitude->pitch_rate_rad_s;
    status.angular_velocity_z = yaw_rate;
    status.flags = flags;
    status.target_system = server->autopilot_system_id;
    status.target_component = server->autopilot_component_id;
    status.delta_yaw = NAN;
    status.delta_yaw_velocity = NAN;
    (void)mavlink_msg_gimbal_device_attitude_status_encode_status(
        server->system_id, server->gimbal_component_id,
        &server->encode_status, message, &status);
}

static void send_gimbal_status(struct ca_mavlink_server *server,
                               const struct route *route)
{
    struct ca_gimbal_attitude attitude;
    mavlink_message_t message;
    if (!ca_backend_gimbal_attitude(server->backend, &attitude)) return;
    pack_gimbal_status(server, &message, &attitude);
    (void)send_message(server, route, &message);
}

static bool target_matches(const struct ca_mavlink_server *server, uint8_t target_system, uint8_t target_component,
                           uint8_t component)
{
    return (target_system == 0U || target_system == server->system_id) &&
           (target_component == 0U || target_component == component);
}

static uint8_t capture_one(struct ca_mavlink_server *server, int32_t index)
{
    bool success = ca_media_capture_photo(server->media,
                                          server->photo_scope) == 0;
    if (success) server->image_count++;
    broadcast_image_captured(server, index, success);
    broadcast_capture_status(server);
    return success ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
}

static uint8_t set_zoom(struct ca_mavlink_server *server, float type,
                        float value)
{
    if (!APCAM_HAVE_ZOOM) return MAV_RESULT_UNSUPPORTED;
    if (!isfinite(type) || !isfinite(value)) return MAV_RESULT_DENIED;
    if (type < 0.0f || type > 2.0f || floorf(type) != type) return MAV_RESULT_UNSUPPORTED;
    if (type != 2.0f && (value < -1.0f || value > 1.0f)) return MAV_RESULT_DENIED;
    if (APCAM_ZOOM_NATIVE_RATE && type == 1.0f)
        return ca_backend_set_zoom_rate(server->backend, value) == 0 ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
    if (!APCAM_ZOOM_SELECTS_LENS && selected_zoom_max(server) <= 1 && value != 0)
        return MAV_RESULT_UNSUPPORTED;
    unsigned zoom_type = (unsigned)type;
    if (zoom_type == 2U) {
        if (!isfinite(value) || value < 0.0f || value > 100.0f) {
            return MAV_RESULT_DENIED;
        }
        server->zoom_rate = 0.0f;
        return set_selected_zoom(server, 1.0f + value * ((selected_zoom_max(server) - 1.0f) / 100.0f)) == 0
                   ? MAV_RESULT_ACCEPTED
                   : MAV_RESULT_FAILED;
    }
    if (zoom_type == 1U) {
        if (!isfinite(value) || value < -1.0f || value > 1.0f) {
            return MAV_RESULT_DENIED;
        }
        if (value != 0.0f) {
            float zoom = selected_zoom(server);
            enum ca_media_lens lens = ca_media_lens(server->media);
            if (!isfinite(zoom)) return MAV_RESULT_FAILED;
            if (server->zoom_rate == 0.0f || lens != server->zoom_lens ||
                zoom != server->zoom_observed) {
                server->zoom_target = server->zoom_observed = zoom;
                server->zoom_lens = lens;
                server->zoom_updated_ms = monotonic_ms();
            }
        }
        /* Repeated rate messages must not reset fractional progress. */
        server->zoom_rate = value;
        return MAV_RESULT_ACCEPTED;
    }
    if (zoom_type == 0U && isfinite(value)) {
        if (value == 0.0f) return MAV_RESULT_ACCEPTED;
        server->zoom_rate = 0.0f;
        float zoom = selected_zoom(server) + (value < 0.0f ? -0.1f : 0.1f);
        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > selected_zoom_max(server)) zoom = selected_zoom_max(server);
        return set_selected_zoom(server, zoom) == 0
                   ? MAV_RESULT_ACCEPTED
                   : MAV_RESULT_FAILED;
    }
    return MAV_RESULT_UNSUPPORTED;
}

static uint8_t set_focus(struct ca_mavlink_server *server, float type,
                         float value)
{
    if (!APCAM_HAVE_FOCUS) return MAV_RESULT_UNSUPPORTED;
    unsigned focus_type = (unsigned)type;
    if (focus_type == 4U || focus_type == 5U || focus_type == 6U) {
        if (ca_media_autofocus(server->media, 960U, 540U) < 0) {
            return MAV_RESULT_FAILED;
        }
        server->focus_percent = NAN;
        return MAV_RESULT_ACCEPTED;
    }
    if (focus_type == 1U) {
        int direction = value > 0.01f ? 1 : value < -0.01f ? -1 : 0;
        if (ca_media_manual_focus(server->media, direction) < 0) {
            return MAV_RESULT_FAILED;
        }
        if (direction != 0) server->focus_percent = NAN;
        return MAV_RESULT_ACCEPTED;
    }
    if (focus_type == 2U) {
        if (!isfinite(value) || value < 0.0f || value > 100.0f) {
            return MAV_RESULT_DENIED;
        }
        if (ca_media_set_focus_percent(server->media, value) < 0) {
            return MAV_RESULT_FAILED;
        }
        server->focus_percent = value;
        return MAV_RESULT_ACCEPTED;
    }
    return MAV_RESULT_UNSUPPORTED;
}

static void send_protocol_capabilities(struct ca_mavlink_server *server,
                                       const struct route *route)
{
    mavlink_message_t message;
    mavlink_autopilot_version_t version = {
        .capabilities = MAV_PROTOCOL_CAPABILITY_PARAM_ENCODE_C_CAST |
                        MAV_PROTOCOL_CAPABILITY_MAVLINK2 | MAV_PROTOCOL_CAPABILITY_FTP,
    };
    (void)mavlink_msg_autopilot_version_encode_status(server->system_id,
        server->camera_component_id, &server->encode_status, &message, &version);
    (void)send_message(server, route, &message);
}

static void send_image_tracking(struct ca_mavlink_server *server, const struct route *route)
{
    const auto s=ca_media_tracking_status(server->media);
    const bool active=s.state==CA_TRACK_ACTIVE || s.state==CA_TRACK_COASTING;
    mavlink_camera_tracking_image_status_t status {};
    status.tracking_status=active?CAMERA_TRACKING_STATUS_FLAGS_ACTIVE:
        s.state==CA_TRACK_LOST?CAMERA_TRACKING_STATUS_FLAGS_ERROR:CAMERA_TRACKING_STATUS_FLAGS_IDLE;
    if(s.state==CA_TRACK_COASTING) status.tracking_status|=CAMERA_TRACKING_STATUS_FLAGS_COASTING;
    status.tracking_mode=active?CAMERA_TRACKING_MODE_RECTANGLE:CAMERA_TRACKING_MODE_NONE;
    status.target_data=CAMERA_TRACKING_TARGET_DATA_IN_STATUS;
    status.point_x=status.point_y=status.radius=NAN;
    // A predicted rectangle may partly leave the image during coasting.
    status.rec_top_x=active?std::clamp(s.rect.left,0.f,1.f):NAN;
    status.rec_top_y=active?std::clamp(s.rect.top,0.f,1.f):NAN;
    status.rec_bottom_x=active?std::clamp(s.rect.right,0.f,1.f):NAN;
    status.rec_bottom_y=active?std::clamp(s.rect.bottom,0.f,1.f):NAN;
    mavlink_message_t message;
    mavlink_msg_camera_tracking_image_status_encode_status(server->system_id,server->camera_component_id,
        &server->encode_status,&message,&status);
    if(route) (void)send_message(server,route,&message); else broadcast_message(server,&message);
}

static uint8_t handle_camera_command(struct ca_mavlink_server *server,
                                     const struct route *route,
                                     const mavlink_message_t *message,
                                     uint16_t command, const float params[7])
{
    switch (command) {
    case MAV_CMD_CAMERA_TRACK_RECTANGLE: {
        if(ca_mavlink_server_survey_owns_gimbal(server)) return MAV_RESULT_TEMPORARILY_REJECTED;
        if(!ca_media_tracking_available(server->media)) return MAV_RESULT_UNSUPPORTED;
        if(server->manual_control && *server->manual_control) return MAV_RESULT_TEMPORARILY_REJECTED;
        const ca_tracking_rect rect={params[0],params[1],params[2],params[3]};
        if(ca_media_tracking_start(server->media,rect,CA_TRACK_OWNER_MAVLINK)<0) return MAV_RESULT_DENIED;
        server->target_location_active=false; stop_tracking_rate(server);
        return MAV_RESULT_ACCEPTED;
    }
    case MAV_CMD_CAMERA_TRACK_POINT: {
        if(ca_mavlink_server_survey_owns_gimbal(server)) return MAV_RESULT_TEMPORARILY_REJECTED;
        if(!ca_media_tracking_available(server->media)) return MAV_RESULT_UNSUPPORTED;
        if(server->manual_control && *server->manual_control) return MAV_RESULT_TEMPORARILY_REJECTED;
        const float x=params[0], y=params[1];
        if(!isfinite(x) || !isfinite(y) || x<0 || x>1 || y<0 || y>1) return MAV_RESULT_DENIED;
        unsigned width=0,height=0; enum ca_video_codec codec; uint16_t flags=0;
        stream_properties(server,1U,&width,&height,&codec,&flags);
        const ca_tracking_rect rect=ca_tracking_point_rect(x,y,params[2],
            width && height ? float(width)/height : 16.f/9);
        if(ca_media_tracking_start(server->media,rect,CA_TRACK_OWNER_MAVLINK)<0) return MAV_RESULT_DENIED;
        server->target_location_active=false; stop_tracking_rate(server);
        return MAV_RESULT_ACCEPTED;
    }
    case MAV_CMD_CAMERA_STOP_TRACKING:
        ca_media_tracking_stop(server->media);
        send_image_tracking(server,route);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_REQUEST_MESSAGE: {
        if (!isfinite(params[0]) || params[0] < 0 || params[0] > 16777215 ||
            floorf(params[0]) != params[0]) return MAV_RESULT_DENIED;
        uint32_t requested = (uint32_t)params[0];
#if APCAM_HAVE_THERMAL
        if (requested == MAVLINK_MSG_ID_CAMERA_THERMAL_RANGE) {
            if (!thermal_selection_valid(params[1], params[2])) return MAV_RESULT_DENIED;
            return send_thermal_range(server, route, (unsigned)params[1]);
        }
#endif
        unsigned instance = isfinite(params[1]) ? (unsigned)params[1] : 0U;
        if (requested == MAVLINK_MSG_ID_AUTOPILOT_VERSION) send_protocol_capabilities(server, route);
        else if(requested==MAVLINK_MSG_ID_CAMERA_TRACKING_IMAGE_STATUS) send_image_tracking(server,route);
        else if (requested == MAVLINK_MSG_ID_CAMERA_INFORMATION) send_camera_information(server, route);
        else if (requested == MAVLINK_MSG_ID_CAMERA_FOV_STATUS) send_camera_fov(server, route);
        else if (requested == MAVLINK_MSG_ID_CAMERA_SETTINGS) send_camera_settings(server, route);
        else if (requested == MAVLINK_MSG_ID_STORAGE_INFORMATION) send_storage_information(server, route);
        else if (requested == MAVLINK_MSG_ID_CAMERA_CAPTURE_STATUS) send_capture_status(server, route);
        else if (requested == MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION ||
                 requested == MAVLINK_MSG_ID_VIDEO_STREAM_STATUS) {
            if (!send_stream_selection(server, route, requested, instance)) {
                return MAV_RESULT_UNSUPPORTED;
            }
        }
        else return MAV_RESULT_UNSUPPORTED;
        return MAV_RESULT_ACCEPTED;
    }
    case MAV_CMD_SET_MESSAGE_INTERVAL:
        if(params[0]==MAVLINK_MSG_ID_CAMERA_TRACKING_IMAGE_STATUS) {
            if(!isfinite(params[1]) || (params[1]<0 && params[1]!=-1) || params[1]>60000000) return MAV_RESULT_DENIED;
            server->image_tracking_interval_ms=params[1]==-1?UINT32_MAX:params[1]==0?0:
                std::max(50U,unsigned(params[1]/1000));
            return MAV_RESULT_ACCEPTED;
        }
#if APCAM_HAVE_THERMAL
        if (params[0] != MAVLINK_MSG_ID_CAMERA_THERMAL_RANGE) return MAV_RESULT_UNSUPPORTED;
        return set_thermal_interval(server, params);
#else
        return MAV_RESULT_UNSUPPORTED;
#endif
    case MAV_CMD_REQUEST_AUTOPILOT_CAPABILITIES:
        send_protocol_capabilities(server, route);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_REQUEST_CAMERA_INFORMATION:
        send_camera_information(server, route);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_REQUEST_CAMERA_SETTINGS:
        send_camera_settings(server, route);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_REQUEST_STORAGE_INFORMATION:
        send_storage_information(server, route);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_REQUEST_CAMERA_CAPTURE_STATUS:
        send_capture_status(server, route);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION:
        return send_stream_selection(server, route,
                                     MAVLINK_MSG_ID_VIDEO_STREAM_INFORMATION,
                                     (unsigned)params[0])
                   ? MAV_RESULT_ACCEPTED : MAV_RESULT_DENIED;
    case MAV_CMD_REQUEST_VIDEO_STREAM_STATUS:
        return send_stream_selection(server, route, MAVLINK_MSG_ID_VIDEO_STREAM_STATUS,
                                     (unsigned)params[0])
                   ? MAV_RESULT_ACCEPTED : MAV_RESULT_DENIED;
    case MAV_CMD_SET_CAMERA_MODE:
        if (!isfinite(params[1]) || params[1]<0 || params[1]>2 || floorf(params[1])!=params[1]) return MAV_RESULT_UNSUPPORTED;
        return set_camera_mode(server,(unsigned)params[1]);
    case MAV_CMD_SET_CAMERA_ZOOM: {
        uint8_t result = set_zoom(server, params[0], params[1]);
        if (result == MAV_RESULT_ACCEPTED) send_camera_settings(server, route);
        return result;
    }
    case MAV_CMD_SET_CAMERA_FOCUS:
        return set_focus(server, params[0], params[1]);
    case MAV_CMD_SET_CAMERA_SOURCE:
        if ((unsigned)params[1] == 2U) {
            return ca_media_set_thermal_main(server->media, true) == 0
                       ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
        }
        if ((unsigned)params[1] == 0U || (unsigned)params[1] == 1U) {
            return ca_media_set_thermal_main(server->media, false) == 0
                       ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
        }
        return MAV_RESULT_UNSUPPORTED;
    case MAV_CMD_IMAGE_START_CAPTURE: {
        int32_t count = isfinite(params[2]) ? (int32_t)params[2] : 1;
        int32_t index = isfinite(params[3]) && params[3] > 0.0f
                            ? (int32_t)params[3]
                            : server->next_image_index;
        if (count < 0 || !isfinite(params[1]) || params[1] < 0.0f) {
            return MAV_RESULT_DENIED;
        }
        if(server->camera_mode==CAMERA_MODE_IMAGE_SURVEY) {
            return survey_capture_start(server,count,params[1]);
        }
        server->next_image_index = index + 1;
        if (count == 1) {
            server->captures_remaining = 0;
            server->capture_interval_s = 0.0f;
            return capture_one(server, index);
        }
        /* MAVLink defines count zero as capture-until-stopped.  Bound a zero
         * interval so a malformed request cannot fill the microSD at loop
         * rate; the media pipeline cannot usefully exceed 10 stills/s. */
        server->capture_interval_s = params[1] < 0.1f ? 0.1f : params[1];
        server->captures_remaining = count == 0 ? -1 : count - 1;
        server->next_capture_ms = monotonic_ms() +
                                  (uint64_t)(server->capture_interval_s * 1000.0f);
        return capture_one(server, index);
    }
    case MAV_CMD_IMAGE_STOP_CAPTURE:
        survey_pause(server,"survey paused: capture stopped");
        server->captures_remaining = 0;
        server->capture_interval_s = 0.0f;
        broadcast_capture_status(server);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_VIDEO_START_CAPTURE:
        if (ca_media_set_recording(server->media, true) < 0) return MAV_RESULT_FAILED;
        if (server->recording_started_ms == 0U) server->recording_started_ms = monotonic_ms();
        broadcast_capture_status(server);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_VIDEO_STOP_CAPTURE:
        if (ca_media_set_recording(server->media, false) < 0) return MAV_RESULT_FAILED;
        server->recording_started_ms = 0U;
        broadcast_capture_status(server);
        return MAV_RESULT_ACCEPTED;
    case MAV_CMD_VIDEO_START_STREAMING:
    case MAV_CMD_VIDEO_STOP_STREAMING: {
        if (!isfinite(params[0]) || params[0] < 0 || params[0] > stream_count(server,route) || floorf(params[0]) != params[0]) return MAV_RESULT_DENIED;
        unsigned stream = (unsigned)params[0];
        bool enabled = command == MAV_CMD_VIDEO_START_STREAMING;
        if (stream == 0U) {
            ca_thermal_stream_enable(enabled);
            for (unsigned i = 0; i < APCAM_NUM_STREAMS; i++) {
                server->stream_enabled[i] = enabled;
                server->misb_stream_enabled[i] = enabled;
            }
        }
        // The reserved raw ID remains discoverable while its source is stale.
        else if (raw_thermal_stream(stream)) {
            if (enabled && !ca_thermal_stream_available()) return MAV_RESULT_DENIED;
            ca_thermal_stream_enable(enabled);
        }
        else if (unsigned source=stream_source(server,stream)) {
            if(stream_uses_misb(server,stream,route)) server->misb_stream_enabled[source-1] = enabled;
            else server->stream_enabled[source-1] = enabled;
        }
        else return MAV_RESULT_DENIED;
        send_stream_status(server, route, stream == 0U ? 1U : stream);
        return MAV_RESULT_ACCEPTED;
    }
    default:
        (void)message;
        return MAV_RESULT_UNSUPPORTED;
    }
}

static uint8_t handle_command_long(struct ca_mavlink_server *server,
                                const struct route *route,
                                const mavlink_message_t *message)
{
    /* MAVLink 2 may trim confirmation and both broadcast target bytes. */
    if (message->len < 30U) return 255;
    mavlink_command_long_t request;
    mavlink_msg_command_long_decode(message, &request);
    float params[7] = {request.param1, request.param2, request.param3,
                       request.param4, request.param5, request.param6,
                       request.param7};
    uint16_t command = request.command;
    uint8_t target_system = request.target_system;
    uint8_t target_component = request.target_component;
    if (route->kind == ROUTE_MCU &&
        (target_component == server->gimbal_component_id ||
         (command == MAV_CMD_REQUEST_MESSAGE &&
          (params[0] == MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION ||
           params[0] == MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS)))) return 255;
#if APCAM_GIMBAL_OPTIONAL
    if (!have_gimbal(server) && target_component == server->gimbal_component_id) return 255;
    if (have_gimbal(server) && target_matches(server, target_system, target_component,
                       server->gimbal_component_id)) {
#else
    if (target_matches(server, target_system, target_component,
                       server->gimbal_component_id)) {
#endif
        if (command == MAV_CMD_REQUEST_MESSAGE &&
            ((uint32_t)params[0] == MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION ||
             (uint32_t)params[0] == MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS)) {
            if ((uint32_t)params[0] == MAVLINK_MSG_ID_GIMBAL_DEVICE_INFORMATION) {
                send_gimbal_information(server, route);
            } else {
                send_gimbal_status(server, route);
            }
            return send_ack(server, route, server->gimbal_component_id, command,
                     MAV_RESULT_ACCEPTED, message);
        }
        if (target_component == server->gimbal_component_id) {
            return send_ack(server, route, server->gimbal_component_id, command,
                     MAV_RESULT_UNSUPPORTED, message);
        }
    }
    if (!target_matches(server, target_system, target_component,
                        server->camera_component_id)) return 255;
    uint8_t result = handle_camera_command(server, route, message, command, params);
    return send_ack(server, route, server->camera_component_id, command, result, message);
}

static void quaternion_to_euler(const float q[4], float *roll, float *pitch,
                                float *yaw)
{
    float sin_pitch = 2.0f * (q[0] * q[2] - q[3] * q[1]);
    if (sin_pitch > 1.0f) sin_pitch = 1.0f;
    if (sin_pitch < -1.0f) sin_pitch = -1.0f;
    *roll = atan2f(2.0f * (q[0] * q[1] + q[2] * q[3]),
                   1.0f - 2.0f * (q[1] * q[1] + q[2] * q[2]));
    *pitch = asinf(sin_pitch);
    *yaw = atan2f(2.0f * (q[0] * q[3] + q[1] * q[2]),
                  1.0f - 2.0f * (q[2] * q[2] + q[3] * q[3]));
}

static void save_vehicle_attitude(struct ca_mavlink_server *server,
                                  float roll, float pitch, float yaw, float yaw_rate,
                                  uint64_t sample_ms, bool primary, bool reset)
{
    if (reset || primary != server->vehicle_attitude_primary) {
        server->vehicle_yaw_history = {};
    }
    server->vehicle_attitude_primary = primary;
    server->vehicle_attitude_updated_ms = sample_ms;
    server->have_vehicle_attitude = true;
    if (primary) server->primary_attitude_ms = sample_ms;
    server->vehicle_yaw_history.add(sample_ms, yaw, yaw_rate);
    ca_metadata_set_vehicle_attitude_motion(roll, pitch, yaw, yaw_rate, sample_ms);
}

static void handle_autopilot_state_for_gimbal(
    struct ca_mavlink_server *server,
    const mavlink_message_t *message)
{
    /* MAVLink 2 can trim zero target-component and extension bytes. Decode
     * zero-fills them; validate the quaternion instead of requiring MIN_LEN. */
    mavlink_autopilot_state_for_gimbal_device_t state;
    if (message->sysid != server->autopilot_system_id ||
        message->compid != server->autopilot_component_id) {
        return;
    }
    mavlink_msg_autopilot_state_for_gimbal_device_decode(message, &state);
    if (!target_matches(server, state.target_system, state.target_component,
                        server->gimbal_component_id)) {
        return;
    }
    float q[4];
    float norm = 0.0f;
    for (unsigned i = 0; i < 4U; i++) {
        q[i] = state.q[i];
        if (!isfinite(q[i])) return;
        norm += q[i] * q[i];
    }
    if (!isfinite(norm) || norm < 0.000001f) return;
    norm = sqrtf(norm);
    for (unsigned i = 0; i < 4U; i++) q[i] /= norm;
    float roll;
    float pitch;
    float yaw;
    quaternion_to_euler(q, &roll, &pitch, &yaw);
    float yaw_rate = message->len >= 57U ? state.angular_velocity_z : NAN;
    if (!isfinite(yaw_rate)) yaw_rate = state.feed_forward_angular_velocity_z;
    if (!isfinite(yaw_rate)) yaw_rate = 0.0f;
    uint64_t sample_ms;
    bool reset;
    if (!server->attitude_clock.sample(state.time_boot_us, 1, monotonic_ms() * 1000,
                                       sample_ms, reset)) return;
    save_vehicle_attitude(server, roll, pitch, yaw, yaw_rate, sample_ms, true, reset);
    ca_metadata_set_velocity(state.vx, state.vy, state.vz, sample_ms);
    CA_BINLOG(CA_LOG_ATT, ca_log_att, .boot_ms=(uint32_t)(state.time_boot_us/1000),
        .source=1, .roll=roll*57.295779513f,.pitch=pitch*57.295779513f,
        .yaw=yaw*57.295779513f,.rollrate=NAN,.pitchrate=NAN,.yawrate=yaw_rate*57.295779513f,
        .source_system=message->sysid,.source_component=message->compid);
}

static void handle_attitude(struct ca_mavlink_server *server,
                            const mavlink_message_t *message)
{
    /* Never overwrite a fresh gimbal-state quaternion with the independently
     * scheduled ATTITUDE stream. Map its clock even while it is a standby. */
    mavlink_attitude_t attitude;
    if (message->sysid != server->autopilot_system_id ||
        message->compid != server->autopilot_component_id) {
        return;
    }
    mavlink_msg_attitude_decode(message, &attitude);
    if (!isfinite(attitude.roll) || !isfinite(attitude.pitch) ||
        !isfinite(attitude.yaw)) {
        return;
    }
    CA_BINLOG(CA_LOG_ATT, ca_log_att, .boot_ms=attitude.time_boot_ms, .source=2,
        .roll=attitude.roll*57.295779513f,.pitch=attitude.pitch*57.295779513f,
        .yaw=attitude.yaw*57.295779513f,.rollrate=attitude.rollspeed*57.295779513f,
        .pitchrate=attitude.pitchspeed*57.295779513f,.yawrate=attitude.yawspeed*57.295779513f,
        .source_system=message->sysid,.source_component=message->compid);
    uint64_t sample_ms;
    bool reset;
    const uint64_t now = monotonic_ms();
    if (!server->fallback_clock.sample(attitude.time_boot_ms, 1000, now * 1000,
                                       sample_ms, reset)) return;
    if (server->primary_attitude_ms &&
        now - server->primary_attitude_ms <= VEHICLE_PREDICTION_MS) return;
    /* ATTITUDE rates are body rates; convert to Euler yaw rate. */
    float cp = cosf(attitude.pitch);
    float yaw_rate = fabsf(cp) > 0.01f ?
        (attitude.pitchspeed * sinf(attitude.roll) +
         attitude.yawspeed * cosf(attitude.roll)) / cp : NAN;
    if (!isfinite(yaw_rate)) {
        // Vertical pitch makes Euler yaw rate undefined, not the measured
        // attitude. Preserve metadata without feeding an invalid rate to ROI.
        ca_metadata_set_vehicle_attitude_motion(attitude.roll, attitude.pitch,
                                                attitude.yaw, NAN, sample_ms);
        return;
    }
    save_vehicle_attitude(server, attitude.roll, attitude.pitch, attitude.yaw,
                          yaw_rate, sample_ms, false, reset);
}

static void handle_system_time(struct ca_mavlink_server *server,
                               const mavlink_message_t *message)
{
    /* Adopt the autopilot's UTC date if ours predates 1 September 2026 UTC. */
#ifdef CAMERA_APP_ZT30_RAM
    (void)server;(void)message;return;
#else
    const time_t earliest_valid_time = 1788220800;
    mavlink_system_time_t time;
    uint64_t unix_us;
    struct timespec now, wanted;
    if (message->sysid != server->autopilot_system_id ||
        message->compid != server->autopilot_component_id) {
        return;
    }
    mavlink_msg_system_time_decode(message, &time);
    unix_us = time.time_unix_usec;
    if (unix_us < (uint64_t)earliest_valid_time * UINT64_C(1000000)) return;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 ||
        now.tv_sec >= earliest_valid_time) {
        return;
    }
    wanted.tv_sec = (time_t)(unix_us / UINT64_C(1000000));
    wanted.tv_nsec = (long)(unix_us % UINT64_C(1000000)) * 1000L;
    if (clock_settime(CLOCK_REALTIME, &wanted) == 0) {
        ca_log("system time set from MAVLink SYSTEM_TIME epoch_us=%llu",
               (unsigned long long)unix_us);
    } else {
        ca_log("system time setting from MAVLink failed: %s", strerror(errno));
    }
#endif
}

static void handle_global_position_int(
    struct ca_mavlink_server *server,
    const mavlink_message_t *message)
{
    /* MAVLink 2 may trim any trailing zero altitude bytes; decode zero-fills
     * the missing fields. */
    mavlink_global_position_int_t position;
    if (message->sysid != server->autopilot_system_id ||
        message->compid != server->autopilot_component_id) {
        return;
    }
    mavlink_msg_global_position_int_decode(message, &position);
    int32_t lat_e7 = position.lat;
    int32_t lon_e7 = position.lon;
    int32_t alt_mm = position.alt;
    int32_t relative_alt_mm = position.relative_alt;
    uint16_t heading_cdeg = position.hdg;
    if (lat_e7 < -900000000 || lat_e7 > 900000000 ||
        lon_e7 < -1800000000 || lon_e7 > 1800000000) {
        return;
    }
    uint64_t sample_ms;
    bool reset;
    if (!server->position_clock.sample(position.time_boot_ms, 1000, monotonic_ms() * 1000,
                                       sample_ms, reset)) return;
    ca_metadata_set_position(lat_e7, lon_e7, alt_mm * 0.001f,
                             relative_alt_mm * 0.001f,
                             heading_cdeg == 0xffffU
                                 ? NAN
                                 : heading_cdeg * 0.01f * (float)(M_PI / 180.0), sample_ms);
    ca_metadata_set_velocity(position.vx * 0.01f, position.vy * 0.01f,
                              position.vz * 0.01f, sample_ms);
    CA_BINLOG(CA_LOG_POS, ca_log_pos, .boot_ms=position.time_boot_ms,
        .lat=lat_e7,.lon=lon_e7,.alt=alt_mm*.001f,.relalt=relative_alt_mm*.001f,
        .vn=position.vx*.01f,.ve=position.vy*.01f,.vd=position.vz*.01f,
        .source_system=message->sysid,.source_component=message->compid);
    bool first_position = !server->have_vehicle_position;
    server->vehicle_lat_e7 = lat_e7;
    server->vehicle_lon_e7 = lon_e7;
    server->vehicle_alt_amsl_m = alt_mm * 0.001f;
    server->vehicle_vn_m_s = position.vx * 0.01f;
    server->vehicle_ve_m_s = position.vy * 0.01f;
    server->vehicle_vd_m_s = position.vz * 0.01f;
    server->vehicle_position_updated_ms = sample_ms;
    server->have_vehicle_position = true;
    if (first_position) {
        ca_log("MAVLink vehicle position available");
    }
}

void ca_mavlink_server_suspend_gimbal(struct ca_mavlink_server *server)
{
    /* Preserve the ROI for release, but discard its controller history. */
    if (!server || ca_mavlink_server_survey_owns_gimbal(server)) return;
    survey_pause(server,"survey paused: manual gimbal control");
    ca_media_tracking_stop(server->media);
    stop_tracking_rate(server);
    server->last_target_location_ms=0;
    server->yaw_lock=false;
}

static bool clear_target_location(struct ca_mavlink_server *server)
{
    bool was_active = server->target_location_active;
    server->target_location_active = false;
    stop_tracking_rate(server);
    return was_active;
}

static uint8_t handle_command_int(struct ca_mavlink_server *server,
                               const struct route *route,
                               const mavlink_message_t *message)
{
    /* MAVLink 2 trims all trailing zero fields, including broadcast targets
     * and the global frame.  Only the command field itself must be present. */
    mavlink_command_int_t request;
    if (message->len < 29U) return 255;
    mavlink_msg_command_int_decode(message, &request);
#if APCAM_GIMBAL_OPTIONAL
    if (!have_gimbal(server)) return 255;
#endif
    if (!target_matches(server, request.target_system, request.target_component,
                        server->gimbal_component_id)) {
        return 255;
    }
    uint16_t command = request.command;
    uint8_t result = MAV_RESULT_UNSUPPORTED;
    if (ca_mavlink_server_survey_owns_gimbal(server) &&
        (command == MAV_CMD_DO_SET_ROI_LOCATION || command == MAV_CMD_DO_SET_ROI_NONE)) {
        return send_ack(server,route,server->gimbal_component_id,command,MAV_RESULT_TEMPORARILY_REJECTED,message);
    }
    if (server->manual_control && *server->manual_control &&
        (command==MAV_CMD_DO_SET_ROI_LOCATION || command==MAV_CMD_DO_SET_ROI_NONE)) {
        return send_ack(server,route,server->gimbal_component_id,command,MAV_RESULT_TEMPORARILY_REJECTED,message);
    }
    if (command == MAV_CMD_DO_SET_ROI_LOCATION) {
        survey_pause(server,"survey paused: external ROI");
        uint8_t frame = request.frame;
        int32_t lat_e7 = request.x;
        int32_t lon_e7 = request.y;
        float alt_amsl_m = request.z;
        if (!server->settings.position_targeting) {
            result = MAV_RESULT_UNSUPPORTED;
        } else if (frame != MAV_FRAME_GLOBAL && frame != MAV_FRAME_GLOBAL_INT) {
            result = MAV_RESULT_DENIED;
        } else if (lat_e7 < -900000000 || lat_e7 > 900000000 ||
                   lon_e7 < -1800000000 || lon_e7 > 1800000000 ||
                   !isfinite(alt_amsl_m)) {
            result = MAV_RESULT_DENIED;
        } else {
            bool changed = !server->target_location_active ||
                server->target_lat_e7 != lat_e7 ||
                server->target_lon_e7 != lon_e7 ||
                server->target_alt_amsl_m != alt_amsl_m;
            server->target_lat_e7 = lat_e7;
            server->target_lon_e7 = lon_e7;
            server->target_alt_amsl_m = alt_amsl_m;
            server->target_location_active = true;
            ca_media_tracking_stop(server->media);
            server->yaw_lock = true;
            result = MAV_RESULT_ACCEPTED;
            if (changed) {
                server->last_target_location_ms = 0U;
                ca_log("MAVLink location target lat=%.7f lon=%.7f alt_amsl=%.2f",
                       lat_e7 * 1.0e-7, lon_e7 * 1.0e-7, alt_amsl_m);
            }
        }
    } else if (command == MAV_CMD_DO_SET_ROI_NONE) {
        bool was_active = clear_target_location(server);
        result = MAV_RESULT_ACCEPTED;
        if (was_active) ca_log("MAVLink location target cleared");
    }
    return send_ack(server, route, server->gimbal_component_id, command, result, message);
}

static uint8_t handle_gimbal_set_attitude(struct ca_mavlink_server *server,
                                       const mavlink_message_t *message)
{
    mavlink_gimbal_device_set_attitude_t request;
    if (message->len < MAVLINK_MSG_ID_GIMBAL_DEVICE_SET_ATTITUDE_MIN_LEN) return 255;
    mavlink_msg_gimbal_device_set_attitude_decode(message, &request);
    uint16_t flags = request.flags;
#if APCAM_GIMBAL_OPTIONAL
    if (!have_gimbal(server)) return 255;
#endif
    if (!target_matches(server, request.target_system, request.target_component,
                        server->gimbal_component_id)) return 255;
    if (ca_mavlink_server_survey_owns_gimbal(server)) return MAV_RESULT_TEMPORARILY_REJECTED;
    if (server->manual_control && *server->manual_control) return MAV_RESULT_TEMPORARILY_REJECTED;
    ca_media_tracking_stop(server->media);
    clear_target_location(server);
    if ((flags & (GIMBAL_DEVICE_FLAGS_RETRACT | GIMBAL_DEVICE_FLAGS_NEUTRAL)) !=
        0U) {
        server->yaw_lock = false;
        return ca_backend_set_gimbal_neutral(server->backend) == 0 ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
    }
    bool earth_yaw;
    if ((flags & GIMBAL_DEVICE_FLAGS_YAW_IN_EARTH_FRAME) != 0U) {
        earth_yaw = true;
    } else if ((flags & GIMBAL_DEVICE_FLAGS_YAW_IN_VEHICLE_FRAME) != 0U) {
        earth_yaw = false;
    } else {
        earth_yaw = (flags & GIMBAL_DEVICE_FLAGS_YAW_LOCK) != 0U;
    }
    float vehicle_yaw = 0.0f;
    float vehicle_yaw_rate = 0.0f;
    if (earth_yaw && !current_vehicle_attitude(
            server, &vehicle_yaw, &vehicle_yaw_rate)) {
        return MAV_RESULT_TEMPORARILY_REJECTED;
    }
    server->yaw_lock = earth_yaw;
    float pitch_rate = request.angular_velocity_y;
    float yaw_rate = request.angular_velocity_z;
    if (isfinite(pitch_rate) || isfinite(yaw_rate)) {
        if (!isfinite(pitch_rate)) pitch_rate = 0.0f;
        if (!isfinite(yaw_rate)) yaw_rate = 0.0f;
        if (earth_yaw) yaw_rate -= vehicle_yaw_rate;
        return ca_backend_set_gimbal_rates(server->backend, pitch_rate, yaw_rate) == 0 ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
    }
    float q[4];
    for (unsigned i = 0; i < 4U; i++) q[i] = request.q[i];
    if (isfinite(q[0])) {
        float roll, pitch, yaw;
        quaternion_to_euler(q, &roll, &pitch, &yaw);
        (void)roll;
        if (earth_yaw) yaw = wrap_pi_f(yaw - vehicle_yaw);
        return ca_backend_set_gimbal_angles(server->backend, pitch, yaw) == 0 ? MAV_RESULT_ACCEPTED : MAV_RESULT_FAILED;
    }
    return MAV_RESULT_DENIED;
}

/* The camera component owns the shared camera/gimbal configuration. Values
 * are C-cast INT32 (all ranges fit exactly in a float), as used by ArduPilot.
 * Keep running and saved settings separate for restart-only configuration. */
static void refresh_parameters(struct ca_mavlink_server *server)
{
    struct ca_config parameters;
    char error[160];
    ca_config_defaults(&parameters);
    if (ca_config_load(&parameters, server->config_path, error, sizeof(error)) == 0) {
#ifdef CAMERA_APP_ZT30_DEV
        APC_ZT30_Config::effective(parameters);
#endif
        server->parameters = parameters;
    }
}

static void send_parameter(struct ca_mavlink_server *server, size_t index)
{
    mavlink_message_t message;
    mavlink_param_value_t value = {};
    value.param_value = (float)ca_config_param_get(&server->parameters, index);
    value.param_count = (uint16_t)ca_config_param_count();
    value.param_index = (uint16_t)index;
    value.param_type = MAV_PARAM_TYPE_INT32;
    const char *name = ca_config_param_name(index);
    memcpy(value.param_id, name, strlen(name));
    (void)mavlink_msg_param_value_encode_status(server->system_id,
        server->camera_component_id, &server->encode_status, &message, &value);
    broadcast_message(server, &message);
}

static void parameter_status(struct ca_mavlink_server *server, const char *text)
{
    ca_binlog_message(text);
    mavlink_message_t message;
    mavlink_statustext_t status = {.severity = MAV_SEVERITY_INFO};
    snprintf(status.text, sizeof(status.text), "%s", text);
    (void)mavlink_msg_statustext_encode_status(server->system_id,
        server->camera_component_id, &server->encode_status, &message, &status);
    broadcast_message(server, &message);
}

/* Share runtime application between PARAM_SET, PARAM_EXT_SET and INI reload. */
static bool live_config_parameter(size_t index)
{
#ifdef CAMERA_APP_ZT30_DEV
    const char *name=ca_config_param_name(index);
    return name && (!strncmp(name,"IMG_",4) || !strcmp(name,"PHOTO_SCOPE") ||
                    !strcmp(name,"RAW_STREAM_FPS") || !strcmp(name,"RAW_RECORD_FPS") ||
                    !strcmp(name,"LOG_DISARMED") || !strcmp(name,"REC_AUTOSTART") ||
                    !strncmp(name,"SURV_",5) || !strcmp(name,"OSD_CROSS") || !strcmp(name,"OSD_RECORD") ||
                    !strcmp(name,"MAV_POS_TARGET") || !strcmp(name,"TRACK_METHOD") || !strcmp(name,"THERMAL_PALETTE") || !strcmp(name,"THERMAL_HFOV_CD") || !strcmp(name,"OSD_THERMAL_FOV"));
#else
    const char *name = ca_config_param_name(index);
    if (strcmp(name, "VIDEO_META_FMT") == 0) return false;
    if (strcmp(name, "PHOTO_SCOPE") == 0) return true;
    if (strcmp(name, "THERMAL_PALETTE") == 0) return APCAM_HAVE_THERMAL;
    int camera_index = ca_camera_param_find(name);
    struct ca_camera_parameter p;
    return camera_index >= 0 && ca_camera_param_info((size_t)camera_index, &p) &&
        p.operation == CA_CAMERA_CONFIG;
#endif
}

static int apply_runtime_config(struct ca_mavlink_server *server, const struct ca_config *next)
{
    struct ca_config previous = server->settings;
    bool was_recording = ca_media_recording(server->media);
    bool pipeline = next->main_resolution != previous.main_resolution ||
        next->sub_resolution != previous.sub_resolution ||
        next->recording_resolution != previous.recording_resolution ||
        next->main_codec != previous.main_codec || next->sub_codec != previous.sub_codec;
    /* A media reopen can take seconds. Do not leave a moving rate command
     * running while the event loop cannot service tracking or feedback. */
    if (pipeline && !was_recording) stop_tracking_rate(server);
    if (ca_media_configure(server->media, next) < 0) return -1;
    bool record = was_recording;
    if (next->autorecord != previous.autorecord) {
        record = next->autorecord == CA_AUTORECORD_ENABLED ||
            (next->autorecord == CA_AUTORECORD_WHILE_ARMED &&
             server->have_vehicle_armed && server->vehicle_armed);
    }
    if ((next->thermal_palette != previous.thermal_palette && APCAM_HAVE_THERMAL_PALETTE &&
         ca_media_set_thermal_palette(server->media, next->thermal_palette) < 0) ||
        (record != was_recording && ca_media_set_recording(server->media, record) < 0) ||
        (strcmp(next->timezone, previous.timezone) != 0 && APC_Timezone::apply(next->timezone) < 0)) {
        int saved_errno = errno;
        if (ca_media_recording(server->media) != was_recording)
            (void)ca_media_set_recording(server->media, was_recording);
        if (next->thermal_palette != previous.thermal_palette && APCAM_HAVE_THERMAL_PALETTE)
            (void)ca_media_set_thermal_palette(server->media, previous.thermal_palette);
        if (ca_media_configure(server->media, &previous) < 0)
            ca_log("configuration runtime rollback failed");
        errno = saved_errno;
        return -1;
    }
    if (next->tracking_method != previous.tracking_method || !next->position_targeting) {
        stop_tracking_rate(server);
        server->last_target_location_ms = 0;
    }
    if (!next->position_targeting) server->target_location_active = false;
    for (size_t i=0;i<ca_config_param_count();i++) {
        int value=ca_config_param_get(next,i);
        if (value!=ca_config_param_get(&previous,i))
            ca_binlog_parameter(ca_config_param_name(i),value,true);
    }
    if(memcmp(&next->survey,&previous.survey,sizeof(next->survey)) || !next->position_targeting || pipeline)
        survey_pause(server,"survey paused: configuration changed");
    server->settings = *next;
    server->photo_scope = next->photo_scope;
    if (next->position_targeting != previous.position_targeting) send_gimbal_information(server, NULL);
    return 0;
}

static bool video_format_parameter(size_t index)
{
    const char *name = ca_config_param_name(index);
    return strcmp(name, "REC_RESOLUTION") == 0 ||
        strcmp(name, "VIDEO_MAIN_RES") == 0 || strcmp(name, "VIDEO_SUB_RES") == 0 ||
        strcmp(name, "VIDEO_MAIN_CODEC") == 0 || strcmp(name, "VIDEO_SUB_CODEC") == 0;
}

static int apply_camera_config(struct ca_mavlink_server *server, size_t index, float value)
{
    struct ca_config previous = server->settings, next = previous;
    bool was_recording = ca_media_recording(server->media);
    bool was_tracking = server->target_location_active;
    if (ca_config_param_assign(&next, index, value) < 0) return -1;
    if (apply_runtime_config(server, &next) < 0) {
        if (errno == EBUSY && was_recording && video_format_parameter(index)) {
            /* Match web saves: leave the active recording intact and let
             * reload_config apply the saved format after recording stops. */
            if (ca_config_param_save(&server->parameters, server->config_path, index, value) < 0) {
                parameter_status(server, "Parameter could not be saved");
                return -1;
            }
            server->config_pending = true;
            ca_binlog_parameter(ca_config_param_name(index), value, false);
            parameter_status(server, "Format saved; applies when recording stops");
            return 0;
        }
        parameter_status(server, errno == EBUSY ?
            "Stop recording before changing video format" : "Camera could not apply parameter");
        return -1;
    }
    if (ca_config_param_save(&server->parameters, server->config_path, index, value) < 0) {
        int saved_errno = errno;
        if (apply_runtime_config(server, &previous) < 0 ||
            (ca_media_recording(server->media) != was_recording &&
             ca_media_set_recording(server->media, was_recording) < 0))
            ca_log("parameter persistence rollback failed");
        server->target_location_active = was_tracking;
        errno = saved_errno;
        parameter_status(server, "Parameter could not be saved");
        return -1;
    }
    ca_binlog_parameter(ca_config_param_name(index),value,false);
    parameter_status(server, "Parameter applied and saved");
    return 0;
}

static bool same_config_file(const struct stat *a, const struct stat *b)
{
    return a->st_ino == b->st_ino && a->st_dev == b->st_dev && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static void config_status(struct ca_mavlink_server *server, uint64_t hash,
                           const char *state, const char *message)
{
    char contents[512], temporary[sizeof(server->config_status_path) + 5];
    snprintf(contents, sizeof(contents), "%016llx %ld %s\n%s\n",
             (unsigned long long)hash, (long)getpid(), state, message);
    if (strcmp(contents, server->config_last_status) == 0) return;
    snprintf(temporary, sizeof(temporary), "%s.tmp", server->config_status_path);
    FILE *file = fopen(temporary, "w");
    if (!file) return;
    bool ok = fputs(contents, file) >= 0;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temporary, server->config_status_path) == 0) {
        snprintf(server->config_last_status, sizeof(server->config_last_status), "%s", contents);
        ca_log("configuration %s: %s", state, message);
    } else unlink(temporary);
}

static void reload_config(struct ca_mavlink_server *server, uint64_t now)
{
#ifdef CAMERA_APP_ZT30_RAM
    (void)server;(void)now;return; // bounded RAM configuration, no hot reload
#else
    if (now - server->config_check_ms < 500U) return;
    server->config_check_ms = now;
    struct stat before, after;
    if (stat(server->config_path, &before) < 0) return;
    if (server->config_seen && !server->config_pending && same_config_file(&before, &server->config_stat)) return;
    struct ca_config desired;
    ca_config_defaults(&desired);
    char error[256] = "";
    int loaded = ca_config_load(&desired, server->config_path, error, sizeof(error));
    uint64_t hash;
    if (apcam_config_file_hash(server->config_path, &hash) < 0 ||
        stat(server->config_path, &after) < 0 || !same_config_file(&before, &after)) return;
    server->config_seen = true;
    server->config_stat = after;
    server->config_pending = false;
    if (loaded < 0) {
        config_status(server, hash, "error", error[0] ? error : "Could not read configuration");
        return;
    }
#ifdef CAMERA_APP_ZT30_DEV
    APC_ZT30_Config::effective(desired);
    server->parameters=desired;
    struct ca_config next=server->settings;
    for(size_t i=0;i<ca_config_param_count();i++)if(live_config_parameter(i))
        (void)ca_config_param_assign(&next,i,float(ca_config_param_get(&desired,i)));
    memcpy(next.timezone,desired.timezone,sizeof(next.timezone));
    if(apply_runtime_config(server,&next)<0) {
        snprintf(error,sizeof(error),"Could not apply saved settings: %s",strerror(errno));
        config_status(server,hash,"error",error);return;
    }
    bool restart=false;
    for(size_t i=0;i<APC_Config::count();i++) {
        const auto &f=APC_Config::fields()[i];
        if(memcmp((const char*)&desired+f.offset,(const char*)&server->settings+f.offset,f.size))restart=true;
    }
    config_status(server,hash,restart?"restart":"applied",restart?
        "Live image settings applied. Restart to apply other saved settings.":
        "All saved settings applied without restarting the camera app.");
    return;
#else
    for (size_t i=0;i<ca_config_param_count();i++) {
        int value=ca_config_param_get(&desired,i);
        if (value!=ca_config_param_get(&server->parameters,i))
            ca_binlog_parameter(ca_config_param_name(i),value,false);
    }
    server->parameters = desired;
    /* Apply inexpensive settings even if a format change must wait for a
     * recording to finish. Keep transport/mount identity at its running value. */
    struct ca_config next = server->settings;
    for (size_t i = 0; i < ca_config_param_count(); i++) {
        if (live_config_parameter(i))
            (void)ca_config_param_assign(&next, i, (float)ca_config_param_get(&desired, i));
    }
    memcpy(next.timezone, desired.timezone, sizeof(next.timezone));
    next.network_capture = desired.network_capture;
    memcpy(next.video1_uri, desired.video1_uri, sizeof(next.video1_uri));
    next.main_resolution = server->settings.main_resolution;
    next.sub_resolution = server->settings.sub_resolution;
    next.recording_resolution = server->settings.recording_resolution;
    next.main_codec = server->settings.main_codec;
    next.sub_codec = server->settings.sub_codec;
    if (apply_runtime_config(server, &next) < 0) {
        snprintf(error, sizeof(error), "Could not apply saved live settings: %s", strerror(errno));
        config_status(server, hash, "error", error);
        return;
    }
    next.main_resolution = desired.main_resolution;
    next.sub_resolution = desired.sub_resolution;
    next.recording_resolution = desired.recording_resolution;
    next.main_codec = desired.main_codec;
    next.sub_codec = desired.sub_codec;
    if (apply_runtime_config(server, &next) < 0) {
        if (errno == EBUSY) {
            server->config_pending = true;
            config_status(server, hash, "pending", "Live settings applied; video format waits until recording stops. Restart-only changes still require restart.");
        } else {
            snprintf(error, sizeof(error), "Live settings applied; video format failed: %s", strerror(errno));
            config_status(server, hash, "error", error);
        }
        return;
    }
    if (server->network_error[0]) {
        config_status(server, hash, "error", server->network_error);
        return;
    }
    bool restart = memcmp(&desired.support, &server->settings.support, sizeof(desired.support)) != 0 ||
        memcmp(&desired.network, &server->settings.network, sizeof(desired.network)) != 0 ||
        strcmp(desired.main_alias, server->settings.main_alias) != 0 ||
        strcmp(desired.sub_alias, server->settings.sub_alias) != 0;
    for (size_t i = 0; i < ca_config_param_count(); i++) {
        if (!live_config_parameter(i) && ca_config_param_get(&desired, i) != ca_config_param_get(&server->settings, i)) restart = true;
    }
    config_status(server, hash, restart ? "restart" : "applied", restart ?
        "Live settings applied. Restart to apply mount, identity, transport, network, stream alias or SupportProxy changes." :
        "All saved settings applied without restarting the camera app.");
#endif
#endif
}

static uint8_t handle_parameter(struct ca_mavlink_server *server,
                              const mavlink_message_t *message)
{
    uint8_t system, component;
    char name[17] = {};
    int index = -1;
    mavlink_param_set_t set = {};
    if (message->msgid == MAVLINK_MSG_ID_PARAM_REQUEST_LIST) {
        mavlink_param_request_list_t request;
        mavlink_msg_param_request_list_decode(message, &request);
        system = request.target_system;
        component = request.target_component;
    } else if (message->msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ) {
        mavlink_param_request_read_t request;
        mavlink_msg_param_request_read_decode(message, &request);
        system = request.target_system;
        component = request.target_component;
        memcpy(name, request.param_id, sizeof(request.param_id));
        index = request.param_index == -1 ? ca_config_param_find(name)
                                          : request.param_index;
    } else {
        mavlink_msg_param_set_decode(message, &set);
        system = set.target_system;
        component = set.target_component;
        memcpy(name, set.param_id, sizeof(set.param_id));
        index = ca_config_param_find(name);
    }
    if (!target_matches(server, system, component, server->camera_component_id)) return 255;
    refresh_parameters(server);
    if (message->msgid == MAVLINK_MSG_ID_PARAM_REQUEST_LIST) {
        /* Pace the list in periodic() so even a UART can drain between values. */
        server->next_parameter = 0U;
        server->parameter_list_active = true;
        return 255;
    }
    if (index < 0 || (size_t)index >= ca_config_param_count()) return PARAM_ACK_VALUE_UNSUPPORTED;
    uint8_t result = PARAM_ACK_ACCEPTED;
    if (message->msgid == MAVLINK_MSG_ID_PARAM_SET) {
        /* MAVProxy may send REAL32 even for an integer parameter. Validate
         * the numeric value without truncating fractional or non-finite input. */
        if ((set.param_type != MAV_PARAM_TYPE_INT32 &&
             set.param_type != MAV_PARAM_TYPE_REAL32) ||
            (live_config_parameter((size_t)index) ?
                apply_camera_config(server, (size_t)index, set.param_value) :
                ca_config_param_save(&server->parameters, server->config_path,
                                     (size_t)index, set.param_value)) < 0) {
            parameter_status(server, "Parameter write rejected; value unchanged");
            result = PARAM_ACK_FAILED;
        } else if (!live_config_parameter((size_t)index)) {
            ca_binlog_parameter(ca_config_param_name((size_t)index),set.param_value,false);
            parameter_status(server, "Parameter saved; restart camera-app to apply");
        }
    }
    send_parameter(server, (size_t)index);
    return result;
}

/* Camera definition settings use binary little-endian PARAM_EXT values,
 * independently of the original C-cast PARAM_* configuration service. */
static void ext_encode(char output[128], unsigned type, float value)
{
    memset(output, 0, 128);
    if (type == MAV_PARAM_EXT_TYPE_UINT8) {
        output[0] = (char)(uint8_t)value;
        return;
    }
    uint32_t bits;
    if (type == MAV_PARAM_EXT_TYPE_REAL32) memcpy(&bits, &value, sizeof(bits));
    else bits = (uint32_t)(int32_t)value;
    for (unsigned i = 0; i < 4; i++) output[i] = (char)(bits >> (8 * i));
}

static float ext_decode(const char input[128], unsigned type)
{
    if (type == MAV_PARAM_EXT_TYPE_UINT8) return (float)(uint8_t)input[0];
    uint32_t bits = 0;
    for (unsigned i = 0; i < 4; i++) bits |= (uint32_t)(uint8_t)input[i] << (8 * i);
    if (type == MAV_PARAM_EXT_TYPE_REAL32) {
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    int32_t value;
    memcpy(&value, &bits, sizeof(value));
    return (float)value;
}

static bool camera_parameter_get(struct ca_mavlink_server *server,
                                  const struct ca_camera_parameter *p, float *value)
{
    uint8_t thermal;
    switch (p->operation) {
    case CA_CAMERA_MODE: *value=server->camera_mode; return true;
    case CA_CAMERA_CONFIG:
        /* Format choices describe the saved configuration, including changes
         * waiting for a recording to finish, just as in the web UI. */
        *value = (float)ca_config_param_get(video_format_parameter((size_t)p->config_index)
            ? &server->parameters : &server->settings, (size_t)p->config_index);
        return true;
    case CA_CAMERA_ZOOM:
        *value = !APCAM_ZOOM_SELECTS_LENS && APCAM_TARGET!=APCAM_TARGET_ZT30 ? selected_zoom(server) :
            ca_media_lens_zoom(server->media, CA_MEDIA_LENS_WIDE);
        return isfinite(*value);
    case CA_CAMERA_OPTICAL_ZOOM:
        *value = ca_media_lens_zoom(server->media, CA_MEDIA_LENS_ZOOM);
        /* An unavailable lens still occupies an advertised parameter index.
         * Reply with NaN so reads and fetch-all complete; writes still fail. */
        return true;
    case CA_CAMERA_AUTOFOCUS: *value = 0; return true;
    case CA_CAMERA_LENS: *value = (float)ca_media_lens(server->media); return true;
    case CA_CAMERA_SOURCE: *value = ca_media_thermal_main(server->media) ? 1 : 0; return true;
    case CA_CAMERA_PALETTE:
        if (ca_media_get_thermal_palette(server->media, &thermal) < 0) return false;
        *value = thermal;
        return true;
    case CA_CAMERA_GAIN:
        if (ca_media_get_thermal_gain(server->media, &thermal) < 0) return false;
        *value = thermal;
        return true;
    }
    return false;
}

/* Preserve saved parameters and separately report the effective runtime values.
 * Polling also observes vendor/UI controls which bypass PARAM_EXT_SET. */
static void update_binlog(struct ca_mavlink_server *server, bool allow_stop)
{
    uint64_t now=monotonic_ms();
    bool wanted=server->vehicle_armed || server->settings.log_disarmed;
    bool started=false;
    if (wanted && !ca_binlog_active() && (!server->log_retry_ms || now-server->log_retry_ms>=10000)) {
        server->log_retry_ms=now;
        started=ca_binlog_start(&server->parameters);
    }
    if (!ca_binlog_active()) return;
    // Disarmed logging can start before backend initialization so startup MCU
    // packets are retained. Take the initial media snapshot here in that case too.
    if (started || !server->log_snapshot_valid) {
        server->log_snapshot_valid=false;
        for (size_t i=0;i<128;i++) server->logged_camera[i]=NAN;
        server->log_status_ms=0;
        struct ca_log_vid video={.time_us=ca_binlog_time_us(),.active=ca_media_recording(server->media)};
        const char *path=ca_media_recording_path(server->media);
        if (path) snprintf(video.path,sizeof(video.path),"%s",path);
        ca_binlog_emit(CA_LOG_VID,&video,sizeof(video));
    }
    if (!server->log_snapshot_valid || now-server->log_snapshot_ms>=100) {
        server->log_snapshot_ms=now;
        for (size_t i=0;i<ca_config_param_count() && i<128;i++) {
            int saved=ca_config_param_get(&server->parameters,i);
            int applied=ca_config_param_get(&server->settings,i);
            if (!server->log_snapshot_valid || server->logged_saved[i]!=saved)
                ca_binlog_parameter(ca_config_param_name(i),saved,false);
            if (!server->log_snapshot_valid || server->logged_applied[i]!=applied)
                ca_binlog_parameter(ca_config_param_name(i),applied,true);
            server->logged_saved[i]=saved; server->logged_applied[i]=applied;
        }
        for (size_t i=0;i<ca_camera_param_count() && i<128;i++) {
            struct ca_camera_parameter p;
            float value;
            if (!ca_camera_param_info(i,&p) || p.operation==CA_CAMERA_CONFIG) continue;
            if (p.operation==CA_CAMERA_GAIN || p.operation==CA_CAMERA_PALETTE) {
                uint8_t gain,palette;
                if (!ca_media_cached_thermal_controls(server->media,&gain,&palette)) continue;
                value=p.operation==CA_CAMERA_GAIN ? gain : palette;
            } else if (!camera_parameter_get(server,&p,&value)) continue;
            if (!server->log_snapshot_valid ||
                (value != server->logged_camera[i] &&
                 !(isnan(value) && isnan(server->logged_camera[i]))))
                ca_binlog_parameter(p.name, value, false);
            server->logged_camera[i]=value;
        }
        server->log_snapshot_valid=true;
    }
    struct ca_log_mode mode={};
    mode.flight_mode = server->flight_mode;
    mode.armed = server->vehicle_armed;
    mode.mode = (uint8_t)(server->target_location_active ? 1 : 0);
    mode.method = server->settings.tracking_method;
    mode.yawlock = server->yaw_lock;
    mode.recording = ca_media_recording(server->media);
    mode.system = server->system_id;
    if (started || now-server->log_status_ms>=1000 || memcmp(&mode,&server->logged_mode,sizeof(mode))) {
        server->logged_mode=mode;
        mode.time_us=ca_binlog_time_us();
        ca_binlog_emit(CA_LOG_MODE,&mode,sizeof(mode));
        CA_BINLOG(CA_LOG_ROI,ca_log_roi,.lat=server->target_lat_e7,.lon=server->target_lon_e7,
            .alt=server->target_alt_amsl_m,.active=server->target_location_active);
        CA_BINLOG(CA_LOG_TIME,ca_log_time,.utc_us=realtime_us());
        ca_binlog_stats();
        server->log_status_ms=now;
    }
    if (!wanted && allow_stop) {
        ca_binlog_message("Logging stopped: disarmed");
        ca_binlog_stop();
        server->log_retry_ms=0;
    }
}

static int camera_parameter_set(struct ca_mavlink_server *server,
                                 const struct ca_camera_parameter *p, float value)
{
    switch (p->operation) {
    case CA_CAMERA_MODE: return set_camera_mode(server,(unsigned)value)==MAV_RESULT_ACCEPTED ? 0 : -1;
    case CA_CAMERA_CONFIG:
        if(live_config_parameter((size_t)p->config_index))
            return apply_camera_config(server,(size_t)p->config_index,value);
        if (ca_config_param_save(&server->parameters, server->config_path,
                                 (size_t)p->config_index, value) < 0) return -1;
        parameter_status(server, "Parameter saved; restart required");
        return 0;
    case CA_CAMERA_ZOOM:
        server->zoom_rate = 0;
        return !APCAM_ZOOM_SELECTS_LENS && APCAM_TARGET!=APCAM_TARGET_ZT30 ? set_selected_zoom(server, value) :
            ca_media_set_lens_zoom(server->media, CA_MEDIA_LENS_WIDE, value);
    case CA_CAMERA_OPTICAL_ZOOM:
        server->zoom_rate = 0;
        return ca_media_set_lens_zoom(server->media, CA_MEDIA_LENS_ZOOM, value);
    case CA_CAMERA_AUTOFOCUS:
        return value == 0 || set_focus(server, FOCUS_TYPE_AUTO, 0) == MAV_RESULT_ACCEPTED ? 0 : -1;
    case CA_CAMERA_LENS:
        server->zoom_rate = 0;
        return ca_media_set_lens(server->media, (enum ca_media_lens)(int)value);
    case CA_CAMERA_SOURCE: return ca_media_set_thermal_main(server->media, value != 0);
    case CA_CAMERA_PALETTE: return ca_media_set_thermal_palette(server->media, (uint8_t)value);
    case CA_CAMERA_GAIN: return ca_media_set_thermal_gain(server->media, (uint8_t)value);
    }
    return -1;
}

static void send_ext_parameter(struct ca_mavlink_server *server,
                                 const struct route *route, size_t index)
{
    struct ca_camera_parameter p;
    float current;
    if (!ca_camera_param_info(index, &p) || !camera_parameter_get(server, &p, &current)) return;
    mavlink_param_ext_value_t value = {};
    value.param_count = (uint16_t)ca_camera_param_count();
    value.param_index = (uint16_t)index;
    value.param_type = (uint8_t)p.type;
    memcpy(value.param_id, p.name, strlen(p.name));
    ext_encode(value.param_value, p.type, current);
    mavlink_message_t response;
    mavlink_msg_param_ext_value_encode_status(server->system_id, server->camera_component_id,
        &server->encode_status, &response, &value);
    (void)send_message(server, route, &response);
}

static uint8_t handle_ext_parameter(struct ca_mavlink_server *server,
                                   const struct route *route, const mavlink_message_t *message)
{
    uint8_t system, component;
    int index = -1;
    char name[17] = {};
    mavlink_param_ext_set_t set = {};
    if (message->msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_LIST) {
        mavlink_param_ext_request_list_t request;
        mavlink_msg_param_ext_request_list_decode(message, &request);
        system = request.target_system;
        component = request.target_component;
    } else if (message->msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_READ) {
        mavlink_param_ext_request_read_t request;
        mavlink_msg_param_ext_request_read_decode(message, &request);
        system = request.target_system;
        component = request.target_component;
        memcpy(name, request.param_id, 16);
        index = request.param_index == -1 ? ca_camera_param_find(name) : request.param_index;
    } else {
        mavlink_msg_param_ext_set_decode(message, &set);
        system = set.target_system;
        component = set.target_component;
        memcpy(name, set.param_id, 16);
        index = ca_camera_param_find(name);
    }
    if (!target_matches(server, system, component, server->camera_component_id)) return 255;
    refresh_parameters(server);
    if (message->msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_LIST) {
        struct ext_parameter_list *list = NULL;
        for (unsigned i = 0; i < CA_MAVLINK_CLIENTS; i++) {
            struct ext_parameter_list *candidate = &server->ext_lists[i];
            if (candidate->active && candidate->system == message->sysid && candidate->component == message->compid) {
                list = candidate;
                break;
            }
            if (!candidate->active && !list) list = candidate;
        }
        if (list) {
            *list = {};
            list->active = true;
            list->system = message->sysid;
            list->component = message->compid;
            list->route = *route;
        }
        return 255;
    }
    struct ca_camera_parameter p;
    bool found = index >= 0 && ca_camera_param_info((size_t)index, &p);
    if (message->msgid != MAVLINK_MSG_ID_PARAM_EXT_SET) {
        if (found) send_ext_parameter(server, route, (size_t)index);
        return 255;
    }
    mavlink_param_ext_ack_t ack {};
    ack.param_result = PARAM_ACK_VALUE_UNSUPPORTED;
    memcpy(ack.param_id, set.param_id, 16);
    ack.param_type = found ? (uint8_t)p.type : set.param_type;
    float current = 0;
    if (found) {
        (void)camera_parameter_get(server, &p, &current);
        float requested = ext_decode(set.param_value, set.param_type);
        if (set.param_type == p.type && ca_camera_param_valid(&p, requested)) {
            if (p.operation == CA_CAMERA_CONFIG) {
                /* Pipeline reconfiguration can outlast the normal set timeout. */
                mavlink_message_t progress;
                ack.param_result = PARAM_ACK_IN_PROGRESS;
                ext_encode(ack.param_value, p.type, current);
                mavlink_msg_param_ext_ack_encode_status(server->system_id,
                    server->camera_component_id, &server->encode_status, &progress, &ack);
                (void)send_message(server, route, &progress);
            }
            if (camera_parameter_set(server, &p, requested) == 0) {
                ack.param_result = PARAM_ACK_ACCEPTED;
                /* ACK reports the accepted command value. Subsequent reads
                 * return actual state (including action reset/readback). */
                current = requested;
            } else ack.param_result = PARAM_ACK_FAILED;
        }
    }
    if (found) ext_encode(ack.param_value, p.type, current);
    mavlink_message_t response;
    mavlink_msg_param_ext_ack_encode_status(server->system_id, server->camera_component_id,
        &server->encode_status, &response, &ack);
    (void)send_message(server, route, &response);
    if (found && ack.param_result == PARAM_ACK_ACCEPTED && p.operation != CA_CAMERA_CONFIG) {
        ca_binlog_parameter(p.name,current,false);
        send_camera_settings(server, route);
    }
    return ack.param_result;
}

static void handle_camera_ftp(struct ca_mavlink_server *server,
                               const struct route *route, const mavlink_message_t *message)
{
    mavlink_file_transfer_protocol_t request, reply = {};
    mavlink_msg_file_transfer_protocol_decode(message, &request);
    if (request.target_network != 0 ||
        !target_matches(server, request.target_system, request.target_component, server->camera_component_id)) return;
    reply.target_system = message->sysid;
    reply.target_component = message->compid;
    /* card downloads need multi-packet bursts to be usable; keep them short
     * on the UART so gimbal control traffic is not delayed */
    server->ftp.burst_packets = (route->kind == ROUTE_UART || route->kind == ROUTE_MCU) ? 8U : 64U;
    ca_camera_ftp_reply(&server->ftp, server->definition_xml, server->definition_length,
                        message->sysid, message->compid, monotonic_ms(), request.payload, reply.payload);
    mavlink_message_t response;
    mavlink_msg_file_transfer_protocol_encode_status(server->system_id, server->camera_component_id,
        &server->encode_status, &response, &reply);
    if (send_message(server, route, &response) < 0) return;
    /* end the burst on the last packet the link can still queue, so the
     * client never waits for packets that were never sent */
    bool final = route_send_room(server, route) < 2U * MAVLINK_MAX_PACKET_LEN;
    while (ca_camera_ftp_burst_next(&server->ftp, message->sysid, message->compid, reply.payload, final)) {
        mavlink_msg_file_transfer_protocol_encode_status(server->system_id, server->camera_component_id,
            &server->encode_status, &response, &reply);
        if (send_message(server, route, &response) < 0) break;
        final = route_send_room(server, route) < 2U * MAVLINK_MAX_PACKET_LEN;
    }
}

static void log_mavlink_command(const mavlink_message_t *message, uint8_t result)
{
    ca_log_mavc r = {.time_us=ca_binlog_time_us()};
    r.source_system = message->sysid;
    r.source_component = message->compid;
    r.result = result;
    if (message->msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
        mavlink_command_long_t c;
        mavlink_msg_command_long_decode(message, &c);
        r.target_system=c.target_system; r.target_component=c.target_component;
        r.frame=255; r.command=c.command; r.was_long=1;
        r.p1=c.param1; r.p2=c.param2; r.p3=c.param3; r.p4=c.param4;
        r.x=c.param5; r.y=c.param6; r.z=c.param7;
    } else {
        mavlink_command_int_t c;
        mavlink_msg_command_int_decode(message, &c);
        r.target_system=c.target_system; r.target_component=c.target_component;
        r.frame=c.frame; r.command=c.command;
        r.p1=c.param1; r.p2=c.param2; r.p3=c.param3; r.p4=c.param4;
        r.x=c.x; r.y=c.y; r.z=c.z;
    }
    ca_binlog_emit(CA_LOG_MAVC, &r, sizeof(r));
}

static void log_gimbal_setpoint(const mavlink_message_t *message, uint8_t result)
{
    mavlink_gimbal_device_set_attitude_t c;
    mavlink_msg_gimbal_device_set_attitude_decode(message, &c);
    CA_BINLOG(CA_LOG_GMBC, ca_log_gmbc,
        .target_system=c.target_system, .target_component=c.target_component,
        .source_system=message->sysid, .source_component=message->compid,
        .flags=c.flags, .q={c.q[0], c.q[1], c.q[2], c.q[3]},
        .rates={c.angular_velocity_x, c.angular_velocity_y, c.angular_velocity_z}, .result=result);
}

static void log_parameter_write(const mavlink_message_t *message, uint8_t result)
{
    ca_log_mavp r = {.time_us=ca_binlog_time_us()};
    r.source_system=message->sysid; r.source_component=message->compid;
    r.result=result;
    if (message->msgid == MAVLINK_MSG_ID_PARAM_EXT_SET) {
        mavlink_param_ext_set_t c;
        mavlink_msg_param_ext_set_decode(message, &c);
        r.target_system=c.target_system; r.target_component=c.target_component;
        r.type=c.param_type; r.extended=1;
        memcpy(r.name, c.param_id, sizeof(r.name));
        memcpy(r.raw, c.param_value, sizeof(r.raw));
        r.value=(c.param_type == MAV_PARAM_EXT_TYPE_UINT8 || c.param_type == MAV_PARAM_EXT_TYPE_INT32 ||
                 c.param_type == MAV_PARAM_EXT_TYPE_REAL32) ? ext_decode(c.param_value, c.param_type) : NAN;
    } else {
        mavlink_param_set_t c;
        mavlink_msg_param_set_decode(message, &c);
        r.target_system=c.target_system; r.target_component=c.target_component;
        r.type=c.param_type;
        memcpy(r.name, c.param_id, sizeof(r.name));
        memcpy(r.raw, &c.param_value, sizeof(c.param_value));
        r.value=c.param_value;
    }
    ca_binlog_emit(CA_LOG_MAVP, &r, sizeof(r));
}

static void process_message(struct ca_mavlink_server *server,
                            const struct route *route,
                            const mavlink_message_t *message)
{
#ifdef CAMERA_APP_ZT30_RAM
    // Qualification accepts camera information and one fixed lens read. No parameter,
    // actuator, clock, stream configuration or recording mutations.
    if(message->msgid==MAVLINK_MSG_ID_COMMAND_LONG) {
        const auto command=mavlink_msg_command_long_get_command(message);
        if(command!=MAV_CMD_REQUEST_MESSAGE && command!=MAV_CMD_REQUEST_CAMERA_INFORMATION &&
           command!=MAV_CMD_REQUEST_CAMERA_CAPTURE_STATUS && command!=MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION)return;
    } else if(message->msgid==MAVLINK_MSG_ID_PARAM_EXT_REQUEST_READ) {
        mavlink_param_ext_request_read_t request{};
        mavlink_msg_param_ext_request_read_decode(message,&request);
        if(!ca_zt30::ram_lens_read(request.param_index,request.param_id))return;
    } else if(message->msgid!=MAVLINK_MSG_ID_HEARTBEAT)return;
#endif
    /* Stay silent until automatic identity is known. Never select a GCS,
     * even one which incorrectly advertises a valid autopilot type. */
    if (message->msgid == MAVLINK_MSG_ID_HEARTBEAT && message->len >= 6U &&
        message->sysid != 0U &&
        mavlink_msg_heartbeat_get_type(message) != MAV_TYPE_GCS &&
        mavlink_msg_heartbeat_get_autopilot(message) != MAV_AUTOPILOT_INVALID &&
        server->autopilot_system_id == 0U) {
        server->autopilot_system_id = message->sysid;
        server->autopilot_component_id = message->compid;
        if (server->system_id == 0U) server->system_id = message->sysid;
        ca_log("MAVLink system ID=%u flight controller=%u/%u",
               server->system_id, message->sysid, message->compid);
        broadcast_heartbeats(server);
    }
    if (server->system_id == 0U) return;
    if (message->msgid == MAVLINK_MSG_ID_HEARTBEAT && message->len >= 7U &&
        message->sysid == server->system_id &&
        message->compid == MAV_COMP_ID_AUTOPILOT1) {
        bool armed = (mavlink_msg_heartbeat_get_base_mode(message) &
                      MAV_MODE_FLAG_SAFETY_ARMED) != 0U;
        server->have_vehicle_armed = true;
        server->vehicle_armed = armed;
        server->flight_mode = mavlink_msg_heartbeat_get_custom_mode(message);
        update_binlog(server, false);
        mavlink_heartbeat_t heartbeat;
        mavlink_msg_heartbeat_decode(message, &heartbeat);
        CA_BINLOG(CA_LOG_MAVH, ca_log_mavh,
            .source_system=message->sysid, .source_component=message->compid,
            .custom_mode=heartbeat.custom_mode, .type=heartbeat.type, .autopilot=heartbeat.autopilot,
            .base_mode=heartbeat.base_mode, .system_status=heartbeat.system_status, .version=heartbeat.mavlink_version);
        /* Reconcile on each matching heartbeat, including the first heartbeat
         * after a restart while airborne. Link loss is not a disarm event. */
        if (server->settings.autorecord == CA_AUTORECORD_WHILE_ARMED &&
            ca_media_recording(server->media) != armed) {
            if (ca_media_set_recording(server->media, armed) < 0) {
                ca_log("armed-state recording could not %s: %s",
                       armed ? "start" : "stop", strerror(errno));
            } else {
                ca_log("recording %s: vehicle %u %s", armed ? "started" : "stopped",
                       server->system_id, armed ? "armed" : "disarmed");
            }
        }
    }
    survey_message(server,message);
    if (message->msgid == MAVLINK_MSG_ID_HEARTBEAT) update_binlog(server, true);
    if (message->msgid == MAVLINK_MSG_ID_FILE_TRANSFER_PROTOCOL) {
        handle_camera_ftp(server, route, message);
        return;
    }
    if (message->msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_LIST ||
        message->msgid == MAVLINK_MSG_ID_PARAM_EXT_REQUEST_READ ||
        message->msgid == MAVLINK_MSG_ID_PARAM_EXT_SET) {
        uint8_t result = handle_ext_parameter(server, route, message);
        if (message->msgid == MAVLINK_MSG_ID_PARAM_EXT_SET) {
            update_binlog(server, false); // Include the write which enables LOG_DISARMED.
            log_parameter_write(message, result);
        }
        return;
    }
    if (message->msgid == MAVLINK_MSG_ID_PARAM_REQUEST_LIST ||
        message->msgid == MAVLINK_MSG_ID_PARAM_REQUEST_READ ||
        message->msgid == MAVLINK_MSG_ID_PARAM_SET) {
        uint8_t result = handle_parameter(server, message);
        if (message->msgid == MAVLINK_MSG_ID_PARAM_SET) {
            update_binlog(server, false);
            log_parameter_write(message, result);
        }
        return;
    }
    // GCS heartbeats can be forwarded over the same ArduPilot NET link.  Do
    // not let one replace the autopilot as the target of gimbal status or
    // ArduPilot will route, but not consume, the resulting status messages.
    if (message->msgid == MAVLINK_MSG_ID_HEARTBEAT &&
        message->sysid == server->autopilot_system_id &&
        message->compid == server->autopilot_component_id) {
        request_telemetry_intervals(server, route);
        /* ArduPilot may retain a MAVLink mount backend while camera-app is
         * restarted from the web UI.  Refresh the capability flags once so a
         * position-targeting change takes effect without restarting it too. */
        if (!server->gimbal_information_announced) {
            send_gimbal_information(server, route);
            server->gimbal_information_announced = true;
        }
    } else if (message->msgid == MAVLINK_MSG_ID_COMMAND_LONG) {
        log_mavlink_command(message, handle_command_long(server, route, message));
    } else if (message->msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
        handle_global_position_int(server, message);
    } else if (message->msgid == MAVLINK_MSG_ID_ATTITUDE) {
        handle_attitude(server, message);
    } else if (message->msgid == MAVLINK_MSG_ID_SYSTEM_TIME) {
        handle_system_time(server, message);
    } else if (message->msgid == MAVLINK_MSG_ID_GIMBAL_DEVICE_SET_ATTITUDE &&
               route->kind != ROUTE_MCU) {
        log_gimbal_setpoint(message, handle_gimbal_set_attitude(server, message));
    } else if (message->msgid ==
               MAVLINK_MSG_ID_AUTOPILOT_STATE_FOR_GIMBAL_DEVICE) {
        handle_autopilot_state_for_gimbal(server, message);
    } else if (message->msgid == MAVLINK_MSG_ID_COMMAND_INT && route->kind != ROUTE_MCU) {
        // The COMMAND_INT handlers below implement the gimbal service.
        log_mavlink_command(message, handle_command_int(server, route, message));
    }
}

static void feed(struct ca_mavlink_server *server,
                 struct ca_mavlink_parser *parser, const struct route *route,
                 const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        mavlink_message_t message;
        int result = ca_mavlink_parse_byte(parser, data[i], &message);
        if (result == 1) {
            if (route->kind == ROUTE_MCU) server->have_mcu_peer = true;
            process_message(server, route, &message);
            if (message.msgid == MAVLINK_MSG_ID_HEARTBEAT &&
                message.sysid == server->system_id &&
                message.compid == MAV_COMP_ID_AUTOPILOT1 &&
                mavlink_msg_heartbeat_get_autopilot(&message) != MAV_AUTOPILOT_INVALID) {
                server->flight_controller_route = *route;
                server->have_flight_controller_route = true;
            }
            ca_support_mavlink_send(server->support, &message);
        }
    }
}

void ca_mavlink_server_feed_mcu(struct ca_mavlink_server *server,
                                const uint8_t *data, size_t length)
{
    if (!server) return;
    const struct route route = {.kind = ROUTE_MCU};
    feed(server, &server->mcu_parser, &route, data, length);
}

static void close_client(struct ca_mavlink_server *server, unsigned slot)
{
    if (slot >= CA_MAVLINK_CLIENTS || server->clients[slot].fd < 0) return;
    for (unsigned i = 0; i < CA_MAVLINK_CLIENTS; i++) {
        if (server->ext_lists[i].route.kind == ROUTE_TCP &&
            server->ext_lists[i].route.client == slot) server->ext_lists[i].active = false;
    }
    if (server->have_flight_controller_route && server->flight_controller_route.kind == ROUTE_TCP &&
        server->flight_controller_route.client == slot) server->have_flight_controller_route = false;
    (void)ca_poll_change(server->pollset, CA_POLL_DEL,
                    server->clients[slot].fd, NULL);
    close(server->clients[slot].fd);
    server->clients[slot].fd = -1;
    ca_mavlink_parser_init(&server->clients[slot].parser);
    ca_log("MAVLink TCP client %u disconnected", slot + 1U);
}

static int accept_clients(struct ca_mavlink_server *server)
{
    for (;;) {
        struct sockaddr_in address;
        socklen_t length = sizeof(address);
        int fd = accept4(server->listener_fd, (struct sockaddr *)&address,
                         &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        #ifdef CAMERA_APP_ZT30_RAM
        if(!ca_zt30_ram_peer(reinterpret_cast<const sockaddr*>(&address))) { close(fd);continue; }
        #endif
        unsigned slot;
        for (slot = 0; slot < CA_MAVLINK_CLIENTS; slot++) {
            if (server->clients[slot].fd < 0) break;
        }
        if (slot == CA_MAVLINK_CLIENTS) {
            close(fd);
            continue;
        }
        server->clients[slot].fd = fd;
        set_send_buffer(fd);
        ca_mavlink_parser_init(&server->clients[slot].parser);
        ca_poll_event event = {
            .events = CA_POLL_IN | CA_POLL_RDHUP,
            .data = { .u64 = CA_EVENT_CLIENT_BASE + slot },
        };
        if (ca_poll_change(server->pollset, CA_POLL_ADD, fd, &event) < 0) {
            close_client(server, slot);
            return -1;
        }
        char host[INET_ADDRSTRLEN] = "?";
        (void)inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
        ca_log("MAVLink TCP client %u connected from %s:%u", slot + 1U,
               host, ntohs(address.sin_port));
        struct route route = {.kind = ROUTE_TCP, .client = slot};
        send_heartbeat(server, &route, server->camera_component_id, 30U);
#if APCAM_GIMBAL_OPTIONAL
        if (have_gimbal(server))
#endif
        send_heartbeat(server, &route, server->gimbal_component_id, 26U);
    }
}

static int read_client(struct ca_mavlink_server *server, unsigned slot)
{
    uint8_t data[2048];
    struct route route = {.kind = ROUTE_TCP, .client = slot};
    for (;;) {
        ssize_t received = recv(server->clients[slot].fd, data, sizeof(data), 0);
        if (received > 0) {
            feed(server, &server->clients[slot].parser, &route, data,
                 (size_t)received);
            continue;
        }
        if (received == 0) {
            close_client(server, slot);
            return 0;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        close_client(server, slot);
        return 0;
    }
}

static int read_udp(struct ca_mavlink_server *server)
{
    for (;;) {
        uint8_t data[2048];
        struct route route = {.kind = ROUTE_UDP};
        route.address_length = sizeof(route.address);
        ssize_t received = recvfrom(server->udp_fd, data, sizeof(data), 0,
                                    (struct sockaddr *)&route.address,
                                    &route.address_length);
        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        #ifdef CAMERA_APP_ZT30_RAM
        if(!ca_zt30_ram_peer(reinterpret_cast<const sockaddr*>(&route.address)))continue;
        #endif
        server->last_udp = route;
        server->have_udp_peer = true;
        feed(server, &server->udp_parser, &route, data, (size_t)received);
    }
}

static int read_uart(struct ca_mavlink_server *server)
{
    const struct route route = {.kind = ROUTE_UART};
    for (;;) {
        uint8_t data[2048];
        ssize_t received = read(server->uart_fd, data, sizeof(data));
        if (received > 0) {
            feed(server, &server->uart_parser, &route, data,
                 (size_t)received);
            continue;
        }
        if (received == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

#include "apcam/survey_runtime.h"
#include "survey_server.inc"

static uint8_t survey_capture_start(ca_mavlink_server *s,int32_t count,float interval)
{
    const uint8_t result=set_camera_mode(s,CAMERA_MODE_IMAGE_SURVEY);
    if(result!=MAV_RESULT_ACCEPTED) return result;
    s->survey->remaining=count?count:-1;
    s->survey->interval_ms=unsigned(fminf(interval*1000,UINT32_MAX));
    return MAV_RESULT_ACCEPTED;
}

int ca_mavlink_server_open(struct ca_mavlink_server **result,
                           const struct ca_mavlink_server_config *config)
{
    if (result == NULL || config == NULL || config->backend == NULL ||
        config->media == NULL || config->capture_root == NULL ||
        config->config_path == NULL || config->settings.mavlink_system_id > 255U ||
        config->settings.mavlink_camera_component_id < MAV_COMP_ID_CAMERA ||
        config->settings.mavlink_camera_component_id > MAV_COMP_ID_CAMERA6 ||
        config->tcp_port > 65535U || config->udp_port > 65535U) {
        errno = EINVAL;
        return -1;
    }
    struct ca_mavlink_server *server = (struct ca_mavlink_server*)(calloc(1, sizeof(*server)));
    if (server == NULL) return -1;
    server->pollset = NULL;
    server->udp_fd = -1;
    server->listener_fd = -1;
    server->uart_fd = -1;
    for (unsigned i = 0; i < CA_MAVLINK_CLIENTS; i++) server->clients[i].fd = -1;
    if (strlen(config->capture_root) >= sizeof(server->capture_root)) {
        free(server);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(server->capture_root, config->capture_root,
           strlen(config->capture_root) + 1U);
    ca_camera_ftp_init(&server->ftp, config->record_root, config->capture_root, config->log_root);
    server->manual_control = config->manual_control;
    server->backend = config->backend;
    server->media = config->media;
    server->settings = config->settings;
    snprintf(server->network_error, sizeof(server->network_error), "%s", config->network_error ? config->network_error : "");
    server->photo_scope = config->photo_scope;
    server->rtsp_port = config->rtsp_port;
    server->system_id = (uint8_t)config->settings.mavlink_system_id;
    server->camera_component_id = (uint8_t)config->settings.mavlink_camera_component_id;
    /* Gimbal component IDs are not contiguous: #1 is 154, #2-6 are 171-175.
     * Snapshot both identities together; saved changes apply after restart. */
    static const uint8_t gimbal_components[] = {
        MAV_COMP_ID_GIMBAL, MAV_COMP_ID_GIMBAL2, MAV_COMP_ID_GIMBAL3,
        MAV_COMP_ID_GIMBAL4, MAV_COMP_ID_GIMBAL5, MAV_COMP_ID_GIMBAL6,
    };
    server->gimbal_component_id =
        gimbal_components[server->camera_component_id - MAV_COMP_ID_CAMERA];
    server->parameters = config->settings;
    const char *ready = getenv("CAMERA_APP_READY_PATH");
    if (!ready || !*ready) ready = "/run/camera-app.ready";
    if (snprintf(server->config_status_path, sizeof(server->config_status_path), "%s.config", ready) >=
        (int)sizeof(server->config_status_path)) { errno = ENAMETOOLONG; goto fail; }
    server->config_path = strdup(config->config_path);
    if (server->config_path == NULL) goto fail;
    server->definition_xml = ca_camera_definition(&server->definition_length);
    if (server->definition_xml == NULL) goto fail;
    server->definition_version = ca_camera_definition_version(
        server->definition_xml, server->definition_length);
    server->survey=new(std::nothrow) survey_runtime;
    // Capture lives in MEDIA_ROOT/DCIM/capture on all camera targets.
    {
    const char *terrain_root=getenv("CAMERA_APP_TERRAIN_ROOT");
    std::string terrain_directory=terrain_root?terrain_root:std::string(server->capture_root)+"/../../TERRAIN";
    ca_terrain_start(terrain_directory.c_str());
    }
    if(!server->survey) goto fail;
    server->survey->session=realtime_us();
    { char path[4096]; snprintf(path,sizeof(path),"%s.survey.command",server->config_path); unlink(path); }
    server->camera_mode = APCAM_HAVE_PHOTO ? 0 : 1;
    for (unsigned i = 0; i < APCAM_NUM_STREAMS; i++) {
        server->stream_enabled[i] = true;
        server->misb_stream_enabled[i] = true;
    }
#if APCAM_HAVE_THERMAL
    for (unsigned i = 0; i < APCAM_NUM_STREAMS; i++) {
        server->thermal_interval_ms[i] = THERMAL_DEFAULT_INTERVAL_MS;
    }
#endif
    server->next_image_index = 1;
    server->focus_percent = NAN;
    server->started_ms = monotonic_ms();
    server->pollset = ca_poll_open();
    if (server->pollset == NULL) goto fail;
    /* Keep the web UI and other transports available for reconfiguration,
     * even when both network listeners are unavailable. */
    server->udp_fd = open_listener(server, SOCK_DGRAM, config->udp_port,
                                   CA_EVENT_UDP);
    server->listener_fd = open_listener(server, SOCK_STREAM, config->tcp_port,
                                        CA_EVENT_LISTENER);
    if (config->uart_device != NULL) {
        server->uart_fd = ca_external_uart_open(config->uart_device);
        if (server->uart_fd < 0) {
            ca_log("MAVLink UART %s unavailable: %s; continuing without UART",
                   config->uart_device, strerror(errno));
        } else {
            ca_poll_event event = {
                .events = CA_POLL_IN, .data = { .u64 = CA_EVENT_UART }};
            if (ca_poll_change(server->pollset, CA_POLL_ADD, server->uart_fd,
                          &event) < 0) {
                int saved_errno = errno;
                close(server->uart_fd);
                server->uart_fd = -1;
                ca_log("MAVLink UART %s setup failed: %s; continuing without UART",
                       config->uart_device, strerror(saved_errno));
            } else {
                ca_mavlink_parser_init(&server->uart_parser);
                ca_log("MAVLink 2 UART listening on %s at 230400 8N1",
                       config->uart_device);
            }
        }
    }
    if (ca_support_mavlink_open(&server->support, &config->settings.support) < 0)
        ca_log("SupportProxy MAVLink startup failed: %s", strerror(errno));
    ca_mavlink_parser_init(&server->udp_parser);
    ca_mavlink_parser_init(&server->mcu_parser);
    *result = server;
    return 0;
fail: {
        int saved_errno = errno;
        ca_mavlink_server_close(server);
        errno = saved_errno;
        return -1;
    }
}

int ca_mavlink_server_fd(const struct ca_mavlink_server *server)
{
    return server != NULL ? ca_poll_fd(server->pollset) : -1;
}

int ca_mavlink_server_handle(struct ca_mavlink_server *server)
{
    ca_poll_event events[8];
    int count = ca_poll_wait(server->pollset, events,
                           (int)(sizeof(events) / sizeof(events[0])), 0);
    if (count < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < count; i++) {
        uint64_t tag = events[i].data.u64;
        if (tag == CA_EVENT_LISTENER) {
            if (accept_clients(server) < 0) return -1;
        } else if (tag == CA_EVENT_UDP) {
            if (read_udp(server) < 0) return -1;
        } else if (tag == CA_EVENT_UART) {
            int uart_error = 0;
            if ((events[i].events & CA_POLL_IN) != 0U && read_uart(server) < 0) {
                uart_error = errno;
            }
            if (uart_error == 0 && server->uart_fd >= 0 &&
                (events[i].events & CA_POLL_OUT) != 0U &&
                flush_uart(server) < 0) {
                uart_error = errno;
            }
            if (server->uart_fd >= 0 &&
                (events[i].events & (CA_POLL_ERR | CA_POLL_HUP)) != 0U &&
                uart_error == 0) {
                uart_error = EIO;
            }
            if (uart_error != 0) disable_uart(server, uart_error);
        } else if (tag >= CA_EVENT_CLIENT_BASE &&
                   tag < CA_EVENT_CLIENT_BASE + CA_MAVLINK_CLIENTS) {
            unsigned slot = (unsigned)(tag - CA_EVENT_CLIENT_BASE);
            if ((events[i].events & (CA_POLL_ERR | CA_POLL_HUP | CA_POLL_RDHUP)) != 0U) {
                close_client(server, slot);
            } else if (read_client(server, slot) < 0) {
                return -1;
            }
        }
    }
    return 0;
}

void ca_mavlink_server_periodic(struct ca_mavlink_server *server)
{
    if (server == NULL) return;
    mavlink_message_t incoming;
    for (unsigned i = 0; i < 128U && ca_support_mavlink_receive(server->support, &incoming); i++) {
        const struct route proxy_route = {.kind = ROUTE_PROXY};
        if (server->have_flight_controller_route) {
            uint8_t packet[MAVLINK_MAX_PACKET_LEN];
            size_t length = ca_mavlink_to_wire(packet, sizeof(packet), &incoming);
            if (length) (void)send_route(server, &server->flight_controller_route, packet, length);
        }
        process_message(server, &proxy_route, &incoming);
    }
    uint64_t now = monotonic_ms();
    flush_mcu(server, now);
    reload_config(server, now);
    const uint32_t tracking_interval=server->image_tracking_interval_ms?server->image_tracking_interval_ms:200;
    if(ca_media_tracking_available(server->media) && have_peer(server) &&
       tracking_interval!=UINT32_MAX && now-server->last_image_tracking_ms>=tracking_interval) {
        server->last_image_tracking_ms=now;
        send_image_tracking(server,nullptr);
    }
#if APCAM_HAVE_THERMAL
    unsigned thermal_index = thermal_stream_id(server) - 1U;
    uint32_t thermal_interval = server->thermal_interval_ms[thermal_index];
    if (have_peer(server) && thermal_interval != 0U &&
        now - server->last_thermal_ms[thermal_index] >= thermal_interval) {
        server->last_thermal_ms[thermal_index] = now;
        (void)send_thermal_range(server, NULL, thermal_index + 1U);
    }
#endif
    update_binlog(server, true);
    bool recording = ca_media_recording(server->media);
    if (recording != server->reported_recording) {
        server->reported_recording = recording;
        if (recording && server->recording_started_ms == 0U) server->recording_started_ms = now;
        if (!recording) server->recording_started_ms = 0U;
        mavlink_message_t status;
        pack_capture_status(server, &status);
        broadcast_message(server, &status);
    }
    if (server->parameter_list_active) {
        send_parameter(server, server->next_parameter++);
        if (server->next_parameter >= ca_config_param_count()) {
            server->parameter_list_active = false;
        }
    }
    for (unsigned i = 0; i < CA_MAVLINK_CLIENTS; i++) {
        struct ext_parameter_list *list = &server->ext_lists[i];
        if (!list->active) continue;
        send_ext_parameter(server, &list->route, list->next++);
        if (list->next >= ca_camera_param_count()) list->active = false;
    }
    ca_metadata_record_sample(now);
    update_survey(server, now);
    update_target_location(server, now);
    if (server->zoom_rate != 0.0f) {
        float observed = selected_zoom(server);
        if (!isfinite(observed) || ca_media_lens(server->media) != server->zoom_lens ||
            observed != server->zoom_observed) {
            /* A vendor command or pipeline change took control of this lens. */
            server->zoom_rate = 0;
        } else if (now > server->zoom_updated_ms) {
            float elapsed = (float)(now - server->zoom_updated_ms) / 1000.0f;
            server->zoom_updated_ms = now;
            server->zoom_target += server->zoom_rate * 2.0f * elapsed;
            if (server->zoom_target <= 1.0f) { server->zoom_target = 1.0f; server->zoom_rate = 0; }
            if (server->zoom_target >= selected_zoom_max(server)) {
                server->zoom_target = selected_zoom_max(server);
                server->zoom_rate = 0;
            }
            if (set_selected_zoom(server, server->zoom_target) < 0) server->zoom_rate = 0;
            server->zoom_observed = selected_zoom(server);
        }
    }
    if (server->captures_remaining != 0 && now >= server->next_capture_ms) {
        (void)capture_one(server, server->next_image_index++);
        if (server->captures_remaining > 0) server->captures_remaining--;
        if (server->captures_remaining != 0) {
            server->next_capture_ms = now +
                (uint64_t)(server->capture_interval_s * 1000.0f);
        } else {
            server->capture_interval_s = 0.0f;
        }
    }
    if (have_peer(server) && now - server->last_heartbeat_ms >= 1000U) {
        server->last_heartbeat_ms = now;
        broadcast_heartbeats(server);
    }
    // Poll the local gimbal at 10Hz, but publish MAVLink device status at the
    // low regular rate recommended by the Gimbal v2 protocol (e.g. 5Hz).
    if (have_peer(server) && now - server->last_attitude_request_ms >=
        (server->tracking_rate_active ? 50U : 100U)) {
        server->last_attitude_request_ms = now;
        (void)ca_backend_request_gimbal_attitude(server->backend);
    }
    struct ca_gimbal_attitude attitude;
    ca_metadata_set_zoom(ca_media_zoom(server->media));
    if (ca_backend_gimbal_attitude(server->backend, &attitude) &&
        now >= attitude.timestamp_ms && now - attitude.timestamp_ms < 1000U) {
        ca_metadata_set_gimbal_attitude_motion(attitude.roll_rad,
                                               attitude.pitch_rad,
                                               attitude.yaw_rad, attitude.yaw_rate_rad_s,
                                               attitude.timestamp_ms);
        if (have_peer(server) && now - server->last_attitude_status_ms >= 200U) {
            mavlink_message_t message;
            server->last_attitude_status_ms = now;
            pack_gimbal_status(server, &message, &attitude);
            broadcast_message(server, &message);
        }
    }
}

void ca_mavlink_server_close(struct ca_mavlink_server *server)
{
    if (server == NULL) return;
    stop_tracking_rate(server);
    ca_support_mavlink_close(server->support);
    for (unsigned i = 0; i < CA_MAVLINK_CLIENTS; i++) close_client(server, i);
    if (server->listener_fd >= 0) close(server->listener_fd);
    if (server->udp_fd >= 0) close(server->udp_fd);
    if (server->uart_fd >= 0) close(server->uart_fd);
    ca_poll_close(server->pollset);
    free(server->definition_xml);
    free(server->config_path);
    ca_camera_ftp_close(&server->ftp);
    ca_terrain_stop();
    delete server->survey;
    free(server);
}
