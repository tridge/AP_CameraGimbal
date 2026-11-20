#ifndef CAMERA_APP_UDP_TRANSPORT_H
#define CAMERA_APP_UDP_TRANSPORT_H
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
/* Parses "IPv4:PORT" or "localhost:PORT" (the part after udp:// or tcp://). */
static inline int ca_parse_endpoint(const char *address, struct sockaddr_in *peer)
{
    const char *separator = strrchr(address, ':');
    char host[256];
    char *end;
    unsigned long port;

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
    memset(peer, 0, sizeof(*peer));
    peer->sin_family = AF_INET;
    peer->sin_port = htons((uint16_t)port);
    if (strcmp(host, "localhost") == 0) {
        peer->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (inet_pton(AF_INET, host, &peer->sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}
static inline int ca_open_udp_transport(const char *device)
{
    struct sockaddr_in peer;
    int fd;

    if (ca_parse_endpoint(device + strlen("udp://"), &peer) < 0)
        return -1;
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
#endif
