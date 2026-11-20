#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/raw_thermal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct ca_raw_thermal {
    char root[PATH_MAX];
    char path[PATH_MAX];
    unsigned count;
};

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
        if (written == 0) errno = EIO;
        return -1;
    }
    return 0;
}

static int make_path(struct ca_raw_thermal *capture, int *fd,
                     const struct timespec *captured_at)
{
    uint64_t epoch_ms;
    time_t seconds;
    unsigned milliseconds;
    struct tm local;
    char day[16];
    char directory[PATH_MAX];

    if (captured_at->tv_sec < 0 || captured_at->tv_nsec < 0 ||
        captured_at->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    epoch_ms = (uint64_t)captured_at->tv_sec * 1000U +
               (uint64_t)captured_at->tv_nsec / 1000000U;
    seconds = (time_t)(epoch_ms / 1000U);
    milliseconds = (unsigned)(epoch_ms % 1000U);
    if (localtime_r(&seconds, &local) == NULL ||
        strftime(day, sizeof(day), "%Y-%m-%d", &local) == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (mkdir(capture->root, 0700) < 0 && errno != EEXIST) return -1;
    if (snprintf(directory, sizeof(directory), "%s/%s", capture->root,
                 day) >= (int)sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    for (unsigned attempt = 0; attempt < 1000U; attempt++) {
        int length;
        if (attempt == 0U) {
            length = snprintf(capture->path, sizeof(capture->path),
                              "%s/%s_%02d-%02d-%02d_%u_I.bin", directory,
                              day, local.tm_hour, local.tm_min, local.tm_sec,
                              milliseconds);
        } else {
            length = snprintf(capture->path, sizeof(capture->path),
                              "%s/%s_%02d-%02d-%02d_%u_%u_I.bin", directory,
                              day, local.tm_hour, local.tm_min, local.tm_sec,
                              milliseconds, attempt);
        }
        if (length < 0 || (size_t)length >= sizeof(capture->path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        *fd = open(capture->path,
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (*fd >= 0) return 0;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int ca_raw_thermal_open(struct ca_raw_thermal **result, const char *root)
{
    struct ca_raw_thermal *capture;

    if (result == NULL || root == NULL || *root == '\0' ||
        strlen(root) >= PATH_MAX) {
        errno = EINVAL;
        return -1;
    }
    capture = (struct ca_raw_thermal*)(calloc(1, sizeof(*capture)));
    if (capture == NULL) return -1;
    memcpy(capture->root, root, strlen(root) + 1U);
    *result = capture;
    return 0;
}

int ca_raw_thermal_write(struct ca_raw_thermal *capture,
                         const uint16_t *pixels, uint32_t width,
                         uint32_t height)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) return -1;
    return ca_raw_thermal_write_at(capture, pixels, width, height, &now);
}

int ca_raw_thermal_write_at(struct ca_raw_thermal *capture,
                            const uint16_t *pixels, uint32_t width,
                            uint32_t height,
                            const struct timespec *captured_at)
{
    size_t pixel_count;
    size_t bytes;
    int fd = -1;
    int result;

    if (capture == NULL || pixels == NULL || captured_at == NULL ||
        width == 0U || height == 0U ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        errno = EINVAL;
        return -1;
    }
    pixel_count = (size_t)width * height;
    if (pixel_count > SIZE_MAX / sizeof(*pixels)) {
        errno = EOVERFLOW;
        return -1;
    }
    bytes = pixel_count * sizeof(*pixels);
    if (make_path(capture, &fd, captured_at) < 0) return -1;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    result = write_all(fd, (const uint8_t *)pixels, bytes);
#else
    result = 0;
    for (size_t offset = 0; offset < pixel_count && result == 0;) {
        uint8_t buffer[4096];
        size_t chunk = pixel_count - offset;
        if (chunk > sizeof(buffer) / 2U) chunk = sizeof(buffer) / 2U;
        for (size_t i = 0; i < chunk; i++) {
            uint16_t value = pixels[offset + i];
            buffer[i * 2U] = (uint8_t)value;
            buffer[i * 2U + 1U] = (uint8_t)(value >> 8);
        }
        result = write_all(fd, buffer, chunk * 2U);
        offset += chunk;
    }
#endif
    if (result == 0) {
        const struct timespec times[2] = {*captured_at, *captured_at};
        if (futimens(fd, times) < 0) result = -1;
    }
    if (close(fd) < 0 && result == 0) result = -1;
    if (result < 0) {
        int saved_errno = errno;
        (void)unlink(capture->path);
        capture->path[0] = '\0';
        errno = saved_errno;
        return -1;
    }
    capture->count++;
    return 0;
}

const char *ca_raw_thermal_path(const struct ca_raw_thermal *capture)
{
    return capture != NULL ? capture->path : "";
}

unsigned ca_raw_thermal_count(const struct ca_raw_thermal *capture)
{
    return capture != NULL ? capture->count : 0U;
}

void ca_raw_thermal_close(struct ca_raw_thermal *capture)
{
    free(capture);
}
