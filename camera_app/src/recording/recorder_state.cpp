#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/recorder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void ca_recorder_init(struct ca_recorder *recorder, const char *state_path)
{
    recorder->active = false;
    recorder->state_path = state_path;
    if (state_path != NULL) (void)unlink(state_path);
}

int ca_recorder_set(struct ca_recorder *recorder, bool active)
{
    int fd;
    char text[128];
    int length;

    if (recorder->active == active) return 0;
    if (!active) {
        recorder->active = false;
        if (recorder->state_path != NULL && unlink(recorder->state_path) < 0 && errno != ENOENT) {
            return -1;
        }
        return 0;
    }
    if (recorder->state_path != NULL) {
        fd = open(recorder->state_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) return -1;
        length = snprintf(text, sizeof(text),
                          "recording-control-active=1\nstarted=%lld\n",
                          (long long)time(NULL));
        if (length < 0 || (size_t)length >= sizeof(text) ||
            write(fd, text, (size_t)length) != length) {
            int saved_errno = errno;
            close(fd);
            errno = saved_errno != 0 ? saved_errno : EIO;
            return -1;
        }
        close(fd);
    }
    recorder->active = true;
    return 0;
}

bool ca_recorder_active(const struct ca_recorder *recorder)
{
    return recorder->active;
}

void ca_recorder_close(struct ca_recorder *recorder)
{
    (void)ca_recorder_set(recorder, false);
}
