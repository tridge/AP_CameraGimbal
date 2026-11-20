#pragma once
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <poll.h>
#include <time.h>

namespace apcam {
inline int64_t probe_milliseconds()
{
    timespec t{}; if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return int64_t(t.tv_sec)*1000+t.tv_nsec/1000000;
}
// Private live_video_server protocol: stream byte, status byte, then fMP4.
// Require two complete nonempty media fragments, not merely an open socket or
// cached initialization. This is liveness; decoding is tested independently.
inline bool live_video_probe(unsigned port, unsigned stream, unsigned milliseconds = 8000)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    int64_t deadline = probe_milliseconds()+milliseconds;
    auto wait = [&](short events) {
        int64_t left = deadline-probe_milliseconds();
        if (left <= 0) return false;
        pollfd p{fd, events, 0}; int n;
        do {n = poll(&p, 1, int(left));} while (n < 0 && errno == EINTR &&
            (left = deadline-probe_milliseconds()) > 0);
        return n == 1 && (p.revents & events);
    };
    auto read = [&](void *output, size_t length) {
        auto *p = static_cast<uint8_t *>(output); size_t used = 0;
        while (used < length) {
            if (!wait(POLLIN)) return false;
            ssize_t n = recv(fd, p+used, length-used, 0);
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) return false;
            used += size_t(n);
        }
        return true;
    };
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(uint16_t(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    if (!ok && errno == EINPROGRESS && wait(POLLOUT)) {
        int error = 0; socklen_t size = sizeof(error);
        ok = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && !error;
    }
    uint8_t status = uint8_t(stream);
    ok = ok && wait(POLLOUT) && send(fd, &status, 1, MSG_NOSIGNAL) == 1 && read(&status, 1) && !status;
    bool initialization = false, fragment = false; unsigned frames = 0; uint64_t bytes = 0;
    for (unsigned box = 0; ok && frames < 2 && box < 64; ++box) {
        uint8_t header[8]; ok = read(header, sizeof(header)); if (!ok) break;
        uint32_t size = (uint32_t(header[0])<<24)|(uint32_t(header[1])<<16)|(uint32_t(header[2])<<8)|header[3];
        if (size < 8 || size > 4*1024*1024 || (bytes += size) > 8*1024*1024) {ok = false; break;}
        uint8_t data[16384]; uint32_t left = size-8;
        while (ok && left) {unsigned n = left < sizeof(data) ? left : unsigned(sizeof(data)); ok = read(data, n); left -= n;}
        if (!ok) break;
        if (!memcmp(header+4, "moov", 4)) initialization = true;
        if (!memcmp(header+4, "moof", 4)) fragment = initialization;
        if (!memcmp(header+4, "mdat", 4) && size > 8 && fragment) {++frames; fragment = false;}
    }
    close(fd); return ok && frames == 2;
}
} // namespace apcam
