#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/still.h"

#include "camera_app/metadata.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_all(int fd, const unsigned char *data, size_t length)
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

int ca_still_write_jpeg(const char *root, char suffix,
                        const unsigned char *jpeg, size_t jpeg_length,
                        const struct timespec *captured_at,
                        char *path, size_t path_size, const ca_metadata *capture_metadata)
{
    struct tm local;
    char day[16];
    char directory[PATH_MAX];
    char temporary[PATH_MAX];
    unsigned milliseconds;
    struct ca_metadata metadata;
    uint8_t segments[CA_METADATA_SEGMENTS_MAX];
    size_t segments_length;
    int fd = -1;
    int result = -1;

    if (root == NULL || *root == '\0' || jpeg == NULL || jpeg_length < 4U ||
        captured_at == NULL || captured_at->tv_sec < 0 ||
        captured_at->tv_nsec < 0 || captured_at->tv_nsec >= 1000000000L ||
        path == NULL || path_size == 0U ||
        jpeg[0] != 0xffU || jpeg[1] != 0xd8U ||
        jpeg[jpeg_length - 2U] != 0xffU || jpeg[jpeg_length - 1U] != 0xd9U) {
        errno = EINVAL;
        return -1;
    }
    if (localtime_r(&captured_at->tv_sec, &local) == NULL ||
        strftime(day, sizeof(day), "%Y-%m-%d", &local) == 0U) {
        errno = EINVAL;
        return -1;
    }
    milliseconds = (unsigned)(captured_at->tv_nsec / 1000000L);
    if (mkdir(root, 0700) < 0 && errno != EEXIST) return -1;
    if (snprintf(directory, sizeof(directory), "%s/%s", root, day) >=
        (int)sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;

    for (unsigned attempt = 0; attempt < 1000U; attempt++) {
        int length;
        if (attempt == 0U) {
            length = snprintf(path, path_size,
                              "%s/%s_%02d-%02d-%02d_%u_%c.jpg", directory,
                              day, local.tm_hour, local.tm_min, local.tm_sec,
                              milliseconds, suffix);
        } else {
            length = snprintf(path, path_size,
                              "%s/%s_%02d-%02d-%02d_%u_%u_%c.jpg", directory,
                              day, local.tm_hour, local.tm_min, local.tm_sec,
                              milliseconds, attempt, suffix);
        }
        if (length < 0 || (size_t)length >= path_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (access(path, F_OK) == 0) continue;
        if (errno != ENOENT) return -1;
        length = snprintf(temporary, sizeof(temporary), "%s.part", path);
        if (length < 0 || (size_t)length >= sizeof(temporary)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) return -1;
    }
    if (fd < 0) {
        errno = EEXIST;
        return -1;
    }
    /* EXIF/XMP go right after SOI; the rest of the file is untouched */
    if(capture_metadata) metadata=*capture_metadata;
    else ca_metadata_snapshot(&metadata);
    segments_length = ca_metadata_jpeg_segments(&metadata, captured_at,
                                                segments, sizeof(segments));
    if (write_all(fd, jpeg, 2U) == 0 &&
        write_all(fd, segments, segments_length) == 0 &&
        write_all(fd, jpeg + 2U, jpeg_length - 2U) == 0) {
        const struct timespec times[2] = {*captured_at, *captured_at};
        if (futimens(fd, times) == 0 && fsync(fd) == 0 && close(fd) == 0) {
            fd = -1;
            if (rename(temporary, path) == 0) result = 0;
        }
    }
    if (result < 0) {
        int saved_errno = errno;
        if (fd >= 0) (void)close(fd);
        (void)unlink(temporary);
        path[0] = '\0';
        errno = saved_errno;
    }
    return result;
}
