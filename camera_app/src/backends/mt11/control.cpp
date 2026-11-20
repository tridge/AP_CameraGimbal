#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/binlog.h"
#include "camera_app/backend.h"
#include "camera_app/gimbal_angle_target.h"
#include "apcam/gimbal_transform.h"
#include <math.h>

#include "camera_app/log.h"
#include "camera_app/private_uart.h"
#include "camera_app/siyi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define PI_F 3.14159265358979323846f
#define MT11_SOURCE 0x34U
#define MT11_MCU 0x2eU
#define MT11_LINK 0x6bU
#define MT11_TUNNEL 0x16U
#define MT11_REMOTE 0x0aU
#define THERMAL_RANGE_INTERVAL_MS 200U

struct ca_backend {
    const bool *manual_control, *manual_command;
    int uart_fd;
    bool datagram_transport;
    bool reply_to_mcu;
    uint16_t private_sequence;
    uint16_t public_sequence;
    uint8_t gimbal_mode;
    struct ca_private_parser parser;
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
    ca_lens_set_fn lens_set;
    ca_lens_wide_fn lens_wide;
    void *lens_opaque;
    ca_thermal_main_set_fn thermal_main_set;
    ca_thermal_main_get_fn thermal_main_get;
    void *thermal_main_opaque;
    ca_autofocus_fn autofocus;
    void *autofocus_opaque;
    ca_manual_focus_fn manual_focus;
    void *manual_focus_opaque;
    ca_thermal_range_fn thermal_range;
    void *thermal_opaque;
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
    bool thermal_continuous;
    uint64_t thermal_last_emit_ms;
    bool thermal_wait_logged;
    bool thermal_first_logged;
    bool photo_capture_error_logged;
    bool unsupported_logged[256];
    uint8_t feedback_last;
    struct ca_gimbal_attitude attitude;
    bool have_attitude;
    struct ca_angle_target angle_target;
};

static void emit_feedback(struct ca_backend *backend, uint8_t feedback);

static int write_all(int fd, const uint8_t *data, size_t length)
{
    while (length != 0U) {
        ssize_t written = write(fd, data, length);
        if (written > 0) {
            data += (size_t)written;
            length -= (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd item = {.fd = fd, .events = POLLOUT};
            if (poll(&item, 1, 1000) > 0) continue;
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

static int open_udp_transport(const char *device)
{
    const char *address = device + strlen("udp://");
    const char *separator = strrchr(address, ':');
    struct sockaddr_in peer = {.sin_family = AF_INET};
    char host[256];
    char *end;
    unsigned long port;
    int fd;

    if (separator == NULL || separator == address || separator[1] == '\0' ||
        (size_t)(separator - address) >= sizeof(host)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(host, address, (size_t)(separator - address));
    host[separator - address] = '\0';
    errno = 0;
    port = strtoul(separator + 1, &end, 10);
    if (errno != 0 || *end != '\0' || port < 1U || port > 65535U) {
        errno = EINVAL;
        return -1;
    }
    peer.sin_port = htons((uint16_t)port);
    if (strcmp(host, "localhost") == 0) {
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, host, &peer.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (const struct sockaddr *)&peer,
                           sizeof(peer)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}

static int send_private(struct ca_backend *backend, uint8_t control,
                        uint8_t command, const uint8_t *payload,
                        uint16_t payload_length)
{
    uint8_t frame[CA_PRIVATE_MAX_FRAME];
    size_t length = ca_private_build(frame, sizeof(frame), control,
                                     backend->private_sequence++, MT11_SOURCE,
                                     MT11_MCU, MT11_LINK, command, payload,
                                     payload_length);
    if (length == 0U) {
        errno = EMSGSIZE;
        return -1;
    }
    const int result = backend->datagram_transport
               ? write_datagram(backend->uart_fd, frame, length)
               : write_all(backend->uart_fd, frame, length);
    ca_binlog_packet(true, CA_PACKET_MT11, backend->datagram_transport ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
                     0, 0, frame, length, result < 0 ? -errno : 0);
    return result;
}

static int send_startup(struct ca_backend *backend)
{
    static const uint8_t hello[] = {0x0c, 0x00, 0x01, 0x89, 0x00};
    static const uint8_t interval[] = {0x64, 0x00};

    if (send_private(backend, 0x09, 0x13, hello, sizeof(hello)) < 0 ||
        send_private(backend, 0x09, 0x17, NULL, 0) < 0 ||
        send_private(backend, 0x09, 0xc7, NULL, 0) < 0 ||
        send_private(backend, 0x09, 0x62, NULL, 0) < 0 ||
        send_private(backend, 0x08, 0x0d, interval, sizeof(interval)) < 0) {
        return -1;
    }
    return 0;
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

static void log_unsupported(struct ca_backend *backend, uint8_t opcode,
                            const char *feature)
{
    bool *logged = &backend->unsupported_logged[opcode];
    if (*logged) return;
    *logged = true;
    ca_log("%s not supported (SIYI opcode=0x%02x; ArduPilot camera app "
           "implementation gap; this gap will only be logged once)",
           feature, opcode);
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

static const char *unsupported_linux_feature(const struct ca_siyi_packet *packet)
{
    switch (packet->opcode) {
    case 0x12: return "thermal point temperature";
    case 0x13: return "thermal rectangle temperature";
    case 0x20: return "encoder query";
    case 0x21: return "encoder configuration";
    case 0x40: return "preset control";
    case 0x48: return "SD-card formatting";
    case 0x49: return "storage information";
    case 0x4d: return "advanced imaging command 0x4d";
    case 0x4e: return "advanced imaging command 0x4e";
    case 0x4f: return "thermal shutter";
    case 0x50: return "advanced imaging command 0x50";
    case 0x51: return "AI stream state";
    case 0x52: return "advanced imaging command 0x52";
    case 0x53: return "advanced imaging command 0x53";
    case 0x54: return "advanced imaging command 0x54";
    case 0x55: return "AI mode";
    case 0x56: return "AI target selection";
    case 0x57: return "advanced imaging command 0x57";
    case 0x5f: return "advanced imaging command 0x5f";
    case 0x60: return "EIS state query";
    case 0x61: return "EIS state setting";
    case 0x62: return "dehaze setting";
    case 0x63: return "DIS status";
    case 0x81: return "network configuration query";
    case 0x82: return "network configuration setting";
    case 0x83: return "network configuration command";
    case 0xfa: return "advanced capture command 0xfa";
    case 0xfb: return "advanced capture command 0xfb";
    case 0xfc: return "advanced capture command 0xfc";
    case 0xfd: return "advanced capture command 0xfd";
    case 0xfe: return "advanced capture command 0xfe";
    default: return NULL;
    }
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
            (void)send_private(backend, 0x0a, 0x35, packet, (uint16_t)length);
        } else {
            backend->emit(backend->emit_opaque, packet, length);
        }
    }
}

static void emit_feedback(struct ca_backend *backend, uint8_t feedback)
{
    backend->feedback_last = feedback;
    emit_local(backend, 0x0bU, &feedback, sizeof(feedback));
}

static void current_image_slots(struct ca_backend *backend, uint8_t slots[2])
{
    if (backend->thermal_main_get(backend->thermal_main_opaque)) {
        slots[0] = 2U;
        slots[1] = 0U;
    } else {
        slots[0] = backend->lens_wide(backend->lens_opaque) ? 1U : 0U;
        slots[1] = 2U;
    }
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void put_u16_le(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static bool emit_thermal_range(struct ca_backend *backend)
{
    struct ca_thermal_range range;
    uint8_t payload[12];

    if (!backend->thermal_range(backend->thermal_opaque, &range)) {
        if (!backend->thermal_wait_logged) {
            backend->thermal_wait_logged = true;
            ca_log("thermal range requested before first radiometric frame; "
                   "waiting for capture (this gap will only be logged once)");
        }
        return false;
    }
    put_u16_le(payload + 0, ca_thermal_legacy_centi_c(range.maximum_centi_c));
    put_u16_le(payload + 2, ca_thermal_legacy_centi_c(range.minimum_centi_c));
    put_u16_le(payload + 4, range.maximum_x);
    put_u16_le(payload + 6, range.maximum_y);
    put_u16_le(payload + 8, range.minimum_x);
    put_u16_le(payload + 10, range.minimum_y);
    emit_local(backend, 0x14U, payload, sizeof(payload));
    backend->thermal_last_emit_ms = monotonic_ms();
    if (!backend->thermal_first_logged) {
        backend->thermal_first_logged = true;
        ca_log("thermal range reporting active: min=%.2fC max=%.2fC "
               "frame=%llu",
               range.minimum_centi_c / 100.0,
               range.maximum_centi_c / 100.0,
               (unsigned long long)range.frame_sequence);
    }
    return true;
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

static int set_zoom(struct ca_backend *backend, float zoom)
{
    return backend->zoom_set(backend->zoom_opaque, zoom);
}

static int select_image_lens(struct ca_backend *backend, bool wide)
{
    return backend->lens_set(backend->lens_opaque, wide);
}

static void handle_private(void *opaque, const struct ca_private_frame *frame)
{
    struct ca_backend *backend = (struct ca_backend*)(opaque);
    ca_binlog_packet(false, CA_PACKET_MT11, backend->datagram_transport ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
                     0, 0, frame->raw, frame->raw_length);

    if (frame->source == MT11_MCU && frame->link == 0x11U && backend->private_emit) {
        backend->private_emit(backend->private_opaque, frame);
        return;
    }
    if (frame->source != MT11_MCU || frame->destination != MT11_SOURCE ||
        frame->link != MT11_LINK) {
        return;
    }
    if (frame->command == 0x3fU) {
        if ((frame->control == 0x08U || frame->control == 0x09U) && backend->uart_emit)
            backend->uart_emit(backend->uart_opaque, frame->payload, frame->payload_length);
        return;
    }
    if (frame->command == 0x35U) {
        if (frame->control == 0x08U || frame->control == 0x09U) {
            backend->reply_to_mcu = true;
            (void)ca_backend_handle_siyi(backend, frame->payload, frame->payload_length);
            backend->reply_to_mcu = false;
        }
        return;
    }
    if (frame->command == MT11_TUNNEL) {
        struct ca_siyi_packet packet;
        size_t consumed;
        uint8_t copy[CA_SIYI_MAX_PACKET];

        if (ca_siyi_parse_one(frame->payload, frame->payload_length,
                              &packet, &consumed) != 1 ||
            consumed != frame->payload_length) {
            return;
        }
        if (packet.opcode == 0x0aU && packet.payload_length >= 4U) {
            if (packet.payload_length >= 5U && packet.payload[4] <= 2U) {
                backend->gimbal_mode = packet.payload[4];
            }
            memcpy(copy, frame->payload, frame->payload_length);
            (void)ca_siyi_rewrite_payload_byte(
                copy, frame->payload_length, 3,
                backend->recording_get(backend->recording_opaque) ? 1U : 0U);
            backend->emit(backend->emit_opaque, copy, frame->payload_length);
        } else {
            if (packet.opcode == 0x0dU && packet.payload_length >= 12U) {
                int16_t values[6];
                for (size_t i = 0; i < 6U; i++) {
                    values[i] = (int16_t)((uint16_t)packet.payload[i * 2U] |
                        ((uint16_t)packet.payload[i * 2U + 1U] << 8U));
                }
                const float scale = PI_F / 180.0f;
                float pose[3] = {values[2] / 10.0f, values[1] / 10.0f, values[0] / 10.0f};
                float rates[3] = {values[5] / 10.0f, values[4] / 10.0f, values[3] / 10.0f};
                const struct apcam_gimbal_transform *mapping = &apcam_feedback[backend->mounting_direction == 2U];
                apcam_transform(mapping, pose, pose, false);
                apcam_transform(mapping, rates, rates, true);
                backend->attitude.roll_rad = pose[0] * scale;
                backend->attitude.pitch_rad = pose[1] * scale;
                backend->attitude.yaw_rad = pose[2] * scale;
                backend->attitude.roll_rate_rad_s = rates[0] * scale;
                backend->attitude.pitch_rate_rad_s = rates[1] * scale;
                backend->attitude.yaw_rate_rad_s = rates[2] * scale;
                backend->attitude.timestamp_ms = monotonic_ms();
                ca_binlog_feedback(&backend->attitude);
                backend->have_attitude = true;
            }
            if (packet.opcode == 0x19U && packet.payload_length == 1U &&
                packet.payload[0] <= 2U) {
                backend->gimbal_mode = packet.payload[0];
            }
            backend->emit(backend->emit_opaque, frame->payload,
                          frame->payload_length);
        }
    } else if (frame->command == MT11_REMOTE && frame->payload_length != 0U) {
        if (frame->payload[0] == 2U) {
            emit_feedback(backend, toggle_recording(backend));
        } else if (frame->payload[0] == 0U) {
            emit_feedback(backend, capture_photo(backend) ? 0U : 1U);
        }
    } else if (frame->command == 0x17U && frame->payload_length != 0U &&
               frame->payload[0] >= 1U && frame->payload[0] <= 2U) {
        bool inverted;
        uint8_t previous_direction = backend->mounting_direction;
        backend->mounting_direction = frame->payload[0];
        inverted = backend->orientation == CA_MOUNT_AUTO
                       ? backend->mounting_direction == 2U
                       : backend->orientation == CA_MOUNT_INVERTED;
        if (backend->inverted_set(backend->inverted_opaque, inverted) < 0) {
            ca_log("gimbal-reported mounting direction could not be applied: %s",
                   strerror(errno));
        } else {
            backend->mounting_direction = inverted ? 2U : 1U;
        }
        if (backend->mounting_direction != previous_direction) {
            backend->angle_target.valid = false;
        }
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
        config->thermal_main_set == NULL ||
        config->thermal_main_get == NULL ||
        config->autofocus == NULL || config->manual_focus == NULL ||
        config->thermal_range == NULL ||
        config->photo_capture == NULL ||
        config->thermal_palette_get == NULL ||
        config->thermal_palette_set == NULL ||
        config->inverted_set == NULL ||
        config->thermal_gain_get == NULL ||
        config->thermal_gain_set == NULL ||
        config->name == NULL || strcmp(config->name, "mt11") != 0) {
        errno = EINVAL;
        return -1;
    }
    device = config->uart_device != NULL ? config->uart_device : "/dev/ttyAMA3";
    backend = (struct ca_backend*)(calloc(1, sizeof(*backend)));
    if (backend == NULL) return -1;
    backend->manual_control=config->manual_control;
    backend->manual_command=config->manual_command;
    backend->datagram_transport = strncmp(device, "udp://", 6U) == 0;
    backend->uart_fd = backend->datagram_transport
                           ? open_udp_transport(device)
                           : open(device, O_RDWR | O_NOCTTY | O_NONBLOCK |
                                              O_CLOEXEC);
    if (backend->uart_fd < 0 ||
        (!backend->datagram_transport &&
         (flock(backend->uart_fd, LOCK_EX | LOCK_NB) < 0 ||
          configure_uart(backend->uart_fd) < 0))) {
        int saved_errno = errno;
        if (backend->uart_fd >= 0) close(backend->uart_fd);
        free(backend);
        errno = saved_errno;
        return -1;
    }
    backend->private_sequence = 1;
    backend->public_sequence = 1;
    backend->gimbal_mode = 0;
    backend->emit = config->emit;
    backend->emit_opaque = config->emit_opaque;
    backend->private_emit = config->private_emit;
    backend->private_opaque = config->private_opaque;
    backend->uart_emit = config->uart_emit;
    backend->uart_opaque = config->uart_opaque;
    backend->recording_set = config->recording_set;
    backend->recording_get = config->recording_get;
    backend->recording_opaque = config->recording_opaque;
    backend->zoom_set = config->zoom_set;
    backend->zoom_get = config->zoom_get;
    backend->zoom_opaque = config->zoom_opaque;
    backend->lens_set = config->lens_set;
    backend->lens_wide = config->lens_wide;
    backend->lens_opaque = config->lens_opaque;
    backend->thermal_main_set = config->thermal_main_set;
    backend->thermal_main_get = config->thermal_main_get;
    backend->thermal_main_opaque = config->thermal_main_opaque;
    backend->autofocus = config->autofocus;
    backend->autofocus_opaque = config->autofocus_opaque;
    backend->manual_focus = config->manual_focus;
    backend->manual_focus_opaque = config->manual_focus_opaque;
    backend->thermal_range = config->thermal_range;
    backend->thermal_opaque = config->thermal_opaque;
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
    backend->feedback_last = 6U;
    ca_private_parser_init(&backend->parser);
    if (send_startup(backend) < 0) {
        int saved_errno = errno;
        ca_backend_close(backend);
        errno = saved_errno;
        return -1;
    }
    ca_log("MT11 backend opened %s%s", device,
           backend->datagram_transport ? "" : " at 230400 8N1");
    *result = backend;
    return 0;
}

int ca_backend_fd(const struct ca_backend *backend)
{
    return backend->uart_fd;
}

int ca_backend_handle_fd(struct ca_backend *backend)
{
    uint8_t buffer[2048];
    ssize_t received;

    do {
        received = read(backend->uart_fd, buffer, sizeof(buffer));
    } while (received < 0 && errno == EINTR);
    if (received > 0) {
        ca_private_parser_feed(&backend->parser, buffer, (size_t)received,
                               handle_private, backend);
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
    if (backend->reply_to_mcu && packet.opcode == 0x0aU && packet.payload_length == 0U) {
        uint8_t status[8] = {};
        status[3] = backend->recording_get(backend->recording_opaque) ? 1U : 0U;
        status[4] = backend->gimbal_mode;
        status[5] = backend->mounting_direction;
        emit_local(backend, packet.opcode, status, sizeof(status));
        return 0;
    }
    if (packet.opcode == 0x0cU && packet.payload_length == 1U &&
        packet.payload[0] == 2U) {
        emit_feedback(backend, toggle_recording(backend));
        return 0;
    }
    if (packet.opcode == 0x04U &&
        (packet.payload_length == 1U || packet.payload_length == 5U) &&
        packet.payload[0] == 1U) {
        uint16_t x = 960U;
        uint16_t y = 540U;
        if (packet.payload_length == 5U) {
            x = (uint16_t)packet.payload[1] |
                (uint16_t)((uint16_t)packet.payload[2] << 8U);
            y = (uint16_t)packet.payload[3] |
                (uint16_t)((uint16_t)packet.payload[4] << 8U);
        }
        uint8_t status = 1U;
        if ((x != 0U || y != 0U) && (x >= 1920U || y >= 1080U)) {
            status = 0U;
            ca_log("autofocus point outside 1920x1080 stream: %u,%u", x, y);
        } else if (backend->autofocus(backend->autofocus_opaque, x, y) < 0) {
            status = 0U;
            ca_log("autofocus request rejected point=%u,%u: %s", x, y,
                   strerror(errno));
        }
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    if (packet.opcode == 0x04U) {
        ca_log("invalid autofocus command payload_length=%u mode=%u",
               packet.payload_length,
               packet.payload_length != 0U ? packet.payload[0] : 0U);
        errno = EINVAL;
        return -1;
    }
    if (packet.opcode == 0x06U && packet.payload_length == 1U) {
        int direction = (int)(int8_t)packet.payload[0];
        uint8_t status = 1U;
        if ((direction < -1 || direction > 1) ||
            backend->manual_focus(backend->manual_focus_opaque,
                                  direction) < 0) {
            status = 0U;
            ca_log("manual focus request rejected direction=%d: %s",
                   direction, direction < -1 || direction > 1
                                  ? "invalid direction"
                                  : strerror(errno));
        }
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    if (packet.opcode == 0x06U) {
        uint8_t status = 0U;
        ca_log("invalid manual focus command payload_length=%u",
               packet.payload_length);
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    if (packet.opcode == 0x0cU && packet.payload_length == 1U &&
        packet.payload[0] == 0U) {
        emit_feedback(backend, capture_photo(backend) ? 0U : 1U);
        return 0;
    }
    if (packet.opcode == 0x0cU && packet.payload_length == 1U &&
        packet.payload[0] == 1U) {
        log_unsupported(backend, packet.opcode, "HDR");
        /* 0x0C has no direct ACK payload.  Report HDR-off through the
         * protocol-defined asynchronous function-feedback channel so a
         * client cannot mistake this unsupported request for success. */
        emit_feedback(backend, 3U);
        return 0;
    }
    if (packet.opcode == 0x0bU && packet.payload_length == 0U) {
        emit_feedback(backend, backend->feedback_last);
        return 0;
    }
    if (packet.opcode == 0x14U && packet.payload_length == 1U &&
        packet.payload[0] <= 2U) {
        uint8_t mode = packet.payload[0];
        backend->thermal_continuous = mode == 2U;
        if (mode != 0U) (void)emit_thermal_range(backend);
        return 0;
    }
    if (packet.opcode == 0x37U && packet.payload_length == 0U) {
        uint8_t gain;

        if (backend->thermal_gain_get(backend->thermal_gain_opaque, &gain) < 0) {
            ca_log("thermal gain query failed: %s", strerror(errno));
            return -1;
        }
        emit_local(backend, packet.opcode, &gain, sizeof(gain));
        return 0;
    }
    if (packet.opcode == 0x38U && packet.payload_length == 1U &&
        packet.payload[0] <= 1U) {
        uint8_t gain = packet.payload[0];

        if (backend->thermal_gain_set(backend->thermal_gain_opaque, gain) < 0) {
            ca_log("thermal gain setting failed: %s", strerror(errno));
            return -1;
        }
        ca_log("thermal gain set to %s (%u)",
               gain == 1U ? "high gain/low-temperature range"
                           : "low gain/high-temperature range",
               gain);
        /* MT11 does not ACK 0x38; MAVProxy confirms it with a 0x37 query. */
        return 0;
    }
    if (packet.opcode == 0x37U || packet.opcode == 0x38U) {
        ca_log("invalid thermal gain command opcode=0x%02x payload_length=%u",
               packet.opcode, packet.payload_length);
        errno = EINVAL;
        return -1;
    }
    if (packet.opcode == 0x1aU && packet.payload_length == 0U) {
        uint8_t palette;
        if (backend->thermal_palette_get(backend->thermal_palette_opaque,
                                         &palette) < 0) {
            ca_log("thermal palette query failed: %s", strerror(errno));
            return -1;
        }
        emit_local(backend, packet.opcode, &palette, sizeof(palette));
        return 0;
    }
    if (packet.opcode == 0x1bU && packet.payload_length == 1U &&
        packet.payload[0] <= 11U && packet.payload[0] != 1U) {
        uint8_t palette = packet.payload[0];
        if (backend->thermal_palette_set(backend->thermal_palette_opaque,
                                         palette) < 0) {
            ca_log("thermal palette setting failed: %s", strerror(errno));
            return -1;
        }
        emit_local(backend, packet.opcode, &palette, sizeof(palette));
        ca_log("thermal palette set to %u", palette);
        return 0;
    }
    if (packet.opcode == 0x1aU || packet.opcode == 0x1bU) {
        ca_log("invalid thermal palette command opcode=0x%02x payload_length=%u",
               packet.opcode, packet.payload_length);
        errno = EINVAL;
        return -1;
    }
    if (packet.opcode == 0x30U) {
        uint8_t status = 0;
        if (packet.payload_length == 8U) {
            uint64_t usec = 0;
            struct timespec requested;
            for (unsigned i = 0; i < 8U; i++) {
                usec |= (uint64_t)packet.payload[i] << (8U * i);
            }
            requested.tv_sec = (time_t)(usec / UINT64_C(1000000));
            requested.tv_nsec = (long)((usec % UINT64_C(1000000)) * 1000U);
            if (getenv("CAMERA_APP_TEST_SETTIME_FAILURE") == NULL &&
                clock_settime(CLOCK_REALTIME, &requested) == 0) {
                status = 1;
                ca_log("system time set from SIYI opcode=0x30 epoch_us=%llu",
                       (unsigned long long)usec);
            } else {
                if (getenv("CAMERA_APP_TEST_SETTIME_FAILURE") != NULL) errno = EPERM;
                ca_log("system time setting failed for SIYI opcode=0x30: %s",
                       strerror(errno));
            }
        } else {
            ca_log("invalid SIYI settime payload length=%u (expected 8)",
                   packet.payload_length);
        }
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    if (packet.opcode == 0x0aU && packet.payload_length == 0U) {
        uint8_t status[8] = {};
        status[3] = backend->recording_get(backend->recording_opaque) ? 1U : 0U;
        status[4] = backend->gimbal_mode;
        status[5] = backend->mounting_direction;
        emit_local(backend, packet.opcode, status, sizeof(status));
        return 0;
    }
    if (packet.opcode == 0x10U && packet.payload_length == 0U) {
        uint8_t slots[2];
        current_image_slots(backend, slots);
        emit_local(backend, packet.opcode, slots, sizeof(slots));
        return 0;
    }
    if (packet.opcode == 0x11U && packet.payload_length == 2U) {
        uint8_t main_slot = packet.payload[0];
        bool wide;
        bool was_thermal =
            backend->thermal_main_get(backend->thermal_main_opaque);

        if (main_slot == 2U && packet.payload[1] == 0U) {
            uint8_t slots[2];
            if (backend->thermal_main_set(backend->thermal_main_opaque,
                                          true) < 0) {
                slots[0] = 0xffU;
                slots[1] = 0xffU;
            } else {
                current_image_slots(backend, slots);
            }
            emit_local(backend, packet.opcode, slots, sizeof(slots));
            return 0;
        }

        if (main_slot == 0U || main_slot == 3U || main_slot == 5U) {
            wide = false;
        } else if (main_slot == 1U || main_slot == 4U) {
            wide = true;
        } else {
            uint8_t status[] = {0xffU, 0xffU};
            ca_log("unsupported SIYI image slots main=%u secondary=%u "
                   "(opcode=0x11)", packet.payload[0], packet.payload[1]);
            emit_local(backend, packet.opcode, status, sizeof(status));
            return 0;
        }
        if (backend->thermal_main_set(backend->thermal_main_opaque, false) < 0 ||
            select_image_lens(backend, wide) < 0) {
            uint8_t status[] = {0xffU, 0xffU};
            if (was_thermal) {
                (void)backend->thermal_main_set(backend->thermal_main_opaque,
                                                true);
            }
            emit_local(backend, packet.opcode, status, sizeof(status));
            return 0;
        }
        uint8_t slots[2];
        current_image_slots(backend, slots);
        emit_local(backend, packet.opcode, slots, sizeof(slots));
        return 0;
    }
    if (packet.opcode == 0x11U && packet.payload_length == 1U &&
        (packet.payload[0] == 3U || packet.payload[0] == 5U)) {
        uint8_t mode = packet.payload[0];
        if (backend->thermal_main_set(backend->thermal_main_opaque, false) < 0 ||
            select_image_lens(backend, mode == 5U) < 0) mode = 0xffU;
        emit_local(backend, packet.opcode, &mode, sizeof(mode));
        return 0;
    }
    if (packet.opcode == 0x16U && packet.payload_length == 0U) {
        static const uint8_t maximum[] = {10U, 0U};
        emit_local(backend, packet.opcode, maximum, sizeof(maximum));
        return 0;
    }
    if (packet.opcode == 0x18U && packet.payload_length == 0U) {
        emit_zoom_value(backend, packet.opcode);
        return 0;
    }
    if (packet.opcode == 0x0fU && packet.payload_length == 2U) {
        float zoom = (float)packet.payload[0] + (float)packet.payload[1] * 0.1f;
        uint8_t status = set_zoom(backend, zoom) == 0 ? 1U : 0U;
        emit_local(backend, packet.opcode, &status, sizeof(status));
        return 0;
    }
    if (packet.opcode == 0x05U && packet.payload_length == 1U) {
        int direction = (int)(int8_t)packet.payload[0];
        float zoom = backend->zoom_get(backend->zoom_opaque);
        if (direction == 1) zoom += 0.1f;
        else if (direction == -1) zoom -= 0.1f;
        else if (direction != 0) {
            errno = EINVAL;
            return -1;
        }
        if (zoom < 1.0f) zoom = 1.0f;
        if (zoom > 10.0f) zoom = 10.0f;
        if (set_zoom(backend, zoom) < 0) return -1;
        unsigned tenths = (unsigned)(zoom * 10.0f + 0.5f);
        uint8_t response[2] = {
            (uint8_t)tenths,
            (uint8_t)(tenths >> 8),
        };
        emit_local(backend, packet.opcode, response, sizeof(response));
        return 0;
    }
    /* Never send an unhandled delegated request back for the MCU to forward again. */
    if (backend->reply_to_mcu) { errno = ENOTSUP; return -1; }
    if (packet.opcode == 0x0cU && packet.payload_length == 1U &&
        packet.payload[0] >= 3U && packet.payload[0] <= 5U) {
        backend->gimbal_mode = (uint8_t)(packet.payload[0] - 3U);
    } else {
        const char *feature = unsupported_linux_feature(&packet);
        if (feature != NULL) log_unsupported(backend, packet.opcode, feature);
    }
    ca_binlog_vendor(packet.opcode, packet.payload, packet.payload_length);
    ca_angle_target_invalidate_siyi(&backend->angle_target, packet.opcode, packet.payload, packet.payload_length);
    return send_private(backend, 0x08, MT11_TUNNEL, packet_data,
                        (uint16_t)length);
}

int ca_backend_write_external_uart(struct ca_backend *backend, const uint8_t *data, size_t length)
{
    while (length) {
        size_t count = length > 255U ? 255U : length;
        if (send_private(backend, 0x0a, 0x35, data, (uint16_t)count) < 0) return -1;
        data += count;
        length -= count;
    }
    return 0;
}

int ca_backend_handle_private(ca_backend *backend, const ca_private_frame *frame)
{
    if (!backend || !frame || frame->destination != MT11_MCU || frame->link != 0x11 ||
        !frame->raw || frame->raw_length > CA_PRIVATE_MAX_FRAME) {
        errno = EINVAL;
        return -1;
    }
    // Private gimbal commands may change the target without passing through
    // the public SDK; do not retain an absolute-angle de-duplication cache.
    backend->angle_target.valid = false;
    const int result = backend->datagram_transport
        ? write_datagram(backend->uart_fd, frame->raw, frame->raw_length)
        : write_all(backend->uart_fd, frame->raw, frame->raw_length);
    ca_binlog_packet(true, CA_PACKET_MT11,
        backend->datagram_transport ? CA_PACKET_MCU_UDP : CA_PACKET_MCU_UART,
        0, 0, frame->raw, frame->raw_length, result < 0 ? -errno : 0);
    return result;
}

void ca_backend_periodic(struct ca_backend *backend)
{
    uint64_t now;

    if (backend == NULL || !backend->thermal_continuous) return;
    now = monotonic_ms();
    if (backend->thermal_last_emit_ms == 0U ||
        now - backend->thermal_last_emit_ms >= THERMAL_RANGE_INTERVAL_MS) {
        (void)emit_thermal_range(backend);
    }
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
    length = ca_siyi_build(packet, sizeof(packet), 1,
                           backend->public_sequence++, opcode, payload,
                           payload_length);
    if (length == 0U) {
        errno = EMSGSIZE;
        return -1;
    }
    ca_angle_target_invalidate_siyi(&backend->angle_target, opcode, payload, payload_length);
    return send_private(backend, 0x08, MT11_TUNNEL, packet, (uint16_t)length);
}

int ca_backend_request_gimbal_attitude(struct ca_backend *backend)
{
    return send_public_command(backend, 0x0dU, NULL, 0U);
}

bool ca_backend_gimbal_attitude(const struct ca_backend *backend,
                                struct ca_gimbal_attitude *attitude)
{
    if (backend == NULL || attitude == NULL || !backend->have_attitude) {
        return false;
    }
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
    float angles[3] = {0, pitch_rad * (180.0f / PI_F), yaw_rad * (180.0f / PI_F)};
    angles[1] = fminf(APCAM_GIMBAL_PITCH_MAX, fmaxf(APCAM_GIMBAL_PITCH_MIN, angles[1]));
    angles[2] = fminf(APCAM_GIMBAL_YAW_MAX, fmaxf(APCAM_GIMBAL_YAW_MIN, angles[2]));
    float target_pitch = angles[1], target_yaw = angles[2];
    apcam_transform(&apcam_angle_command[backend->mounting_direction == 2U], angles, angles, false);
    int16_t yaw = angle_tenths(angles[2] * (PI_F / 180.0f), -180.0f, 180.0f);
    int16_t pitch = angle_tenths(angles[1] * (PI_F / 180.0f), -180.0f, 180.0f);
    uint64_t now = monotonic_ms();
    if (ca_angle_target_suppress(&backend->angle_target, yaw, pitch,
                                 target_yaw, target_pitch,
                                 backend->have_attitude ? &backend->attitude : NULL, now)) return 0;
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
    float rates[3] = {0, pitch_rate_rad_s, yaw_rate_rad_s};
    apcam_transform(&apcam_rate_command[backend->mounting_direction == 2U], rates, rates, true);
    uint8_t payload[2] = {
        (uint8_t)rate_byte(rates[2], APCAM_VENDOR_YAW_RATE_FULL_SCALE),
        (uint8_t)rate_byte(rates[1], APCAM_VENDOR_PITCH_RATE_FULL_SCALE),
    };
    int result = send_public_command(backend, 0x07U, payload, sizeof(payload));
    CA_BINLOG(CA_LOG_GCMD, ca_log_gcmd, .mode=2,
        .pitch=pitch_rate_rad_s*57.295779513f, .yaw=yaw_rate_rad_s*57.295779513f,
        .wirep=(float)(int8_t)payload[1], .wirey=(float)(int8_t)payload[0], .result=result);
    return result;
}

int ca_backend_set_gimbal_neutral(struct ca_backend *backend)
{
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
    return "mt11";
}

void ca_backend_close(struct ca_backend *backend)
{
    static const uint8_t shutdown_payload[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00};

    if (backend == NULL) return;
    if (backend->uart_fd >= 0) {
        (void)send_private(backend, 0x08, 0x36, shutdown_payload,
                           sizeof(shutdown_payload));
        (void)tcdrain(backend->uart_fd);
        close(backend->uart_fd);
    }
    free(backend);
}

int ca_backend_set_zoom(struct ca_backend *backend, float ratio)
{
    if (!backend || !isfinite(ratio) || ratio < 1 || ratio > APCAM_ZOOM_CONTROL_MAX) { errno = EINVAL; return -1; }
    return backend->zoom_set(backend->zoom_opaque, ratio);
}
int ca_backend_set_zoom_rate(struct ca_backend *backend, float rate)
{
    (void)backend; (void)rate; errno = ENOTSUP; return -1;
}
