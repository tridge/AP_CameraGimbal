#ifndef CAMERA_APP_EVENT_POLL_H
#define CAMERA_APP_EVENT_POLL_H
/* epoll on Linux; level-triggered poll on the bundled Windows POSIX runtime.
 * A poll set without a pollable descriptor is serviced by the main loop tick. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__CYGWIN__) || defined(__APPLE__)
#include <poll.h>
#define CA_POLL_IN POLLIN
#define CA_POLL_OUT POLLOUT
#define CA_POLL_ERR POLLERR
#define CA_POLL_HUP POLLHUP
#define CA_POLL_RDHUP POLLHUP
#define CA_POLL_ADD 1
#define CA_POLL_MOD 2
#define CA_POLL_DEL 3
#define CA_POLL_CAPACITY 128
union ca_poll_data { void *ptr; int fd; uint32_t u32; uint64_t u64; };
typedef struct { uint32_t events; union ca_poll_data data; } ca_poll_event;
struct ca_pollset {
    struct pollfd fds[CA_POLL_CAPACITY];
    union ca_poll_data data[CA_POLL_CAPACITY];
    unsigned count;
};
static inline struct ca_pollset *ca_poll_open(void) { return (struct ca_pollset *)calloc(1, sizeof(struct ca_pollset)); }
static inline int ca_poll_fd(const struct ca_pollset *set) { (void)set; return -1; }
static inline void ca_poll_close(struct ca_pollset *set) { free(set); }
static inline int ca_poll_change(struct ca_pollset *set, int op, int fd, const ca_poll_event *event)
{
    unsigned i;
    for (i = 0; i < set->count && set->fds[i].fd != fd; i++) {}
    if (op == CA_POLL_ADD) {
        if (i < set->count) { errno = EEXIST; return -1; }
        if (i == CA_POLL_CAPACITY) { errno = ENOSPC; return -1; }
        set->count++;
    } else if (i == set->count) { errno = ENOENT; return -1; }
    if (op == CA_POLL_DEL) {
        set->count--;
        set->fds[i] = set->fds[set->count];
        set->data[i] = set->data[set->count];
    } else {
        set->fds[i] = (struct pollfd){.fd = fd, .events = (short)event->events};
        set->data[i] = event->data;
    }
    return 0;
}
static inline int ca_poll_wait(struct ca_pollset *set, ca_poll_event *events, int maximum, int timeout)
{
    int ready = poll(set->fds, set->count, timeout), count = 0;
    if (ready <= 0) return ready;
    for (unsigned i = 0; i < set->count && count < maximum; i++) {
        if (set->fds[i].revents) {
            events[count++] = (ca_poll_event){.events = (uint32_t)set->fds[i].revents, .data = set->data[i]};
        }
    }
    return count;
}
#else
#include <sys/epoll.h>
#define CA_POLL_IN EPOLLIN
#define CA_POLL_OUT EPOLLOUT
#define CA_POLL_ERR EPOLLERR
#define CA_POLL_HUP EPOLLHUP
#define CA_POLL_RDHUP EPOLLRDHUP
#define CA_POLL_ADD EPOLL_CTL_ADD
#define CA_POLL_MOD EPOLL_CTL_MOD
#define CA_POLL_DEL EPOLL_CTL_DEL
typedef struct epoll_event ca_poll_event;
struct ca_pollset { int fd; };
static inline struct ca_pollset *ca_poll_open(void)
{
    struct ca_pollset *set = (struct ca_pollset*)(malloc(sizeof(*set)));
    if (!set) return NULL;
    set->fd = epoll_create1(EPOLL_CLOEXEC);
    if (set->fd < 0) { free(set); return NULL; }
    return set;
}
static inline int ca_poll_fd(const struct ca_pollset *set) { return set ? set->fd : -1; }
static inline void ca_poll_close(struct ca_pollset *set)
{
    if (set) { close(set->fd); free(set); }
}
static inline int ca_poll_change(struct ca_pollset *set, int op, int fd, ca_poll_event *event)
{ return epoll_ctl(set->fd, op, fd, event); }
static inline int ca_poll_wait(struct ca_pollset *set, ca_poll_event *events, int maximum, int timeout)
{ return epoll_wait(set->fd, events, maximum, timeout); }
#endif
#endif
