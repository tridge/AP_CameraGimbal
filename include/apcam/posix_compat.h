#pragma once

// macOS SITL equivalents for Linux clocks, file sync and descriptor flags.
// Hardware builds use their native APIs. Preserve nonblocking/CLOEXEC on hosts.
#ifdef __APPLE__
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

static inline int fdatasync(int fd) { return fsync(fd); }

#define TIMER_ABSTIME 1
static inline int clock_nanosleep(clockid_t clock, int flags, const struct timespec *deadline, struct timespec *remaining)
{
    if (flags != TIMER_ABSTIME) return nanosleep(deadline, remaining) < 0 ? errno : 0;
    struct timespec now;
    if (clock_gettime(clock, &now) < 0) return errno;
    struct timespec delay = {deadline->tv_sec - now.tv_sec, deadline->tv_nsec - now.tv_nsec};
    if (delay.tv_nsec < 0) { delay.tv_sec--; delay.tv_nsec += 1000000000; }
    if (delay.tv_sec < 0) return 0;
    return nanosleep(&delay, remaining) < 0 ? errno : 0;
}

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x10000000
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0x20000000
#endif

static inline int apcam_fd_flags(int fd, bool nonblock, bool cloexec)
{
    if (fd < 0) return -1;
    const int current = nonblock ? fcntl(fd, F_GETFL) : 0;
    if (current < 0 || (nonblock && fcntl(fd, F_SETFL, current | O_NONBLOCK) < 0) ||
        (cloexec && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
static inline int apcam_socket(int domain, int type, int protocol)
{
    return apcam_fd_flags(socket(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol),
                          type & SOCK_NONBLOCK, type & SOCK_CLOEXEC);
}
static inline int apcam_socketpair(int domain, int type, int protocol, int pair[2])
{
    if (socketpair(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol, pair) < 0) return -1;
    if (apcam_fd_flags(pair[0], type & SOCK_NONBLOCK, type & SOCK_CLOEXEC) < 0) {
        int saved = errno; close(pair[1]); errno = saved; return -1;
    }
    if (apcam_fd_flags(pair[1], type & SOCK_NONBLOCK, type & SOCK_CLOEXEC) < 0) {
        int saved = errno; close(pair[0]); errno = saved; return -1;
    }
    return 0;
}
static inline int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags)
{
    return apcam_fd_flags(accept(fd, address, length), flags & SOCK_NONBLOCK, flags & SOCK_CLOEXEC);
}
static inline int pipe2(int pair[2], int flags)
{
    if (pipe(pair) < 0) return -1;
    if (apcam_fd_flags(pair[0], flags & O_NONBLOCK, flags & O_CLOEXEC) < 0) {
        int saved = errno; close(pair[1]); errno = saved; return -1;
    }
    if (apcam_fd_flags(pair[1], flags & O_NONBLOCK, flags & O_CLOEXEC) < 0) {
        int saved = errno; close(pair[0]); errno = saved; return -1;
    }
    return 0;
}
#define socket apcam_socket
#define socketpair apcam_socketpair
#endif
