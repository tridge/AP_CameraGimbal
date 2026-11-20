#include <new>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/support_proxy.h"
#include "camera_app/log.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_SIZE 128U
struct message_queue {
    mavlink_message_t messages[QUEUE_SIZE];
    unsigned first, count;
};

struct ca_support_mavlink {
    struct ca_support_config config;
    pthread_t thread;
    pthread_mutex_t lock;
    atomic_bool stop;
    struct message_queue outgoing, incoming;
    struct ca_mavlink_parser parser;
    mavlink_signing_t signing;
    mavlink_signing_streams_t signing_streams;
};

static bool pop(struct message_queue *queue, mavlink_message_t *message)
{
    if (!queue->count) return false;
    *message = queue->messages[queue->first];
    queue->first = (queue->first + 1U) % QUEUE_SIZE;
    queue->count--;
    return true;
}

static void push(struct message_queue *queue, const mavlink_message_t *message)
{
    if (queue->count == QUEUE_SIZE) {
        queue->first = (queue->first + 1U) % QUEUE_SIZE;
        queue->count--;
    }
    queue->messages[(queue->first + queue->count) % QUEUE_SIZE] = *message;
    queue->count++;
}

static uint64_t signing_time(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0 || now.tv_sec < 1420070400) return 1U;
    return (uint64_t)(now.tv_sec - 1420070400) * 100000U + now.tv_nsec / 10000U;
}

static int connect_proxy(const struct ca_support_config *config)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", config->mavlink_port);
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(config->host, port, &hints, &addresses) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = addresses; a != NULL; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        if (fd >= 0) close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

static void *relay(void *opaque)
{
    struct ca_support_mavlink *proxy = (struct ca_support_mavlink*)(opaque);
    int fd = -1;
    unsigned retry = 0;
    while (!atomic_load(&proxy->stop)) {
        if (fd < 0) {
            if (retry) { usleep(50000); retry--; continue; }
            fd = connect_proxy(&proxy->config);
            if (fd < 0) { retry = 40; continue; }
            ca_log("SupportProxy MAVLink UDP connected to %s:%u signing=%s",
                   proxy->config.host, proxy->config.mavlink_port,
                   proxy->config.signing ? "enabled" : "disabled");
        }
        for (unsigned i = 0; i < QUEUE_SIZE; i++) {
            mavlink_message_t message;
            pthread_mutex_lock(&proxy->lock);
            bool have = pop(&proxy->outgoing, &message);
            pthread_mutex_unlock(&proxy->lock);
            if (!have) break;
            uint64_t timestamp = signing_time();
            if (timestamp > proxy->signing.timestamp) proxy->signing.timestamp = timestamp;
            // Re-sign for this link without changing the vehicle's identity or
            // sequence number. The local flight controller needs no shared key.
            mavlink_status_t status = {};
            status.current_tx_seq = message.seq;
            if (proxy->config.signing) status.signing = &proxy->signing;
            message.incompat_flags = 0U;
            ca_mavlink_restamp(&message, &status);
            uint8_t packet[MAVLINK_MAX_PACKET_LEN];
            size_t length = ca_mavlink_to_wire(packet, sizeof(packet), &message);
            if (send(fd, packet, length, MSG_NOSIGNAL) < 0 &&
                errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(fd); fd = -1; retry = 40; break;
            }
        }
        if (fd < 0) continue;
        struct pollfd event = {.fd = fd, .events = POLLIN};
        if (poll(&event, 1, 20) <= 0) continue;
        uint8_t packet[4096];
        ssize_t count = recv(fd, packet, sizeof(packet), 0);
        if (count < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(fd); fd = -1; retry = 40;
            }
            continue;
        }
        for (ssize_t i = 0; i < count; i++) {
            mavlink_message_t message;
            if (ca_mavlink_parse_byte(&proxy->parser, packet[i], &message) != 1) continue;
            // A verified proxy signature authenticates this hop only. Strip it
            // before passing commands to a flight controller on the local link.
            mavlink_status_t status = {};
            status.current_tx_seq = message.seq;
            message.incompat_flags = 0U;
            ca_mavlink_restamp(&message, &status);
            pthread_mutex_lock(&proxy->lock);
            push(&proxy->incoming, &message);
            pthread_mutex_unlock(&proxy->lock);
        }
    }
    if (fd >= 0) close(fd);
    return NULL;
}

int ca_support_mavlink_open(struct ca_support_mavlink **result,
                            const struct ca_support_config *config)
{
    *result = NULL;
    if (!config->enabled || !config->mavlink_port) return 0;
    struct ca_support_mavlink *proxy = new (std::nothrow) ca_support_mavlink{};
    if (!proxy) return -1;
    proxy->config = *config;
    atomic_init(&proxy->stop, false);
    ca_mavlink_parser_init(&proxy->parser);
    if (config->signing) {
        ca_mavlink_signing_key(config->signing_passphrase, proxy->signing.secret_key);
        proxy->signing.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING;
        proxy->signing.link_id = config->signing_link_id;
        proxy->signing.timestamp = signing_time();
        proxy->parser.status.signing = &proxy->signing;
        proxy->parser.status.signing_streams = &proxy->signing_streams;
    }
    int error = pthread_mutex_init(&proxy->lock, NULL);
    if (error) { delete proxy; errno = error; return -1; }
    error = pthread_create(&proxy->thread, NULL, relay, proxy);
    if (error) { pthread_mutex_destroy(&proxy->lock); delete proxy; errno = error; return -1; }
    *result = proxy;
    return 0;
}

void ca_support_mavlink_send(struct ca_support_mavlink *proxy,
                             const mavlink_message_t *message)
{
    if (!proxy) return;
    pthread_mutex_lock(&proxy->lock);
    push(&proxy->outgoing, message);
    pthread_mutex_unlock(&proxy->lock);
}

bool ca_support_mavlink_receive(struct ca_support_mavlink *proxy,
                                mavlink_message_t *message)
{
    if (!proxy) return false;
    pthread_mutex_lock(&proxy->lock);
    bool have = pop(&proxy->incoming, message);
    pthread_mutex_unlock(&proxy->lock);
    return have;
}

void ca_support_mavlink_close(struct ca_support_mavlink *proxy)
{
    if (!proxy) return;
    atomic_store(&proxy->stop, true);
    pthread_join(proxy->thread, NULL);
    pthread_mutex_destroy(&proxy->lock);
    delete proxy;
}
