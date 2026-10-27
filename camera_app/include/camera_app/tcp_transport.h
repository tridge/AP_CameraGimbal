#ifndef CAMERA_APP_TCP_TRANSPORT_H
#define CAMERA_APP_TCP_TRANSPORT_H
#include "camera_app/udp_transport.h"
#include <netinet/in.h>
#include <netinet/tcp.h>
/* Starts a non-blocking connection to tcp://IPv4:PORT. The connection is
 * usually still in progress on return: wait for POLLOUT, then check SO_ERROR. */
static inline int ca_open_tcp_transport(const char *device)
{
    struct sockaddr_in peer;
    int fd, one = 1;

    if (ca_parse_endpoint(device + strlen("tcp://"), &peer) < 0)
        return -1;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (const struct sockaddr *)&peer, sizeof(peer)) < 0 &&
        errno != EINPROGRESS) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    return fd;
}
#endif
