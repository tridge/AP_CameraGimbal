#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/siyi_server.h"

#include "camera_app/external_uart.h"
#include "camera_app/binlog.h"
#include "camera_app/log.h"
#include "camera_app/siyi.h"

#include <arpa/inet.h>
#ifdef CAMERA_APP_ZT30_RAM
#include "camera_app/zt30_ram_peer.h"
#endif
#include <errno.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_app/event_poll.h"
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CA_SIYI_TCP_CLIENTS 8U
#define CA_SIYI_STREAM_BUFFER (CA_SIYI_MAX_PACKET * 2U)
#define CA_SIYI_OUTPUT_BUFFER (CA_SIYI_MAX_PACKET * 4U)
#define CA_SIYI_PENDING 64U
#define CA_SIYI_PENDING_MS 2000U
#define CA_BIND_RETRY_ATTEMPTS 50U
#define CA_BIND_RETRY_DELAY_US 100000U
#define CA_EVENT_UDP UINT64_C(1)
#define CA_EVENT_LISTENER UINT64_C(2)
#define CA_EVENT_UART UINT64_C(3)
#define CA_EVENT_CLIENT UINT64_C(0x100)

enum route_kind {
    ROUTE_NONE,
    ROUTE_UDP,
    ROUTE_TCP,
    ROUTE_UART,
};

struct route {
    enum route_kind kind;
    struct sockaddr_storage address;
    socklen_t address_length;
    unsigned slot;
    uint32_t generation;
};

struct tcp_client {
    int fd;
    struct sockaddr_in peer;
    uint32_t generation;
    uint8_t input[CA_SIYI_STREAM_BUFFER];
    size_t input_length;
    uint8_t output[CA_SIYI_OUTPUT_BUFFER];
    size_t output_offset;
    size_t output_length;
};

struct pending_route {
    bool used;
    uint8_t opcode;
    struct route route;
    uint64_t order;
    uint64_t deadline_ms;
};

struct ca_siyi_server {
    struct ca_pollset *pollset;
    int udp_fd;
    int listener_fd;
    int uart_fd;
    uint8_t uart_input[CA_SIYI_STREAM_BUFFER];
    size_t uart_input_length;
    uint8_t uart_output[CA_SIYI_OUTPUT_BUFFER];
    size_t uart_output_offset;
    size_t uart_output_length;
    struct tcp_client clients[CA_SIYI_TCP_CLIENTS];
    struct route active;
    bool active_valid;
    bool active_emitted;
    struct route last[256];
    bool have_last[256];
    struct route last_udp;
    bool have_last_udp;
    struct pending_route pending[CA_SIYI_PENDING];
    uint64_t pending_order;
};

static uint16_t get_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return 0U;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static uint64_t client_event(unsigned slot, uint32_t generation)
{
    return CA_EVENT_CLIENT | slot | ((uint64_t)generation << 16U);
}

static int bind_socket(int type, unsigned port, const char *name)
{
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port), .sin_addr = { .s_addr = htonl(INADDR_ANY) },
    };

    for (unsigned attempt = 0; attempt < CA_BIND_RETRY_ATTEMPTS; attempt++) {
        int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        int one = 1;
        if (fd < 0) return -1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            return fd;
        }
        int saved_errno = errno;
        close(fd);
        if (saved_errno != EADDRINUSE ||
            attempt + 1U == CA_BIND_RETRY_ATTEMPTS) {
            errno = saved_errno;
            return -1;
        }
        if (attempt == 0U) {
            ca_log("%s %u is still busy; waiting for previous app teardown",
                   name, port);
        }
        (void)usleep(CA_BIND_RETRY_DELAY_US);
    }
    errno = EADDRINUSE;
    return -1;
}

static bool route_valid(const struct ca_siyi_server *server,
                        const struct route *route)
{
    if (route->kind == ROUTE_UDP) return server->udp_fd >= 0;
    if (route->kind == ROUTE_UART) return server->uart_fd >= 0;
    if (route->kind != ROUTE_TCP || route->slot >= CA_SIYI_TCP_CLIENTS) {
        return false;
    }
    const struct tcp_client *client = &server->clients[route->slot];
    return client->fd >= 0 && client->generation == route->generation;
}

static void disable_uart(struct ca_siyi_server *server, int error)
{
    if (server->uart_fd < 0) return;
    (void)ca_poll_change(server->pollset, CA_POLL_DEL, server->uart_fd, NULL);
    close(server->uart_fd);
    server->uart_fd = -1;
    server->uart_input_length = 0U;
    server->uart_output_offset = 0U;
    server->uart_output_length = 0U;
    ca_log("SIYI UART disabled after I/O failure: %s", strerror(error));
}

static int update_uart_events(struct ca_siyi_server *server)
{
    ca_poll_event event = {
        .events = CA_POLL_IN |
                  (server->uart_output_length != 0U ? CA_POLL_OUT : 0U),
        .data = { .u64 = CA_EVENT_UART },
    };
    return ca_poll_change(server->pollset, CA_POLL_MOD, server->uart_fd, &event);
}

static int flush_uart(struct ca_siyi_server *server)
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

static int queue_uart(struct ca_siyi_server *server, const uint8_t *packet,
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
           packet, length);
    server->uart_output_length += length;
    int result = flush_uart(server);
    if (result < 0) {
        int saved_errno = errno;
        disable_uart(server, saved_errno);
        errno = saved_errno;
    }
    return result;
}

static int update_client_events(struct ca_siyi_server *server, unsigned slot)
{
    struct tcp_client *client = &server->clients[slot];
    ca_poll_event event = {
        .events = CA_POLL_IN | CA_POLL_RDHUP |
                  (client->output_length != 0U ? CA_POLL_OUT : 0U),
        .data = { .u64 = client_event(slot, client->generation) },
    };
    return ca_poll_change(server->pollset, CA_POLL_MOD, client->fd, &event);
}

static void close_client(struct ca_siyi_server *server, unsigned slot)
{
    struct tcp_client *client = &server->clients[slot];
    if (client->fd < 0) return;
    (void)ca_poll_change(server->pollset, CA_POLL_DEL, client->fd, NULL);
    close(client->fd);
    ca_log("SIYI TCP client %u disconnected", slot + 1U);
    client->fd = -1;
    client->input_length = 0U;
    client->output_offset = 0U;
    client->output_length = 0U;
}

static int flush_client(struct ca_siyi_server *server, unsigned slot)
{
    struct tcp_client *client = &server->clients[slot];
    while (client->output_length != 0U) {
        ssize_t sent = send(client->fd, client->output + client->output_offset,
                            client->output_length, MSG_NOSIGNAL);
        if (sent > 0) {
            client->output_offset += (size_t)sent;
            client->output_length -= (size_t)sent;
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1;
    }
    if (client->output_length == 0U) client->output_offset = 0U;
    return update_client_events(server, slot);
}

static int queue_client(struct ca_siyi_server *server, unsigned slot,
                        const uint8_t *packet, size_t length)
{
    struct tcp_client *client = &server->clients[slot];
    if (length > sizeof(client->output) - client->output_length) {
        errno = ENOBUFS;
        return -1;
    }
    if (client->output_offset != 0U &&
        client->output_offset + client->output_length + length >
            sizeof(client->output)) {
        memmove(client->output, client->output + client->output_offset,
                client->output_length);
        client->output_offset = 0U;
    }
    memcpy(client->output + client->output_offset + client->output_length,
           packet, length);
    client->output_length += length;
    return flush_client(server, slot);
}

static void log_packet(const ca_siyi_server *server, const route *route,
                       const uint8_t *packet, size_t length, bool outgoing, int result=0)
{
    const sockaddr_in *peer = nullptr;
    uint8_t link=CA_PACKET_UART;
    if (route->kind == ROUTE_UDP && route->address.ss_family == AF_INET) {
        link=CA_PACKET_UDP;
        peer=reinterpret_cast<const sockaddr_in *>(&route->address);
    } else if (route->kind == ROUTE_TCP) {
        link=CA_PACKET_TCP;
        if (route->slot < CA_SIYI_TCP_CLIENTS && server->clients[route->slot].generation == route->generation)
            peer=&server->clients[route->slot].peer;
    }
    ca_binlog_packet(outgoing, CA_PACKET_SIYI, link, peer ? ntohl(peer->sin_addr.s_addr) : 0,
                     peer ? ntohs(peer->sin_port) : 0, packet, length, result);
}

static int send_route_unlogged(struct ca_siyi_server *server, const struct route *route,
                      const uint8_t *packet, size_t length)
{
    if (!route_valid(server, route)) {
        errno = ENOTCONN;
        return -1;
    }
    if (route->kind == ROUTE_UDP) {
        ssize_t sent = sendto(server->udp_fd, packet, length, 0,
                              (const struct sockaddr *)&route->address,
                              route->address_length);
        if (sent == (ssize_t)length) return 0;
        if (sent >= 0) errno = EIO;
        return -1;
    }
    if (route->kind == ROUTE_UART) {
        return queue_uart(server, packet, length);
    }
    return queue_client(server, route->slot, packet, length);
}

static int send_route(struct ca_siyi_server *server, const struct route *route,
                      const uint8_t *packet, size_t length)
{
    const int result=send_route_unlogged(server, route, packet, length);
    log_packet(server, route, packet, length, true, result < 0 ? -errno : 0);
    return result;
}

static void purge_pending(struct ca_siyi_server *server, uint64_t now)
{
    for (unsigned i = 0; i < CA_SIYI_PENDING; i++) {
        if (server->pending[i].used && server->pending[i].deadline_ms <= now) {
            server->pending[i].used = false;
        }
    }
}

static void add_pending(struct ca_siyi_server *server, uint8_t opcode,
                        const struct route *route)
{
    unsigned selected = CA_SIYI_PENDING;
    uint64_t oldest = UINT64_MAX;
    uint64_t now = monotonic_ms();
    purge_pending(server, now);
    for (unsigned i = 0; i < CA_SIYI_PENDING; i++) {
        if (!server->pending[i].used) {
            selected = i;
            break;
        }
        if (server->pending[i].order < oldest) {
            oldest = server->pending[i].order;
            selected = i;
        }
    }
    server->pending[selected] = (struct pending_route) {
        .used = true,
        .opcode = opcode,
        .route = *route,
        .order = ++server->pending_order,
        .deadline_ms = now + CA_SIYI_PENDING_MS,
    };
}

static bool take_pending(struct ca_siyi_server *server, uint8_t opcode,
                         struct route *route)
{
    unsigned selected = CA_SIYI_PENDING;
    uint64_t oldest = UINT64_MAX;
    purge_pending(server, monotonic_ms());
    for (unsigned i = 0; i < CA_SIYI_PENDING; i++) {
        if (server->pending[i].used && server->pending[i].opcode == opcode &&
            server->pending[i].order < oldest &&
            route_valid(server, &server->pending[i].route)) {
            oldest = server->pending[i].order;
            selected = i;
        }
    }
    if (selected == CA_SIYI_PENDING) return false;
    *route = server->pending[selected].route;
    server->pending[selected].used = false;
    return true;
}

static void broadcast_unmatched(struct ca_siyi_server *server,
                                const uint8_t *packet, size_t length)
{
    for (unsigned i = 0; i < CA_SIYI_TCP_CLIENTS; i++) {
        if (server->clients[i].fd >= 0 &&
            queue_client(server, i, packet, length) < 0) {
            ca_log("SIYI TCP client %u reply failed: %s", i + 1U,
                   strerror(errno));
            close_client(server, i);
        }
    }
    if (server->have_last_udp &&
        send_route(server, &server->last_udp, packet, length) < 0) {
        ca_log("SIYI UDP unsolicited reply failed: %s", strerror(errno));
    }
    if (server->uart_fd >= 0) {
        const struct route route = {.kind = ROUTE_UART};
        if (send_route(server, &route, packet, length) < 0) {
            ca_log("SIYI UART unsolicited reply failed: %s", strerror(errno));
        }
    }
}

void ca_siyi_server_emit(void *opaque, const uint8_t *packet, size_t length)
{
    struct ca_siyi_server *server = (struct ca_siyi_server*)(opaque);
    struct ca_siyi_packet parsed;
    struct route route;
    size_t consumed;
    bool have_route = false;

    if (server == NULL || packet == NULL || length == 0U) return;
    if (server->active_valid) {
        route = server->active;
        server->active_emitted = true;
        have_route = true;
    } else if (ca_siyi_parse_one(packet, length, &parsed, &consumed) == 1 &&
               consumed == length) {
        if (take_pending(server, parsed.opcode, &route)) {
            have_route = true;
        } else if (server->have_last[parsed.opcode] &&
                   route_valid(server, &server->last[parsed.opcode])) {
            route = server->last[parsed.opcode];
            have_route = true;
        }
    }
    if (have_route) {
        if (send_route(server, &route, packet, length) < 0) {
            ca_log("SIYI routed reply failed: %s", strerror(errno));
            if (route.kind == ROUTE_TCP &&
                route.slot < CA_SIYI_TCP_CLIENTS) {
                close_client(server, route.slot);
            }
        }
        return;
    }
    broadcast_unmatched(server, packet, length);
}

static int dispatch_packet(struct ca_siyi_server *server,
                           const struct route *route, const uint8_t *packet,
                           size_t length, ca_siyi_request_fn request,
                           void *opaque)
{
    struct ca_siyi_packet parsed;
    size_t consumed;
    if (ca_siyi_parse_one(packet, length, &parsed, &consumed) != 1 ||
        consumed != length) {
        errno = EPROTO;
        return -1;
    }
    /* The vendor TCP service expects this one-byte keepalive.  It is a
     * transport heartbeat rather than a camera/MCU command. */
    log_packet(server, route, packet, length, false);
    if (route->kind == ROUTE_TCP && parsed.opcode == 0x00U &&
        parsed.payload_length == 1U && parsed.payload[0] == 0U) {
        return 0;
    }
    server->last[parsed.opcode] = *route;
    server->have_last[parsed.opcode] = true;
    if (route->kind == ROUTE_UDP) {
        server->last_udp = *route;
        server->have_last_udp = true;
    }
    server->active = *route;
    server->active_valid = true;
    server->active_emitted = false;
    int result = request(opaque, packet, length);
    bool emitted = server->active_emitted;
    server->active_valid = false;
    if (!emitted) add_pending(server, parsed.opcode, route);
    return result;
}

static int process_stream(struct ca_siyi_server *server, uint8_t *input,
                          size_t *input_length, const struct route *route,
                          const char *name, ca_siyi_request_fn request,
                          void *opaque)
{
    while (*input_length != 0U) {
        size_t start = 0U;
        while (start + 1U < *input_length &&
               (input[start] != 0x55U || input[start + 1U] != 0x66U)) {
            start++;
        }
        if (start != 0U) {
            memmove(input, input + start, *input_length - start);
            *input_length -= start;
        }
        if (*input_length < CA_SIYI_MIN_PACKET) return 0;
        size_t total = (size_t)get_u16_le(input + 3U) +
                       CA_SIYI_MIN_PACKET;
        if (total > CA_SIYI_MAX_PACKET) {
            ca_log("dropped malformed SIYI %s header", name);
            memmove(input, input + 1U, --*input_length);
            continue;
        }
        if (*input_length < total) return 0;
        if (dispatch_packet(server, route, input, total, request,
                            opaque) < 0 && errno == EPROTO) {
            ca_log("dropped malformed SIYI %s packet", name);
            memmove(input, input + 1U, --*input_length);
            continue;
        }
        memmove(input, input + total, *input_length - total);
        *input_length -= total;
    }
    return 0;
}

static int read_client(struct ca_siyi_server *server, unsigned slot,
                       ca_siyi_request_fn request, void *opaque)
{
    struct tcp_client *client = &server->clients[slot];
    const struct route route = [&] { struct route value {}; value.kind = ROUTE_TCP; value.slot = slot; value.generation = client->generation; return value; }();
    char name[32];
    snprintf(name, sizeof(name), "TCP client %u", slot + 1U);
    for (;;) {
        if (process_stream(server, client->input, &client->input_length,
                           &route, name, request, opaque) < 0) return -1;
        if (client->input_length == sizeof(client->input)) {
            errno = ENOBUFS;
            return -1;
        }
        ssize_t received = recv(client->fd,
                                client->input + client->input_length,
                                sizeof(client->input) - client->input_length,
                                0);
        if (received > 0) {
            client->input_length += (size_t)received;
            continue;
        }
        if (received == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

static int read_uart(struct ca_siyi_server *server,
                     ca_siyi_request_fn request, void *opaque)
{
    const struct route route = {.kind = ROUTE_UART};
    for (;;) {
        if (process_stream(server, server->uart_input,
                           &server->uart_input_length, &route, "UART",
                           request, opaque) < 0) {
            return -1;
        }
        if (server->uart_input_length == sizeof(server->uart_input)) {
            errno = ENOBUFS;
            return -1;
        }
        ssize_t received = read(server->uart_fd,
                                server->uart_input + server->uart_input_length,
                                sizeof(server->uart_input) -
                                    server->uart_input_length);
        if (received > 0) {
            server->uart_input_length += (size_t)received;
            continue;
        }
        if (received == 0) return 0;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

static int handle_udp(struct ca_siyi_server *server,
                      ca_siyi_request_fn request, void *opaque)
{
    for (;;) {
        uint8_t datagram[4096];
        struct route route = {.kind = ROUTE_UDP};
        route.address_length = sizeof(route.address);
        ssize_t received = recvfrom(server->udp_fd, datagram, sizeof(datagram),
                                    0, (struct sockaddr *)&route.address,
                                    &route.address_length);
        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        #ifdef CAMERA_APP_ZT30_RAM
        if(!ca_zt30_ram_peer(reinterpret_cast<const sockaddr*>(&route.address)))continue;
        #endif
        size_t offset = 0U;
        while (offset < (size_t)received) {
            struct ca_siyi_packet parsed;
            size_t consumed = 0U;
            if (ca_siyi_parse_one(datagram + offset,
                                  (size_t)received - offset, &parsed,
                                  &consumed) != 1) {
                ca_log("dropped malformed SIYI UDP datagram");
                break;
            }
            if (dispatch_packet(server, &route, datagram + offset, consumed,
                                request, opaque) < 0 && errno != EPROTO) {
                /* The request callback owns command-level error logging. */
            }
            offset += consumed;
        }
    }
}

static int accept_clients(struct ca_siyi_server *server)
{
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int fd = accept4(server->listener_fd, (struct sockaddr *)&peer,
                         &peer_length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        #ifdef CAMERA_APP_ZT30_RAM
        if(!ca_zt30_ram_peer(reinterpret_cast<const sockaddr*>(&peer))) { close(fd);continue; }
        #endif
        unsigned slot;
        for (slot = 0; slot < CA_SIYI_TCP_CLIENTS; slot++) {
            if (server->clients[slot].fd < 0) break;
        }
        if (slot == CA_SIYI_TCP_CLIENTS) {
            ca_log("SIYI TCP connection rejected: client limit reached");
            close(fd);
            continue;
        }
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct tcp_client *client = &server->clients[slot];
        client->fd = fd;
        client->peer = peer;
        if (++client->generation == 0U) client->generation = 1U;
        client->input_length = 0U;
        client->output_offset = 0U;
        client->output_length = 0U;
        ca_poll_event event = {
            .events = CA_POLL_IN | CA_POLL_RDHUP,
            .data = { .u64 = client_event(slot, client->generation) },
        };
        if (ca_poll_change(server->pollset, CA_POLL_ADD, fd, &event) < 0) {
            close_client(server, slot);
            return -1;
        }
        char address[INET_ADDRSTRLEN] = "?";
        (void)inet_ntop(AF_INET, &peer.sin_addr, address, sizeof(address));
        ca_log("SIYI TCP client %u connected from %s:%u", slot + 1U,
               address, ntohs(peer.sin_port));
    }
}

int ca_siyi_server_open(struct ca_siyi_server **result, unsigned port,
                        const char *uart_device)
{
    struct ca_siyi_server *server;
    ca_poll_event event;
    if (result == NULL || port == 0U || port > 65535U) {
        errno = EINVAL;
        return -1;
    }
    server = (struct ca_siyi_server*)(calloc(1, sizeof(*server)));
    if (server == NULL) return -1;
    server->pollset = NULL;
    server->udp_fd = -1;
    server->listener_fd = -1;
    server->uart_fd = -1;
    for (unsigned i = 0; i < CA_SIYI_TCP_CLIENTS; i++) {
        server->clients[i].fd = -1;
    }
    server->udp_fd = bind_socket(SOCK_DGRAM, port, "UDP");
    if (server->udp_fd < 0) goto fail;
    server->listener_fd = bind_socket(SOCK_STREAM, port, "TCP");
    if (server->listener_fd < 0 || listen(server->listener_fd, 8) < 0) {
        goto fail;
    }
    server->pollset = ca_poll_open();
    if (server->pollset == NULL) goto fail;
    event = (ca_poll_event) {
        .events = CA_POLL_IN,
        .data = { .u64 = CA_EVENT_UDP },
    };
    if (ca_poll_change(server->pollset, CA_POLL_ADD, server->udp_fd, &event) < 0) {
        goto fail;
    }
    event = (ca_poll_event) {
        .events = CA_POLL_IN,
        .data = { .u64 = CA_EVENT_LISTENER },
    };
    if (ca_poll_change(server->pollset, CA_POLL_ADD, server->listener_fd,
                  &event) < 0) {
        goto fail;
    }
    if (uart_device != NULL) {
        server->uart_fd = ca_external_uart_open(uart_device);
        if (server->uart_fd < 0) {
            ca_log("SIYI UART %s unavailable: %s; continuing without UART",
                   uart_device, strerror(errno));
        } else {
            event = (ca_poll_event) {
                .events = CA_POLL_IN,
                .data = { .u64 = CA_EVENT_UART },
            };
            if (ca_poll_change(server->pollset, CA_POLL_ADD, server->uart_fd,
                          &event) < 0) {
                int saved_errno = errno;
                close(server->uart_fd);
                server->uart_fd = -1;
                ca_log("SIYI UART %s setup failed: %s; continuing without UART",
                       uart_device, strerror(saved_errno));
            } else {
                ca_log("SIYI UART listening on %s at 230400 8N1", uart_device);
            }
        }
    }
    *result = server;
    return 0;
fail:
    {
        int saved_errno = errno;
        ca_siyi_server_close(server);
        errno = saved_errno;
    }
    return -1;
}

int ca_siyi_server_fd(const struct ca_siyi_server *server)
{
    return server != NULL ? ca_poll_fd(server->pollset) : -1;
}

int ca_siyi_server_handle(struct ca_siyi_server *server,
                          ca_siyi_request_fn request, void *opaque)
{
    ca_poll_event events[16];
    int count;
    if (server == NULL || request == NULL) {
        errno = EINVAL;
        return -1;
    }
    do {
        count = ca_poll_wait(server->pollset, events,
                           (int)(sizeof(events) / sizeof(events[0])), 0);
    } while (count < 0 && errno == EINTR);
    if (count < 0) return -1;
    for (int i = 0; i < count; i++) {
        uint64_t token = events[i].data.u64;
        if (token == CA_EVENT_UDP) {
            if (handle_udp(server, request, opaque) < 0) return -1;
            continue;
        }
        if (token == CA_EVENT_LISTENER) {
            if (accept_clients(server) < 0) return -1;
            continue;
        }
        if (token == CA_EVENT_UART) {
            int uart_error = 0;
            if ((events[i].events & CA_POLL_IN) != 0U &&
                read_uart(server, request, opaque) < 0) {
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
            continue;
        }
        if ((token & UINT64_C(0xff00)) != CA_EVENT_CLIENT) continue;
        unsigned slot = (unsigned)(token & UINT64_C(0xff));
        uint32_t generation = (uint32_t)(token >> 16U);
        if (slot >= CA_SIYI_TCP_CLIENTS || server->clients[slot].fd < 0 ||
            server->clients[slot].generation != generation) {
            continue;
        }
        bool failed = false;
        if ((events[i].events & CA_POLL_IN) != 0U &&
            read_client(server, slot, request, opaque) < 0) {
            failed = true;
        }
        if (!failed && server->clients[slot].fd >= 0 &&
            (events[i].events & CA_POLL_OUT) != 0U &&
            flush_client(server, slot) < 0) {
            failed = true;
        }
        if ((events[i].events & (CA_POLL_ERR | CA_POLL_HUP | CA_POLL_RDHUP)) != 0U) {
            failed = true;
        }
        if (failed) {
            close_client(server, slot);
        }
    }
    return 0;
}

void ca_siyi_server_close(struct ca_siyi_server *server)
{
    if (server == NULL) return;
    for (unsigned i = 0; i < CA_SIYI_TCP_CLIENTS; i++) {
        close_client(server, i);
    }
    if (server->listener_fd >= 0) close(server->listener_fd);
    if (server->udp_fd >= 0) close(server->udp_fd);
    if (server->uart_fd >= 0) close(server->uart_fd);
    ca_poll_close(server->pollset);
    free(server);
}
