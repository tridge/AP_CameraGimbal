/*
  SIYI A8 mini backend.

  The gimbal MCU is on a UART (230400 8N1) and speaks a small framed link
  protocol.  Sub-command 0x16 tunnels complete SIYI SDK packets, so gimbal
  related SDK commands are forwarded verbatim and the MCU's SDK replies are
  handed back to the client, as the MT11 backend does over its private link.
  The MCU also streams its attitude (sub-command 0x50) at 10 Hz.

  Link frame: AA flags 02 len hdrcrc8 seq16 src dst 6B sub payload crc16
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/binlog.h"
#include "apcam/gimbal_transform.h"
#include <math.h>
#include "camera_app/udp_transport.h"
#include "camera_app/backend.h"
#include "camera_app/gimbal_angle_target.h"
#include "camera_app/gimbal_rate.h"

#include "camera_app/log.h"
#include "camera_app/private_uart.h"
#include "camera_app/siyi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if APCAM_TARGET == APCAM_TARGET_ZT30
#include "binlog_version.h"
#include <stdio.h>
#endif

#ifdef CAMERA_APP_ZT30_DEV
#include "../zt30/zoom_feedback.h"
#endif

#define PI_F 3.14159265358979323846f
#if APCAM_TARGET == APCAM_TARGET_ZR10
#define SIYI_BACKEND_NAME "zr10"
#elif APCAM_TARGET == APCAM_TARGET_ZT30
#define SIYI_BACKEND_NAME "zt30"
#else
#define SIYI_BACKEND_NAME "a8"
#endif

#define A8_DEFAULT_UART "/dev/ttyS1"
#define A8_LINK_START 0xaaU
#define A8_LINK_VERSION 0x02U
#define A8_LINK_FLAG_PLAIN 0x08U
#define A8_LINK_FLAG_REQUEST 0x09U
#define A8_LINK_FLAG_REPLY 0x0aU
#define A8_LINK_CAMERA 0x2cU
#define A8_LINK_GIMBAL 0x2eU
#define A8_LINK_CMD 0x6bU
#define A8_SUB_MOTION_MODE 0x15U
#define A8_SUB_MOUNTING 0x17U
#define A8_SUB_UNKNOWN_14 0x14U
#define A8_SUB_TUNNEL 0x16U
#define A8_SUB_ATTITUDE 0x50U
#define A8_MOUNT_QUERY_INTERVAL_MS 1000U
#define A8_INVERTED_PITCH_THRESHOLD 900
#define A8_LINK_HEADER 11U
#define A8_LINK_OVERHEAD (A8_LINK_HEADER + 2U)
#define A8_LINK_MAX_PAYLOAD 255U
#define A8_LINK_MAX_FRAME (A8_LINK_OVERHEAD + A8_LINK_MAX_PAYLOAD)

struct ca_backend {
    const bool *manual_control, *manual_command;
    int uart_fd;
    bool datagram_transport;
    bool reply_to_mcu;
#if APCAM_TARGET == APCAM_TARGET_ZT30
    uint8_t mcu_versions[8];
#endif
#if APCAM_TARGET == APCAM_TARGET_ZR10 || defined(CAMERA_APP_ZT30_DEV)
    uint64_t zoom_query_ms;
#endif
    uint16_t link_sequence;
    uint16_t public_sequence;
    uint8_t gimbal_mode;
    uint8_t rx[A8_LINK_MAX_FRAME * 2U];
    size_t rx_length;
    ca_siyi_emit_fn emit;
    void *emit_opaque;
    ca_siyi_emit_fn uart_emit;
    void *uart_opaque;
    ca_private_emit_fn private_emit;
    void *private_opaque;
    ca_recording_set_fn recording_set;
    ca_recording_get_fn recording_get;
    void *recording_opaque;
    ca_zoom_set_fn zoom_set;
    ca_zoom_get_fn zoom_get;
    void *zoom_opaque;
    ca_thermal_main_set_fn thermal_main_set;
    ca_thermal_main_get_fn thermal_main_get;
    void *thermal_main_opaque;
    ca_lens_set_fn lens_set;
    ca_lens_wide_fn lens_wide;
    void *lens_opaque;
    ca_photo_capture_fn photo_capture;
    void *photo_capture_opaque;
    ca_thermal_gain_get_fn thermal_gain_get;
    ca_thermal_gain_set_fn thermal_gain_set;
    void *thermal_gain_opaque;
    ca_thermal_palette_get_fn thermal_palette_get;
    ca_thermal_palette_set_fn thermal_palette_set;
    void *thermal_palette_opaque;
    enum ca_mount_orientation orientation;
    ca_inverted_set_fn inverted_set;
    void *inverted_opaque;
    uint8_t mounting_direction;
    bool mounting_direction_known;
    bool mounting_direction_inferred;
    uint64_t mounting_query_ms;
#ifdef CAMERA_APP_ZT30_DEV
    uint64_t attitude_request_ms;
    uint64_t rate_stop_ms;
    uint64_t rate_retry_ms;
    uint64_t rate_error_ms;
    uint8_t tx_pending[A8_LINK_MAX_FRAME];
    size_t tx_offset,tx_length;
#endif
    bool photo_capture_error_logged;
    bool unsupported_logged[256];
    uint8_t feedback_last;
    struct ca_gimbal_attitude attitude;
    bool have_attitude;
    bool attitude_logged;
    struct ca_angle_target angle_target;
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static int write_all(int fd, const uint8_t *data, size_t length, size_t *completed=nullptr)
{
    if (completed) *completed=0;
#ifdef CAMERA_APP_ZT30_DEV
    const uint64_t deadline = monotonic_ms() + 30U;
#endif
    while (length != 0U) {
        ssize_t written = write(fd, data, length);
        if (written > 0) {
            if (completed) *completed+=(size_t)written;
            data += (size_t)written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd item = {.fd = fd, .events = POLLOUT};
#ifdef CAMERA_APP_ZT30_DEV
            const uint64_t now = monotonic_ms();
            if (now >= deadline) { errno=ETIMEDOUT;return -1; }
            if (poll(&item, 1, (int)(deadline-now)) > 0) continue;
            errno=ETIMEDOUT;
#else
            if (poll(&item, 1, 1000) > 0) continue;
#endif
        }
        return -1;
    }
    return 0;
}

static int write_datagram(int fd, const uint8_t *data, size_t length)
{
    ssize_t written;

    do {
        written = send(fd, data, length, MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written == (ssize_t)length) return 0;
    if (written >= 0) errno = EIO;
    return -1;
}

static int configure_uart(int fd)
{
    struct termios settings;

    if (tcgetattr(fd, &settings) < 0) return -1;
    cfmakeraw(&settings);
    settings.c_cflag &= (tcflag_t)~(PARENB | CSTOPB | CRTSCTS);
    settings.c_cflag |= CS8 | CLOCAL | CREAD;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 1;
    if (cfsetispeed(&settings, B230400) < 0 ||
        cfsetospeed(&settings, B230400) < 0 ||
        tcsetattr(fd, TCSANOW, &settings) < 0) {
        return -1;
    }
    return tcflush(fd, TCIOFLUSH);
}

#ifdef CAMERA_APP_ZT30_DEV
static int flush_pending(ca_backend *backend)
{
    if (!backend->tx_length) return 0;
    size_t sent=0;
    const int result=write_all(backend->uart_fd,backend->tx_pending+backend->tx_offset,
        backend->tx_length-backend->tx_offset,&sent);
    backend->tx_offset+=sent;
    if (!result) {
        ca_binlog_packet(true,CA_PACKET_SIYI_MCU,CA_PACKET_MCU_UART,0,0,
            backend->tx_pending,backend->tx_length,0);
        backend->tx_offset=backend->tx_length=0;
    }
    return result;
}
#endif

static int send_link(struct ca_backend *backend, uint8_t flags, uint8_t sub,
                     const uint8_t *payload, size_t payload_length)
{
    uint8_t frame[A8_LINK_MAX_FRAME];
    uint16_t crc;

    if (payload_length > A8_LINK_MAX_PAYLOAD) {
        errno = EMSGSIZE;
        return -1;
    }
    if (backend->uart_fd < 0) {
        errno = ENOTCONN;
        return -1;
    }
#ifdef CAMERA_APP_ZT30_DEV
    if (!backend->datagram_transport && flush_pending(backend)<0) return -1;
#endif
    frame[0] = A8_LINK_START;
    frame[1] = flags;
    frame[2] = A8_LINK_VERSION;
    frame[3] = (uint8_t)payload_length;
    frame[4] = ca_crc8_maxim(frame, 4U);
    frame[5] = (uint8_t)backend->link_sequence;
    frame[6] = (uint8_t)(backend->link_sequence >> 8U);
    backend->link_sequence++;
    frame[7] = A8_LINK_CAMERA;
    frame[8] = A8_LINK_GIMBAL;
    frame[9] = A8_LINK_CMD;
    frame[10] = sub;
    if (payload_length != 0U) memcpy(frame + A8_LINK_HEADER, payload, payload_length);
    crc = ca_crc16(frame, A8_LINK_HEADER + payload_length);
    frame[A8_LINK_HEADER + payload_length] = (uint8_t)crc;
    frame[A8_LINK_HEADER + payload_length + 1U] = (uint8_t)(crc >> 8U);
    size_t length = A8_LINK_OVERHEAD + payload_length;
#ifdef CAMERA_APP_ZT30_DEV
    size_t sent=0;
    const int result=backend->datagram_transport?write_datagram(backend->uart_fd,frame,length):
        write_all(backend->uart_fd,frame,length,&sent);
    if (result<0 && sent) {
        memcpy(backend->tx_pending,frame,length);
        backend->tx_offset=sent;backend->tx_length=length;
    }
#else
    const int result = backend->datagram_transport
               ? write_datagram(backend->uart_fd, frame, length)
               : write_all(backend->uart_fd, frame, length);
#endif
    ca_binlog_packet(true, CA_PACKET_SIYI_MCU, backend->datagram_transport ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
                     0, 0, frame, length, result < 0 ? -errno : 0);
    return result;
}

static int send_tunnel(struct ca_backend *backend, const uint8_t *packet,
                       size_t length)
{
    return send_link(backend, A8_LINK_FLAG_PLAIN, A8_SUB_TUNNEL, packet, length);
}

static int send_public_command(struct ca_backend *backend, uint8_t opcode,
                               const uint8_t *payload, uint16_t payload_length)
{
    uint8_t packet[CA_SIYI_MAX_PACKET];
    size_t length;

    if (backend == NULL) {
        errno = EINVAL;
        return -1;
    }
#ifdef CAMERA_APP_ZT30_DEV
    // Stock ZT30 worker at 0x4dbb2 sends rate bytes via native 6b/06.
    // Its MCU accepts tunneled angle/getter commands, but ignores SDK 07 rates.
    // This also routes startup, watchdog and shutdown zero rates correctly.
    if (opcode == 0x07U && payload_length == 2U)
        return send_link(backend, A8_LINK_FLAG_PLAIN, 0x06U, payload, payload_length);
#endif
    length = ca_siyi_build(packet, sizeof(packet), 1,
                           backend->public_sequence++, opcode, payload,
                           payload_length);
    if (length == 0U) {
        errno = EMSGSIZE;
        return -1;
    }
    ca_angle_target_invalidate_siyi(&backend->angle_target, opcode, payload, payload_length);
#if APCAM_TARGET == APCAM_TARGET_ZR10
    return ca_backend_handle_siyi(backend, packet, length);
#else
    return send_tunnel(backend, packet, length);
#endif
}

static int send_startup(struct ca_backend *backend)
{
#ifdef CAMERA_APP_ZT30_DEV
    ca_zt30::zoom_feedback().reset();
#endif

#ifdef CAMERA_APP_ZT30_DEV
    const uint8_t stopped[2]={0,0};
    if (send_public_command(backend,0x07U,stopped,sizeof(stopped))<0) return -1;
    if (send_public_command(backend, 0x01U, NULL, 0U) < 0 ||
        send_link(backend, A8_LINK_FLAG_REQUEST, A8_SUB_MOTION_MODE, NULL, 0) < 0 ||
        send_link(backend, A8_LINK_FLAG_REQUEST, A8_SUB_MOUNTING, NULL, 0) < 0) return -1;
    return ca_backend_request_gimbal_attitude(backend);
#elif APCAM_TARGET == APCAM_TARGET_ZR10
    return send_public_command(backend, 0x01U, NULL, 0U);
#else
    static const uint8_t stream_rate = 0x0aU;
#if APCAM_TARGET == APCAM_TARGET_ZT30
    if (send_public_command(backend, 0x01U, NULL, 0U) < 0) return -1;
#endif

    /* the vendor app queries these and sends 0x0d/10 once at start */
    if (send_link(backend, A8_LINK_FLAG_REQUEST, A8_SUB_UNKNOWN_14, NULL, 0) < 0 ||
        send_link(backend, A8_LINK_FLAG_REQUEST, A8_SUB_MOTION_MODE, NULL, 0) < 0 ||
        send_link(backend, A8_LINK_FLAG_REQUEST, A8_SUB_MOUNTING, NULL, 0) < 0 ||
        send_link(backend, A8_LINK_FLAG_PLAIN, 0x0dU, &stream_rate, 1U) < 0) {
        return -1;
    }
    return 0;
#endif
}

static void log_unsupported(struct ca_backend *backend, uint8_t opcode,
                            const char *feature)
{
    bool *logged = &backend->unsupported_logged[opcode];
    if (*logged) return;
    *logged = true;
    ca_log("%s not supported (SIYI opcode=0x%02x; %s backend implementation "
           "gap; this gap will only be logged once)", feature, opcode, SIYI_BACKEND_NAME);
}

static void emit_local(struct ca_backend *backend, uint8_t opcode,
                       const uint8_t *payload, uint16_t payload_length)
{
    uint8_t packet[CA_SIYI_MAX_PACKET];
    size_t length = ca_siyi_build(packet, sizeof(packet), 2,
                                  backend->public_sequence++, opcode,
                                  payload, payload_length);
    if (length != 0U) {
        if (backend->reply_to_mcu) {
            /* The external-UART return path is a 6b/35 acknowledgement.
             * 6b/16 is the network SDK tunnel and does not reach that UART. */
            (void)send_link(backend, A8_LINK_FLAG_REPLY, 0x35, packet, length);
            return;
        }
        backend->emit(backend->emit_opaque, packet, length);
    }
}

static void emit_feedback(struct ca_backend *backend, uint8_t feedback)
{
    backend->feedback_last = feedback;
    emit_local(backend, 0x0bU, &feedback, sizeof(feedback));
}

static uint8_t toggle_recording(struct ca_backend *backend)
{
    bool active = backend->recording_get(backend->recording_opaque);
    if (backend->recording_set(backend->recording_opaque, !active) < 0) {
        ca_log("recording %s failed: %s", active ? "stop" : "start",
               strerror(errno));
        return 4U;
    }
    return active ? 6U : 5U;
}

static bool capture_photo(struct ca_backend *backend)
{
    if (backend->photo_capture(backend->photo_capture_opaque) == 0) {
        backend->photo_capture_error_logged = false;
        return true;
    }
    if (!backend->photo_capture_error_logged) {
        backend->photo_capture_error_logged = true;
        ca_log("photo capture failed: %s; further failures suppressed "
               "until a capture succeeds", strerror(errno));
    }
    return false;
}

static void emit_zoom_value(struct ca_backend *backend, uint8_t opcode)
{
    float zoom = backend->zoom_get(backend->zoom_opaque);
    unsigned tenths = (unsigned)(zoom * 10.0f + 0.5f);
    uint8_t payload[2] = {
        (uint8_t)(tenths / 10U),
        (uint8_t)(tenths % 10U),
    };
    emit_local(backend, opcode, payload, sizeof(payload));
}

static bool apply_mounting_direction(struct ca_backend *backend,
                                     uint8_t direction)
{
    bool inverted;

    if (direction < 1U || direction > 2U) return false;
#ifdef CAMERA_APP_ZT30_DEV
    inverted = direction==2U;
#else
    inverted = backend->orientation == CA_MOUNT_AUTO
                   ? direction == 2U
                   : backend->orientation == CA_MOUNT_INVERTED;
#endif
    if (backend->inverted_set(backend->inverted_opaque, inverted) < 0) {
#ifdef CAMERA_APP_ZT30_DEV
        backend->mounting_direction_known=false;
        backend->mounting_direction=inverted?2U:1U;
        if (backend->rate_stop_ms) backend->rate_stop_ms=monotonic_ms();
#endif
        ca_log("gimbal-reported mounting direction could not be applied: %s",
               strerror(errno));
        return false;
    }
    if (backend->mounting_direction != (inverted ? 2U : 1U)) {
        backend->angle_target.valid = false;
    }
    backend->mounting_direction = inverted ? 2U : 1U;
    backend->mounting_direction_known = true;
    return true;
}

static void update_attitude(struct ca_backend *backend, const uint8_t *values,
                            size_t count, bool siyi_yaw)
{
    /* Private 0x50 is right-positive but may use 0..360 degrees. Tunneled
     * SIYI 0x0d changes yaw sign with mounting. camera-app is right-positive
     * and publishes angles in the conventional -180..+180 degree interval. */
    const float scale = PI_F / 1800.0f;
    int16_t v[6] = {};

    for (size_t i = 0; i < count && i < 6U; i++) {
        v[i] = (int16_t)((uint16_t)values[i * 2U] |
                         ((uint16_t)values[i * 2U + 1U] << 8U));
    }
    /* The mounting reply is not reliable during A8 startup.  Its attitude is:
     * upright pitch is within the physical -90..+90 degree range, whereas an
     * inverted, level camera is represented near +/-180 degrees.  That makes
     * an out-of-range raw pitch an unambiguous inverted-mount indication. */
    if (backend->orientation == CA_MOUNT_AUTO &&
        !backend->mounting_direction_known &&
        (v[1] < -A8_INVERTED_PITCH_THRESHOLD ||
         v[1] > A8_INVERTED_PITCH_THRESHOLD)) {
        int16_t raw_pitch = v[1];
        if (apply_mounting_direction(backend, 2U)) {
            backend->mounting_direction_inferred = true;
            ca_log("inferred inverted mounting from raw gimbal pitch %.1f deg",
                   raw_pitch / 10.0);
        }
    }
    const struct apcam_gimbal_transform *mapping = siyi_yaw ?
        &apcam_feedback[backend->mounting_direction == 2U] :
        &apcam_private_feedback[backend->mounting_direction == 2U];
    float pose[3] = {v[2] / 10.0f, v[1] / 10.0f, v[0] / 10.0f};
    float rates[3] = {v[5] / 10.0f, v[4] / 10.0f, v[3] / 10.0f};
    apcam_transform(mapping, pose, pose, false);
    apcam_transform(mapping, rates, rates, true);
    backend->attitude.roll_rad = pose[0] * (scale * 10.0f);
    backend->attitude.pitch_rad = pose[1] * (scale * 10.0f);
    backend->attitude.yaw_rad = pose[2] * (scale * 10.0f);
    if (count >= 6U) {
        backend->attitude.roll_rate_rad_s = rates[0] * (scale * 10.0f);
        backend->attitude.pitch_rate_rad_s = rates[1] * (scale * 10.0f);
        backend->attitude.yaw_rate_rad_s = rates[2] * (scale * 10.0f);
    }
    backend->attitude.timestamp_ms = monotonic_ms();
    ca_binlog_feedback(&backend->attitude);
    backend->have_attitude = true;
    if (!backend->attitude_logged) {
        backend->attitude_logged = true;
        ca_log("gimbal attitude stream active yaw=%.1f pitch=%.1f roll=%.1f deg",
               v[0] / 10.0, v[1] / 10.0, v[2] / 10.0);
    }
}

static void handle_tunnel_reply(struct ca_backend *backend,
                                const uint8_t *payload, size_t length)
{
    struct ca_siyi_packet packet;
    size_t consumed;

    if (ca_siyi_parse_one(payload, length, &packet, &consumed) != 1 ||
        consumed != length) {
        return;
    }
#if APCAM_TARGET == APCAM_TARGET_ZR10
    if (!(packet.control & 2U))return;
    if (packet.opcode == 0x18U && packet.payload_length == 2U && (packet.control & 2U)) {
        float zoom = packet.payload[0] + packet.payload[1] * 0.1f;
        if (zoom >= 1.0f && zoom <= 30.0f)
            (void)backend->zoom_set(backend->zoom_opaque, zoom);
    }
#endif
#if APCAM_TARGET == APCAM_TARGET_ZT30
#ifdef CAMERA_APP_ZT30_DEV
    if ((packet.control & 2U) && packet.opcode == 0x18U && packet.payload_length == 2U) {
        const uint64_t now=monotonic_ms();
        const bool previously_known=isfinite(ca_zt30::zoom_feedback().get(now));
        if (ca_zt30::zoom_feedback().observe(packet.payload[0],packet.payload[1],now) && !previously_known)
            ca_log("ZT30 optical zoom feedback active %.1fx",ca_zt30::zoom_feedback().get(now));
    }
#endif
    if ((packet.control & 2U) && packet.opcode == 0x01U && packet.payload_length == 12U &&
        packet.payload[7] == 0x7a && packet.payload[11] == 0x79) {
        memcpy(backend->mcu_versions, packet.payload + 4, sizeof(backend->mcu_versions));
    }
#endif
    if (packet.opcode == 0x0dU && packet.payload_length >= 12U) {
        update_attitude(backend, packet.payload, 6U, true);
        /* The vendor protocol retains its own mounting and sign conventions. */
    } else if (packet.opcode == 0x19U && packet.payload_length == 1U &&
               packet.payload[0] <= 2U) {
        backend->gimbal_mode = packet.payload[0];
    }
    backend->emit(backend->emit_opaque, payload, length);
}

static void handle_frame(struct ca_backend *backend, const uint8_t *frame,
                         size_t payload_length)
{
    ca_binlog_packet(false, CA_PACKET_SIYI_MCU, backend->datagram_transport ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
                     0, 0, frame, A8_LINK_OVERHEAD + payload_length);
    const uint8_t *payload = frame + A8_LINK_HEADER;
    uint8_t sub = frame[10];

    if (frame[7] == A8_LINK_GIMBAL && frame[9] == 0x11 && backend->private_emit) {
        ca_private_frame parsed {};
        parsed.control = frame[1];
        parsed.sequence = frame[5] | (uint16_t(frame[6]) << 8);
        parsed.source = frame[7]; parsed.destination = frame[8];
        parsed.link = frame[9]; parsed.command = sub;
        parsed.payload = payload; parsed.payload_length = payload_length;
        parsed.raw = frame; parsed.raw_length = A8_LINK_OVERHEAD + payload_length;
        backend->private_emit(backend->private_opaque, &parsed);
        return;
    }

    if (frame[7] != A8_LINK_GIMBAL || frame[9] != A8_LINK_CMD) return;
#if APCAM_TARGET == APCAM_TARGET_ZR10
    if (frame[8] != A8_LINK_CAMERA || (frame[1] & 0x1cU) != 8U) return;
#endif
    switch (sub) {
    case 0x3f:
#ifdef CAMERA_APP_ZT30_DEV
        break; // external UART relay remains unqualified
#endif
        if (frame[8] == A8_LINK_CAMERA &&
            (frame[1] == A8_LINK_FLAG_PLAIN || frame[1] == A8_LINK_FLAG_REQUEST) &&
            backend->uart_emit) {
            backend->uart_emit(backend->uart_opaque, payload, payload_length);
        }
        break;
    case 0x35: {
        /* MCU delegates camera-local SDK operations to Linux. Responses go
         * back through its tunnel, without recursively forwarding requests. */
        if (frame[8] != A8_LINK_CAMERA ||
            (frame[1] != A8_LINK_FLAG_PLAIN && frame[1] != A8_LINK_FLAG_REQUEST)) break;
        backend->reply_to_mcu = true;
        (void)ca_backend_handle_siyi(backend, payload, payload_length);
        backend->reply_to_mcu = false;
        break;
    }
    case A8_SUB_ATTITUDE:
#ifdef CAMERA_APP_ZT30_DEV
        break; // only explicit SDK0d feedback is physically qualified
#endif
        if (payload_length >= 6U) update_attitude(backend, payload, 3U, false);
        break;
    case A8_SUB_TUNNEL:
        handle_tunnel_reply(backend, payload, payload_length);
        break;
    case A8_SUB_MOUNTING:
        /* A late reply to a startup query must not undo the unambiguous
         * attitude-based result that may already have corrected the image. */
        if (payload_length == 1U && !backend->mounting_direction_inferred) {
            apply_mounting_direction(backend, payload[0]);
        }
        break;
    case A8_SUB_MOTION_MODE:
        if (payload_length == 1U && payload[0] <= 2U) {
            backend->gimbal_mode = payload[0];
        }
        break;
    default:
        break;
    }
}

/* consume complete frames from the receive buffer; resync on bad frames */
static void parse_rx(struct ca_backend *backend)
{
    size_t offset = 0;

    while (backend->rx_length - offset >= A8_LINK_OVERHEAD) {
        const uint8_t *frame = backend->rx + offset;
        size_t payload_length = frame[3];
        size_t total = A8_LINK_OVERHEAD + payload_length;
        uint16_t crc;

        if (frame[0] != A8_LINK_START || frame[2] != A8_LINK_VERSION ||
            ca_crc8_maxim(frame, 4U) != frame[4]) {
            offset++;
            continue;
        }
        if (backend->rx_length - offset < total) break;
        crc = (uint16_t)(frame[total - 2U] | (frame[total - 1U] << 8U));
        if (ca_crc16(frame, total - 2U) != crc) {
            offset++;
            continue;
        }
        handle_frame(backend, frame, payload_length);
        offset += total;
    }
    if (offset != 0U) {
        memmove(backend->rx, backend->rx + offset, backend->rx_length - offset);
        backend->rx_length -= offset;
    }
}

int ca_backend_open(struct ca_backend **result,
                    const struct ca_backend_config *config)
{
    struct ca_backend *backend;
    const char *device;

    if (result == NULL || config == NULL || config->emit == NULL ||
        config->recording_set == NULL || config->recording_get == NULL ||
        config->zoom_set == NULL || config->zoom_get == NULL ||
        config->lens_set == NULL || config->lens_wide == NULL ||
        config->photo_capture == NULL || config->thermal_gain_get == NULL ||
        config->thermal_gain_set == NULL ||
        config->thermal_palette_get == NULL ||
        config->thermal_palette_set == NULL ||
        config->inverted_set == NULL || config->name == NULL ||
        strcmp(config->name, SIYI_BACKEND_NAME) != 0) {
        ca_log("invalid %s backend configuration (name=%s)", SIYI_BACKEND_NAME,
               config && config->name ? config->name : "null");
        errno = EINVAL;
        return -1;
    }
    device = config->uart_device != NULL ? config->uart_device : A8_DEFAULT_UART;
    backend = (struct ca_backend*)(calloc(1, sizeof(*backend)));
    if (backend == NULL) return -1;
#if APCAM_TARGET == APCAM_TARGET_ZT30
    // Known product tags are independent from firmware discovery. A zero
    // version is unknown until a validated MCU reply fills the cached tuples.
    backend->mcu_versions[3] = 0x7a;
    backend->mcu_versions[7] = 0x79;
#endif
    backend->manual_control=config->manual_control;
    backend->private_emit=config->private_emit;
    backend->private_opaque=config->private_opaque;
    backend->uart_emit=config->uart_emit;
    backend->uart_opaque=config->uart_opaque;
    backend->manual_command=config->manual_command;
    backend->datagram_transport = strncmp(device, "udp://", 6U) == 0;
    backend->uart_fd = backend->datagram_transport
                           ? ca_open_udp_transport(device)
                           : open(device, O_RDWR | O_NOCTTY | O_NONBLOCK |
                                              O_CLOEXEC);
    if (backend->uart_fd < 0 ||
        (!backend->datagram_transport &&
         (flock(backend->uart_fd, LOCK_EX | LOCK_NB) < 0 ||
          configure_uart(backend->uart_fd) < 0))) {
        int saved_errno = errno;
        ca_log("cannot open gimbal transport %s: %s", device, strerror(saved_errno));
        if (backend->uart_fd >= 0) close(backend->uart_fd);
        free(backend);
        errno = saved_errno;
        return -1;
    }
    backend->link_sequence = 1;
    backend->public_sequence = 1;
    backend->emit = config->emit;
    backend->emit_opaque = config->emit_opaque;
    backend->recording_set = config->recording_set;
    backend->recording_get = config->recording_get;
    backend->recording_opaque = config->recording_opaque;
    backend->zoom_set = config->zoom_set;
    backend->zoom_get = config->zoom_get;
    backend->zoom_opaque = config->zoom_opaque;
    backend->thermal_main_set=config->thermal_main_set;
    backend->thermal_main_get=config->thermal_main_get;
    backend->thermal_main_opaque=config->thermal_main_opaque;
    backend->lens_set = config->lens_set;
    backend->lens_wide = config->lens_wide;
    backend->lens_opaque = config->lens_opaque;
    backend->photo_capture = config->photo_capture;
    backend->photo_capture_opaque = config->photo_capture_opaque;
    backend->thermal_gain_get = config->thermal_gain_get;
    backend->thermal_gain_set = config->thermal_gain_set;
    backend->thermal_gain_opaque = config->thermal_gain_opaque;
    backend->thermal_palette_get = config->thermal_palette_get;
    backend->thermal_palette_set = config->thermal_palette_set;
    backend->thermal_palette_opaque = config->thermal_palette_opaque;
    backend->orientation = config->orientation;
    backend->inverted_set = config->inverted_set;
    backend->inverted_opaque = config->inverted_opaque;
    backend->mounting_direction = config->orientation == CA_MOUNT_INVERTED
                                      ? 2U : 1U;
    backend->mounting_direction_known =
        config->orientation != CA_MOUNT_AUTO;
#ifdef CAMERA_APP_ZT30_DEV
    backend->orientation=CA_MOUNT_AUTO;
    backend->mounting_direction_known=false;
#endif
    backend->feedback_last = 6U;
    if (send_startup(backend) < 0) {
        int saved_errno = errno;
        ca_log("cannot initialize gimbal transport %s: %s", device, strerror(saved_errno));
        ca_backend_close(backend);
        errno = saved_errno;
        return -1;
    }
    backend->mounting_query_ms = monotonic_ms();
    ca_log("%s backend opened %s%s", SIYI_BACKEND_NAME, device,
           backend->datagram_transport ? "" : " at 230400 8N1");
    *result = backend;
    return 0;
}

#ifdef CAMERA_APP_ZT30_DEV
bool ca_backend_has_gimbal(const ca_backend *backend)
{
    const uint64_t now=monotonic_ms();
    return backend && backend->mounting_direction_known && backend->mounting_direction==1U &&
        backend->have_attitude && now>=backend->attitude.timestamp_ms &&
        now-backend->attitude.timestamp_ms<1000U &&
        fabsf(backend->attitude.pitch_rad)<=PI_F/2;
}

static int dev_rates(ca_backend *backend,int yaw,int pitch)
{
    // A stop must remain available after feedback loss or a mounting change.
    if ((yaw || pitch) && !ca_backend_has_gimbal(backend)) { errno=EAGAIN;return -1; }
    // Signed raw45 produces about60deg/s on both axes. Keep SDK/private
    // requests within that qualified range and stop after refresh is lost.
    yaw=yaw>45?45:yaw<-45?-45:yaw;pitch=pitch>45?45:pitch<-45?-45:pitch;
    const uint8_t payload[2]={(uint8_t)(int8_t)yaw,(uint8_t)(int8_t)pitch};
    if ((yaw || pitch) && backend->gimbal_mode!=1U) {
        // FPV rejects yaw motion. Ordinary manual PTZ uses follow mode.
        const uint8_t follow=1;
        if (send_link(backend,A8_LINK_FLAG_PLAIN,A8_SUB_MOTION_MODE,&follow,1)<0) return -1;
        backend->gimbal_mode=follow;
    }
    backend->angle_target.valid=false;
    if (yaw || pitch) { backend->rate_stop_ms=monotonic_ms()+150U;backend->rate_retry_ms=0; }
    const int result=send_public_command(backend,0x07,payload,sizeof(payload));
    if (!result && !yaw && !pitch) backend->rate_stop_ms=0;
    return result;
}
#endif

int ca_backend_write_external_uart(struct ca_backend *backend, const uint8_t *data, size_t length)
{
#ifdef CAMERA_APP_ZT30_DEV
    (void)backend;(void)data;(void)length;errno=ENOTSUP;return -1;
#endif
    /* 6b/35 ACKs write raw bytes to the external UART, including MAVLink.
     * A8's stock 6b/3f return path drops camera replies; 35 was verified with
     * MAVLink 1/2 and multi-frame FTP while the MCU emits its own telemetry. */
    while (length) {
        size_t count = length > A8_LINK_MAX_PAYLOAD ? A8_LINK_MAX_PAYLOAD : length;
        if (send_link(backend, A8_LINK_FLAG_REPLY, 0x35, data, count) < 0) return -1;
        data += count;
        length -= count;
    }
    return 0;
}

int ca_backend_handle_private(ca_backend *backend, const ca_private_frame *request)
{
    if (!backend || !request || request->destination != A8_LINK_GIMBAL ||
        request->link != 0x11 || request->payload_length > A8_LINK_MAX_PAYLOAD) {
        errno = EINVAL;
        return -1;
    }
#ifdef CAMERA_APP_ZT30_DEV
    {
    // Use the same bounded SDK motion path as web/MAVLink, not an unchecked
    // private network-to-MCU pass-through. Presets await physical qualification.
    if (request->command!=0x9a || request->payload_length!=2U ||
        (request->control!=A8_LINK_FLAG_PLAIN && request->control!=A8_LINK_FLAG_REQUEST) ||
        (backend->manual_control && *backend->manual_control &&
         (!backend->manual_command || !*backend->manual_command))) { errno=ENOTSUP;return -1; }
    const int result=dev_rates(backend,(int8_t)request->payload[0],(int8_t)request->payload[1]);
    if (!result && (request->control&1) && backend->private_emit) {
        uint8_t status=1;
        ca_private_frame reply=*request;reply.control=A8_LINK_FLAG_REPLY;
        reply.source=A8_LINK_GIMBAL;reply.destination=request->source;
        reply.payload=&status;reply.payload_length=1;reply.raw=nullptr;reply.raw_length=0;
        backend->private_emit(backend->private_opaque,&reply);
    }
    return result;
    }
#endif
#if APCAM_TARGET == APCAM_TARGET_A8
    if (request->command == 0x9a || request->command == 0x9b) {
        /* A8 cardv translates network motion into camera-originated 6b/06
         * (rates) and 6b/07 (presets). Its MCU ignores the MT11's 11/9a,9b
         * commands. The network ACK confirms dispatch, as in stock cardv. */
        const bool rates = request->command == 0x9a;
        if ((request->control != 0x08 && request->control != 0x09) ||
            request->payload_length != (rates ? 2U : 1U) ||
            (rates && ((int8_t)request->payload[0] < -100 ||
                       (int8_t)request->payload[0] > 100 ||
                       (int8_t)request->payload[1] < -100 ||
                       (int8_t)request->payload[1] > 100))) {
            errno = EINVAL;
            return -1;
        }
        uint8_t status = rates || (request->payload[0] >= 1 && request->payload[0] <= 4);
        if (status) {
            if (rates || request->payload[0] == 3) {
                if (send_link(backend, A8_LINK_FLAG_REQUEST, rates ? 0x06 : 0x07,
                              request->payload, request->payload_length) < 0) return -1;
                backend->angle_target.valid = false;
            } else {
                /* Native pitch presets did not move the inverted A8. Use
                 * the calibrated angle path for level/look-down instead. */
                const bool keep_yaw = request->payload[0] == 4;
                if (keep_yaw && (!backend->have_attitude ||
                    monotonic_ms() - backend->attitude.timestamp_ms > 1000U)) {
                    errno = EAGAIN;
                    return -1;
                }
                const float yaw = keep_yaw ? backend->attitude.yaw_rad : 0.0f;
                const float pitch = request->payload[0] == 1 ? 0.0f : -PI_F / 2;
                if (ca_backend_set_gimbal_angles(backend, pitch, yaw) < 0) return -1;
            }
        }
        if ((request->control & 1) && backend->private_emit) {
            ca_private_frame reply = *request;
            reply.control = A8_LINK_FLAG_REPLY;
            reply.source = A8_LINK_GIMBAL;
            reply.destination = request->source;
            reply.payload = &status;
            reply.payload_length = 1;
            reply.raw = nullptr;
            reply.raw_length = 0;
            backend->private_emit(backend->private_opaque, &reply);
        }
        return 0;
    }
#endif
    uint8_t frame[A8_LINK_MAX_FRAME];
    frame[0] = 0xaa; frame[1] = request->control; frame[2] = 2;
    frame[3] = request->payload_length;
    frame[4] = ca_crc8_maxim(frame, 4);
    frame[5] = request->sequence; frame[6] = request->sequence >> 8;
    frame[7] = request->source; frame[8] = request->destination;
    frame[9] = request->link; frame[10] = request->command;
    if (request->payload_length) memcpy(frame + A8_LINK_HEADER, request->payload, request->payload_length);
    const size_t length = A8_LINK_OVERHEAD + request->payload_length;
    const uint16_t crc = ca_crc16(frame, length - 2);
    frame[length-2] = crc; frame[length-1] = crc >> 8;
    backend->angle_target.valid = false;
    const int result = backend->datagram_transport
        ? write_datagram(backend->uart_fd, frame, length)
        : write_all(backend->uart_fd, frame, length);
    ca_binlog_packet(true, CA_PACKET_SIYI_MCU,
        backend->datagram_transport ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
        0, 0, frame, length, result < 0 ? -errno : 0);
    return result;
}

int ca_backend_fd(const struct ca_backend *backend)
{
    return backend->uart_fd;
}

int ca_backend_handle_fd(struct ca_backend *backend)
{
    ssize_t received;

    if (backend->rx_length >= sizeof(backend->rx)) backend->rx_length = 0;
    do {
        received = read(backend->uart_fd, backend->rx + backend->rx_length,
                        sizeof(backend->rx) - backend->rx_length);
    } while (received < 0 && errno == EINTR);
    if (received > 0) {
        backend->rx_length += (size_t)received;
        parse_rx(backend);
        return 0;
    }
    if (received == 0 || errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

int ca_backend_handle_siyi(struct ca_backend *backend,
                           const uint8_t *packet_data, size_t length)
{
    struct ca_siyi_packet packet;
    size_t consumed;

    if (ca_siyi_parse_one(packet_data, length, &packet, &consumed) != 1 ||
        consumed != length) {
        errno = EPROTO;
        return -1;
    }
    if (ca_backend_manual_blocked(backend->manual_control,backend->manual_command,
                                  packet.opcode,packet.payload,packet.payload_length)) return 0;
    ca_binlog_vendor(packet.opcode, packet.payload, packet.payload_length);
#ifdef CAMERA_APP_ZT30_DEV
    // Camera-local and unknown opcodes must not reach the MCU accidentally.
    // These readonly getters were captured directly in Stage3.
    if (backend->reply_to_mcu) { errno=ENOTSUP;return -1; }
    switch(packet.opcode) {
    case 0x01:
        if (packet.payload_length) { errno=EINVAL;return -1; }
        break; // local AP software version plus actual cached MCU versions
    case 0x10: case 0x11: {
        if(!backend->thermal_main_set || !backend->thermal_main_get || !backend->lens_set || !backend->lens_wide) { errno=ENOTSUP;return -1; }
        if(packet.opcode==0x10 && packet.payload_length) { errno=EINVAL;return -1; }
        bool thermal=backend->thermal_main_get(backend->thermal_main_opaque);
        bool wide=backend->lens_wide(backend->lens_opaque);
        if(packet.opcode==0x11) {
            if(packet.payload_length==2 && packet.payload[0]<=2 &&
               packet.payload[1]==(packet.payload[0]==2?0:2)) {
                const bool next_thermal=packet.payload[0]==2;
                int result=backend->thermal_main_set(backend->thermal_main_opaque,next_thermal);
                if(!result && !next_thermal)result=backend->lens_set(backend->lens_opaque,packet.payload[0]==1);
                if(result) {
                    (void)backend->thermal_main_set(backend->thermal_main_opaque,thermal);
                    errno=EIO;return -1;
                }
            } else if(packet.payload_length==1 && (packet.payload[0]==3 || packet.payload[0]==5)) {
                if(backend->thermal_main_set(backend->thermal_main_opaque,false))return -1;
                if(backend->lens_set(backend->lens_opaque,packet.payload[0]==5)) {
                    (void)backend->thermal_main_set(backend->thermal_main_opaque,thermal);
                    return -1;
                }
                const uint8_t mode=packet.payload[0];emit_local(backend,packet.opcode,&mode,1);return 0;
            } else { errno=EINVAL;return -1; }
            thermal=backend->thermal_main_get(backend->thermal_main_opaque);
            wide=backend->lens_wide(backend->lens_opaque);
        }
        const uint8_t slots[2]={uint8_t(thermal?2:wide?1:0),uint8_t(thermal?0:2)};
        emit_local(backend,packet.opcode,slots,2);return 0;
    }
    case 0x1a: case 0x1b:
        if(packet.payload_length!=(packet.opcode==0x1a?0U:1U)) { errno=EINVAL;return -1; }
        break; // Qualified native thermal palette, handled locally below.
    case 0x0a:
        if (packet.payload_length || !ca_backend_has_gimbal(backend)) { errno=EAGAIN;return -1; }
        break; // shared configuration getter, no camera action
    case 0x02: case 0x0d: case 0x18: case 0x19: case 0x31:
        if (packet.payload_length) { errno=EINVAL;return -1; }
        if (backend->reply_to_mcu) { errno=ENOTSUP;return -1; }
        return send_tunnel(backend,packet_data,length);
    case 0x07:
        if (packet.payload_length!=2U) { errno=EINVAL;return -1; }
        return dev_rates(backend,(int8_t)packet.payload[0],(int8_t)packet.payload[1]);
    case 0x0e:
        if (packet.payload_length!=4U) { errno=EINVAL;return -1; }
        {
        float angles[3]={0,
            (int16_t)(packet.payload[2]|(uint16_t(packet.payload[3])<<8))/10.0f,
            (int16_t)(packet.payload[0]|(uint16_t(packet.payload[1])<<8))/10.0f};
        // Public SDK uses wire coordinates; canonical callers use right-positive yaw.
        apcam_inverse_transform(&apcam_angle_command[backend->mounting_direction==2U],angles,angles,false);
        return ca_backend_set_gimbal_angles(backend,angles[1]*PI_F/180,angles[2]*PI_F/180);
        }
    case 0x0c:
        if(packet.payload_length==1U && packet.payload[0]==0) {
            emit_feedback(backend,capture_photo(backend)?0U:1U);return 0;
        }
        if (packet.payload_length==1U && packet.payload[0]>=3 && packet.payload[0]<=5 &&
            ca_backend_has_gimbal(backend)) {
            const uint8_t mode=packet.payload[0]-3U;
            const int result=send_link(backend,A8_LINK_FLAG_PLAIN,A8_SUB_MOTION_MODE,&mode,1);
            if (!result) { backend->gimbal_mode=mode;backend->angle_target.valid=false; }
            return result;
        }
        errno=ENOTSUP;return -1;
    default:errno=ENOTSUP;return -1;
    }
#endif
    ca_angle_target_invalidate_siyi(&backend->angle_target, packet.opcode,
                                     packet.payload, packet.payload_length);
#if APCAM_TARGET == APCAM_TARGET_ZR10
    /* ZR10 delegates these SDK actions to Linux. The vendor translates them
     * to native 6b commands in its control worker (5d7bc) and zoom handler
     * (5bc38); sending them through 6b/16 merely returns them on 6b/35. */
    uint8_t native_sub=0, native_flags=A8_LINK_FLAG_REQUEST;
    switch(packet.opcode) {
    case 0x05:
    case 0x06:
        if(packet.payload_length!=1U || (int8_t)packet.payload[0]<-1 ||
           (int8_t)packet.payload[0]>1) { errno=EINVAL;return -1; }
        native_sub=packet.opcode==0x05 ? 0x04:0x05;
        break;
    case 0x07:
        if(packet.payload_length!=2U || (int8_t)packet.payload[0]<-100 ||
           (int8_t)packet.payload[0]>100 || (int8_t)packet.payload[1]<-100 ||
           (int8_t)packet.payload[1]>100) { errno=EINVAL;return -1; }
        native_sub=0x06;
        break;
    case 0x08:
        if(packet.payload_length!=1U || packet.payload[0]<1 || packet.payload[0]>3) {
            errno=EINVAL;return -1;
        }
        native_sub=0x07;
        break;
    case 0x0f:
        if(packet.payload_length!=2U || packet.payload[0]<1 || packet.payload[0]>10 ||
           packet.payload[1]>9 || (packet.payload[0]==10 && packet.payload[1])) {
            errno=EINVAL;return -1;
        }
        native_sub=0x37;native_flags=A8_LINK_FLAG_PLAIN;
        break;
    default:break;
    }
    if(native_sub) {
        if(packet.opcode!=0x07)ca_log("ZR10 control SDK=%02x native=6b/%02x data=%02x %02x",packet.opcode,
               native_sub,packet.payload[0],packet.payload_length>1 ? packet.payload[1]:0);
        return send_link(backend,native_flags,native_sub,packet.payload,packet.payload_length);
    }
    if (packet.opcode == 0x04 || packet.opcode == 0x16 || packet.opcode == 0x18) {
        if (backend->reply_to_mcu) {
            log_unsupported(backend, packet.opcode, "camera-side optical lens control");
            errno=ENOTSUP;return -1;
        }
        return send_tunnel(backend, packet_data, length);
    }
#endif
    switch (packet.opcode) {
    case 0x0c:
        if (packet.payload_length != 1U) break;
        if (packet.payload[0] == 0U) {
            emit_feedback(backend, capture_photo(backend) ? 0U : 1U);
            return 0;
        }
        if (packet.payload[0] == 2U) {
            emit_feedback(backend, toggle_recording(backend));
            return 0;
        }
        if (packet.payload[0] == 1U) {
            log_unsupported(backend, packet.opcode, "HDR");
            emit_feedback(backend, 3U);
            return 0;
        }
        if (packet.payload[0] >= 3U && packet.payload[0] <= 5U) {
            uint8_t mode = (uint8_t)(packet.payload[0] - 3U);
            backend->gimbal_mode = mode;
            return send_link(backend, A8_LINK_FLAG_PLAIN, A8_SUB_MOTION_MODE,
                             &mode, 1U);
        }
        break;
#if APCAM_TARGET == APCAM_TARGET_ZT30
    case 0x01: {
        if (packet.payload_length) break;
        uint8_t version[12] = {};
        unsigned major=0,minor=0,patch=0;
        int fields=sscanf(CA_FIRMWARE_VERSION,"v%u.%u.%u",&major,&minor,&patch);
        if (fields>=2 && major<=255 && minor<=255 && patch<=255) {
            version[0]=(uint8_t)patch; version[1]=(uint8_t)minor; version[2]=(uint8_t)major;
        }
        version[3]=0x7b;
        memcpy(version+4,backend->mcu_versions,sizeof(backend->mcu_versions));
        emit_local(backend,packet.opcode,version,sizeof(version));
        return 0;
    }
#endif
    case 0x0b:
        if (packet.payload_length != 0U) break;
        emit_feedback(backend, backend->feedback_last);
        return 0;
    case 0x04: {
        uint8_t status = 1U;
        if (packet.payload_length != 1U && packet.payload_length != 5U) break;
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    case 0x06: {
        uint8_t status = 1U;
        if (packet.payload_length != 1U) break;
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    case 0x0a: {
        uint8_t status[8] = {};
        if (packet.payload_length != 0U) break;
        status[3] = backend->recording_get(backend->recording_opaque) ? 1U : 0U;
        status[4] = backend->gimbal_mode;
        status[5] = backend->mounting_direction;
        emit_local(backend, packet.opcode, status, sizeof(status));
        return 0;
    }
    case 0x10: {
        uint8_t slots[2] = {1U, 2U};
        if (packet.payload_length != 0U) break;
        emit_local(backend, packet.opcode, slots, sizeof(slots));
        return 0;
    }
    case 0x11: {
        uint8_t slots[2] = {1U, 2U};
        if (packet.payload_length == 2U) {
            emit_local(backend, packet.opcode, slots, sizeof(slots));
            return 0;
        }
        if (packet.payload_length == 1U) {
            uint8_t mode = packet.payload[0];
            emit_local(backend, packet.opcode, &mode, sizeof(mode));
            return 0;
        }
        break;
    }
    case 0x16: {
        static const uint8_t maximum[] = {(uint8_t)APCAM_ZOOM_CONTROL_MAX, 0U};
        if (packet.payload_length != 0U) break;
        emit_local(backend, packet.opcode, maximum, sizeof(maximum));
        return 0;
    }
    case 0x18:
        if (packet.payload_length != 0U) break;
        emit_zoom_value(backend, packet.opcode);
        return 0;
    case 0x0f: {
        uint8_t status;
        if (packet.payload_length != 2U) break;
        status = backend->zoom_set(backend->zoom_opaque,
                                   (float)packet.payload[0] +
                                       (float)packet.payload[1] * 0.1f) == 0
                     ? 1U : 0U;
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    case 0x05: {
        int direction;
        float zoom;
        unsigned tenths;
        uint8_t response[2];
        if (packet.payload_length != 1U) break;
        direction = (int)(int8_t)packet.payload[0];
        zoom = backend->zoom_get(backend->zoom_opaque);
        if (direction == 1) zoom += 0.1f;
        else if (direction == -1) zoom -= 0.1f;
        else if (direction != 0) {
            errno = EINVAL;
            return -1;
        }
        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > APCAM_ZOOM_CONTROL_MAX) zoom = APCAM_ZOOM_CONTROL_MAX;
        if (backend->zoom_set(backend->zoom_opaque, zoom) < 0) return -1;
        tenths = (unsigned)(zoom * 10.0f + 0.5f);
        response[0] = (uint8_t)tenths;
        response[1] = (uint8_t)(tenths >> 8);
        emit_local(backend, packet.opcode, response, sizeof(response));
        return 0;
    }
    case 0x37: {
        uint8_t gain;
        if (packet.payload_length != 0U) break;
        if (backend->thermal_gain_get(backend->thermal_gain_opaque, &gain) < 0) {
            return -1;
        }
        emit_local(backend, packet.opcode, &gain, sizeof(gain));
        return 0;
    }
    case 0x38:
        if (packet.payload_length != 1U || packet.payload[0] > 1U) break;
        return backend->thermal_gain_set(backend->thermal_gain_opaque,
                                         packet.payload[0]);
    case 0x1a: {
        uint8_t palette;
        if (packet.payload_length != 0U) break;
        if (backend->thermal_palette_get(backend->thermal_palette_opaque,
                                         &palette) < 0) {
            return -1;
        }
        emit_local(backend, packet.opcode, &palette, sizeof(palette));
        return 0;
    }
    case 0x1b: {
        uint8_t palette;
        if (packet.payload_length != 1U) break;
        palette = packet.payload[0];
        if (backend->thermal_palette_set(backend->thermal_palette_opaque,
                                         palette) < 0) {
            return -1;
        }
        emit_local(backend, packet.opcode, &palette, sizeof(palette));
        return 0;
    }
    case 0x30: {
        uint8_t status = 0U;
        if (packet.payload_length == 8U) {
            uint64_t usec = 0;
            struct timespec requested;
            for (unsigned i = 0; i < 8U; i++) {
                usec |= (uint64_t)packet.payload[i] << (8U * i);
            }
            requested.tv_sec = (time_t)(usec / UINT64_C(1000000));
            requested.tv_nsec = (long)((usec % UINT64_C(1000000)) * 1000U);
            if (clock_settime(CLOCK_REALTIME, &requested) == 0) {
                status = 1U;
                ca_log("system time set from SIYI opcode=0x30 epoch_us=%llu",
                       (unsigned long long)usec);
            } else {
                ca_log("system time setting failed for SIYI opcode=0x30: %s",
                       strerror(errno));
            }
        }
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    case 0x14:
        log_unsupported(backend, packet.opcode, "thermal temperature range");
        return 0;
    default:
        break;
    }
    if (backend->reply_to_mcu) { errno=ENOTSUP;return -1; }
    /* everything else is the gimbal MCU's business */
    return send_tunnel(backend, packet_data, length);
}

void ca_backend_periodic(struct ca_backend *backend)
{
    uint64_t now;
#ifdef CAMERA_APP_ZT30_DEV
    if (!backend) return;
    now=monotonic_ms();
    if (backend->rate_stop_ms && now>=backend->rate_stop_ms && now>=backend->rate_retry_ms) {
        if (dev_rates(backend,0,0)<0) {
            backend->rate_retry_ms=monotonic_ms()+50U;
            if (!backend->rate_error_ms || now-backend->rate_error_ms>=1000U) {
                backend->rate_error_ms=now;
                ca_log("ZT30 gimbal stop write failed; retrying: %s",strerror(errno));
            }
        }
    }
    (void)ca_backend_request_gimbal_attitude(backend);
    if (!backend->zoom_query_ms || now-backend->zoom_query_ms>=1000U) {
        backend->zoom_query_ms=now;
        (void)send_public_command(backend,0x18U,NULL,0U);
    }
#endif

#if APCAM_TARGET == APCAM_TARGET_ZR10
    uint8_t focus[27];
    if(backend && ca_zr10_take_focus && ca_zr10_take_focus(focus))
        (void)send_link(backend,A8_LINK_FLAG_PLAIN,0x02,focus,sizeof(focus));
    if (backend && monotonic_ms() - backend->zoom_query_ms >= 1000U) {
        backend->zoom_query_ms=monotonic_ms();
        (void)send_public_command(backend, 0x18U, NULL, 0U);
    }
#endif
    if (backend == NULL || backend->orientation != CA_MOUNT_AUTO ||
        backend->mounting_direction_known) {
        return;
    }
    now = monotonic_ms();
    if (now - backend->mounting_query_ms < A8_MOUNT_QUERY_INTERVAL_MS) return;
    backend->mounting_query_ms = now;
    (void)send_link(backend, A8_LINK_FLAG_REQUEST, A8_SUB_MOUNTING, NULL, 0U);
}

int ca_backend_request_gimbal_attitude(struct ca_backend *backend)
{
#ifdef CAMERA_APP_ZT30_DEV
    if (!backend) { errno=EINVAL;return -1; }
    const uint64_t now=monotonic_ms();
    if (backend->attitude_request_ms && now-backend->attitude_request_ms<100U) return 0;
    backend->attitude_request_ms=now;
#endif
    return send_public_command(backend, 0x0dU, NULL, 0U);
}

bool ca_backend_gimbal_attitude(const struct ca_backend *backend,
                                struct ca_gimbal_attitude *attitude)
{
    if (backend == NULL || attitude == NULL || !backend->have_attitude) {
        return false;
    }
#ifdef CAMERA_APP_ZT30_DEV
    if (!ca_backend_has_gimbal(backend)) return false;
#endif
    *attitude = backend->attitude;
    return true;
}

static int16_t angle_tenths(float radians, float minimum, float maximum)
{
    float degrees = radians * (180.0f / PI_F);
    if (degrees < minimum) degrees = minimum;
    if (degrees > maximum) degrees = maximum;
    return (int16_t)(degrees * 10.0f + (degrees >= 0.0f ? 0.5f : -0.5f));
}

int ca_backend_set_gimbal_angles(struct ca_backend *backend, float pitch_rad,
                                 float yaw_rad)
{
#ifdef CAMERA_APP_ZT30_DEV
    if (!isfinite(pitch_rad) || !isfinite(yaw_rad)) { errno=EINVAL;return -1; }
    if (!ca_backend_has_gimbal(backend)) { errno=EAGAIN;return -1; }
    if (backend->rate_stop_ms && dev_rates(backend,0,0)<0) return -1;
#endif
    float angles[3] = {0, pitch_rad * (180.0f / PI_F), yaw_rad * (180.0f / PI_F)};
    angles[1] = fminf(APCAM_GIMBAL_PITCH_MAX, fmaxf(APCAM_GIMBAL_PITCH_MIN, angles[1]));
    angles[2] = fminf(APCAM_GIMBAL_YAW_MAX, fmaxf(APCAM_GIMBAL_YAW_MIN, angles[2]));
    float target_pitch = angles[1], target_yaw = angles[2];
    apcam_transform(&apcam_angle_command[backend->mounting_direction == 2U], angles, angles, false);
    int16_t yaw = angle_tenths(angles[2] * (PI_F / 180.0f), -180.0f, 180.0f);
    int16_t pitch = angle_tenths(angles[1] * (PI_F / 180.0f), -180.0f, 180.0f);
    uint64_t now = monotonic_ms();
    const ca_gimbal_attitude *retry_attitude=backend->have_attitude ? &backend->attitude : NULL;
#ifdef CAMERA_APP_ZT30_DEV
    // Raw rate feedback is unqualified and reports a stationary pitch bias.
    // Use actual position, not those rates, to decide whether a lost target
    // needs retrying. Keep the transmitted telemetry unchanged.
    ca_gimbal_attitude retry_pose=backend->attitude;
    retry_pose.yaw_rate_rad_s=retry_pose.pitch_rate_rad_s=0;
    retry_attitude=&retry_pose;
#endif
    if (ca_angle_target_suppress(&backend->angle_target, yaw, pitch,
                                 target_yaw, target_pitch,retry_attitude,now)) return 0;
    uint8_t payload[4] = {
        (uint8_t)yaw, (uint8_t)((uint16_t)yaw >> 8U),
        (uint8_t)pitch, (uint8_t)((uint16_t)pitch >> 8U),
    };
    int result = send_public_command(backend, 0x0eU, payload, sizeof(payload));
    CA_BINLOG(CA_LOG_GCMD, ca_log_gcmd, .mode=1, .pitch=target_pitch, .yaw=target_yaw,
        .wirep=(float)pitch, .wirey=(float)yaw, .result=result);
    if (result == 0) {
        ca_angle_target_sent(&backend->angle_target, yaw, pitch, now);
    }
    return result;
}

static int8_t rate_byte(float radians_per_second, float full_scale)
{
    float units = radians_per_second * (180.0f / PI_F) / (full_scale / 100.0f);
    if (units < -100.0f) units = -100.0f;
    if (units > 100.0f) units = 100.0f;
    return (int8_t)(units + (units >= 0.0f ? 0.5f : -0.5f));
}

int ca_backend_set_gimbal_rates(struct ca_backend *backend,
                                float pitch_rate_rad_s,
                                float yaw_rate_rad_s)
{
#ifdef CAMERA_APP_ZT30_DEV
    if (!backend || !isfinite(pitch_rate_rad_s) || !isfinite(yaw_rate_rad_s)) { errno=EINVAL;return -1; }
#endif
    float rates[3] = {0, pitch_rate_rad_s, yaw_rate_rad_s};
    apcam_transform(&apcam_rate_command[backend->mounting_direction == 2U], rates, rates, true);
    uint8_t payload[2] = {
        (uint8_t)rate_byte(rates[2], APCAM_VENDOR_YAW_RATE_FULL_SCALE),
        (uint8_t)rate_byte(rates[1], APCAM_VENDOR_PITCH_RATE_FULL_SCALE),
    };
#ifdef APCAM_VENDOR_YAW_RATE_CURVE
    static const float yaw_curve[] = APCAM_VENDOR_YAW_RATE_CURVE;
    payload[0] = (uint8_t)ca_gimbal_rate_command(rates[2] * (180.0f / PI_F),
        yaw_curve, sizeof(yaw_curve) / sizeof(yaw_curve[0]));
#endif
#ifdef APCAM_VENDOR_PITCH_RATE_CURVE
    static const float pitch_curve[] = APCAM_VENDOR_PITCH_RATE_CURVE;
    payload[1] = (uint8_t)ca_gimbal_rate_command(rates[1] * (180.0f / PI_F),
        pitch_curve, sizeof(pitch_curve) / sizeof(pitch_curve[0]));
#endif
#ifdef CAMERA_APP_ZT30_DEV
    int result = dev_rates(backend,(int8_t)payload[0],(int8_t)payload[1]);
#else
    int result = send_public_command(backend, 0x07U, payload, sizeof(payload));
#endif
    CA_BINLOG(CA_LOG_GCMD, ca_log_gcmd, .mode=2,
        .pitch=pitch_rate_rad_s*57.295779513f, .yaw=yaw_rate_rad_s*57.295779513f,
        .wirep=(float)(int8_t)payload[1], .wirey=(float)(int8_t)payload[0], .result=result);
    return result;
}

int ca_backend_set_gimbal_neutral(struct ca_backend *backend)
{
#ifdef CAMERA_APP_ZT30_DEV
    (void)backend;errno=ENOTSUP;return -1; // full-travel presets not qualified yet
#endif
    const uint8_t neutral = 1U;
    return send_public_command(backend, 0x08U, &neutral, sizeof(neutral));
}

bool ca_backend_recording(const struct ca_backend *backend)
{
    return backend->recording_get(backend->recording_opaque);
}

const char *ca_backend_name(const struct ca_backend *backend)
{
    (void)backend;
    return SIYI_BACKEND_NAME;
}

void ca_backend_close(struct ca_backend *backend)
{
    if (backend == NULL) return;
#ifdef CAMERA_APP_ZT30_DEV
    if (backend->uart_fd>=0) {
        bool stopped=false;
        for (unsigned i=0;i<3 && !stopped;i++) stopped=dev_rates(backend,0,0)==0;
        if (!stopped) ca_log("ZT30 gimbal stop failed on shutdown: %s",strerror(errno));
    }
#endif
    if (backend->uart_fd >= 0) close(backend->uart_fd);
    free(backend);
}

int ca_backend_set_zoom(struct ca_backend *backend, float ratio)
{
    if (!backend || !isfinite(ratio) || ratio < 1 || ratio > APCAM_ZOOM_CONTROL_MAX) { errno = EINVAL; return -1; }
#if APCAM_TARGET == APCAM_TARGET_ZR10
    unsigned tenths = (unsigned)(ratio * 10 + 0.5f);
    uint8_t payload[2] = {(uint8_t)(tenths / 10), (uint8_t)(tenths % 10)};
    return send_public_command(backend, 0x0f, payload, sizeof(payload));
#else
    return backend->zoom_set(backend->zoom_opaque, ratio);
#endif
}
int ca_backend_set_zoom_rate(struct ca_backend *backend, float rate)
{
#if APCAM_TARGET == APCAM_TARGET_ZR10
    if (!backend || !isfinite(rate) || rate < -1 || rate > 1) { errno = EINVAL; return -1; }
    uint8_t payload = (uint8_t)(int8_t)(rate > 0 ? 1 : rate < 0 ? -1 : 0);
    return send_public_command(backend, 0x05, &payload, 1);
#else
    (void)backend; (void)rate; errno = ENOTSUP; return -1;
#endif
}
