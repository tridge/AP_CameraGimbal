#define _GNU_SOURCE
#include "native.h"
#include "camera_app/log.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int receive_exact(int fd, void *data, size_t size, const atomic_bool *stop)
{
    unsigned char *p = data;
    while (size && !atomic_load(stop)) {
        struct pollfd fds = {.fd = fd, .events = POLLIN};
        int result = poll(&fds, 1, 100);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return -1;
        if (!result) continue;
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        size -= (size_t)n;
    }
    return size ? -1 : 0;
}

int ca_z1_native_receive(const char *helper, const atomic_bool *stop,
                         ca_z1_native_frame_fn publish, ca_z1_native_exposure_fn exposure, void *opaque,
                         struct ca_z1_overlay_control *overlay)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) return -1;
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) max_fd = 1024;
    pid_t child = fork();
    if (child == 0) {
        /* Only async-signal-safe calls between fork and exec in this threaded
         * app. dup2 clears CLOEXEC; handle the rare already-fd-3 case too. */
        if (sockets[1] == 3) {
            int copy = dup(sockets[1]);
            if (copy < 0 || dup2(copy, 3) < 0) _exit(127);
            close(copy);
        } else if (dup2(sockets[1], 3) < 0) {
            _exit(127);
        }
        for (int fd = 4; fd < max_fd; fd++) close(fd);
        execl(helper, helper, "fd:3", "0", (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    if (child < 0) { close(sockets[0]); return -1; }
    ca_log("Z1 native capture helper PID=%ld", (long)child);
    unsigned char *frame = NULL;
    size_t capacity = 0;
    int result = -1;
    bool awaiting_overlay=false;
    unsigned overlay_wait_frames=0;
    while (!atomic_load(stop)) {
        /* A live helper can lose an acknowledgement while still producing
         * video. Retry after three seconds of frames rather than leaving the
         * channel permanently latched busy. */
        if (awaiting_overlay && overlay_wait_frames >= 180) {
            awaiting_overlay=false;
            overlay_wait_frames=0;
        }
        if (overlay && !awaiting_overlay && atomic_load(&overlay->applied)==-EINPROGRESS) {
            uint8_t value=atomic_load(&overlay->desired);
            if (send(sockets[0],&value,1,MSG_NOSIGNAL|MSG_DONTWAIT)==1) {
                awaiting_overlay=true;
                overlay_wait_frames=0;
            }
        }
        struct ca_z1_native_header header;
        if (receive_exact(sockets[0], &header, sizeof(header), stop)) break;
        if (header.magic==CA_Z1_NATIVE_OVERLAY_MAGIC) {
            if (header.size || header.stream>1 || header.key>4095) break;
            /* A previous request can be acknowledged after a retry has
             * already completed. It is stale, not a video transport error. */
            if (!awaiting_overlay) continue;
            if (overlay) {
                int applied=header.key ? -(int)header.key : (int)header.stream;
                /* A timeout may have caused the app to request rollback while
                 * this older command was still in flight. Send the latest next. */
                if ((int)header.stream!=atomic_load(&overlay->desired)) applied=-EINPROGRESS;
                atomic_store(&overlay->applied,applied);
            }
            awaiting_overlay=false;
            overlay_wait_frames=0;
            continue;
        }
        if (header.magic==CA_Z1_NATIVE_AE_MAGIC) {
            struct ca_exposure sample;
            if (header.size!=sizeof(sample) || header.stream || header.key ||
                receive_exact(sockets[0],&sample,sizeof(sample),stop)) break;
            if (sample.lens || sample.source) break;
            if (exposure) exposure(opaque,&sample);
            continue;
        }
        if (header.magic != CA_Z1_NATIVE_MAGIC || header.stream > 1 ||
            header.key > 1 || !header.size || header.size > CA_Z1_NATIVE_MAX_FRAME) {
            ca_log("Z1 native capture helper sent an invalid frame header");
            break;
        }
        if (header.size > capacity) {
            unsigned char *next = realloc(frame, header.size);
            if (!next) break;
            frame = next;
            capacity = header.size;
        }
        if (receive_exact(sockets[0], frame, header.size, stop)) break;
        publish(opaque, frame, header.size, header.pts, header.key != 0, header.stream);
        if (awaiting_overlay && overlay_wait_frames < 180) overlay_wait_frames++;
    }
    if (atomic_load(stop)) result = 0;
    free(frame);
    close(sockets[0]);
    kill(child, SIGTERM);
    int status;
    bool reaped = false;
    for (unsigned i = 0; i < 50; i++) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) { reaped = true; break; }
        usleep(100000);
    }
    if (!reaped) {
        ca_log("Z1 native capture helper did not stop; killing PID=%ld", (long)child);
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    return result;
}
