#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/raw_thermal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char root[] = "/tmp/camera-app-raw-XXXXXX";
    char first[4096];
    char second[4096];
    char directory[4096];
    const uint16_t pixels[] = {0x1234U, 0xabcdU, 0x0001U, 0xffffU};
    const uint8_t expected[] = {0x34, 0x12, 0xcd, 0xab,
                                0x01, 0x00, 0xff, 0xff};
    const struct timespec captured_at = {
        .tv_sec = 1700000000,
        .tv_nsec = 123456789,
    };
    uint8_t data[sizeof(expected)];
    struct ca_raw_thermal *capture = NULL;
    struct stat status;
    int fd;
    char *slash;

    assert(setenv("TZ", "UTC", 1) == 0);
    tzset();
    assert(mkdtemp(root) != NULL);
    assert(ca_raw_thermal_open(&capture, root) == 0);
    assert(ca_raw_thermal_write_at(capture, pixels, 2, 2, &captured_at) == 0);
    assert(strlen(ca_raw_thermal_path(capture)) < sizeof(first));
    strcpy(first, ca_raw_thermal_path(capture));
    assert(strstr(first, "/2023-11-14/2023-11-14_22-13-20_123_I.bin") != NULL);
    assert(stat(first, &status) == 0 && status.st_size == (off_t)sizeof(data));
    assert(status.st_mtim.tv_sec == captured_at.tv_sec);
    assert(status.st_mtim.tv_nsec == captured_at.tv_nsec);
    fd = open(first, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0 && read(fd, data, sizeof(data)) == (ssize_t)sizeof(data));
    assert(close(fd) == 0);
    assert(memcmp(data, expected, sizeof(data)) == 0);

    assert(ca_raw_thermal_write_at(capture, pixels, 2, 2, &captured_at) == 0);
    assert(strlen(ca_raw_thermal_path(capture)) < sizeof(second));
    strcpy(second, ca_raw_thermal_path(capture));
    assert(strcmp(first, second) != 0);
    assert(strstr(second, "_123_1_I.bin") != NULL);
    assert(ca_raw_thermal_count(capture) == 2U);
    assert(ca_raw_thermal_write(capture, NULL, 2, 2) < 0);
    ca_raw_thermal_close(capture);

    strcpy(directory, first);
    slash = strrchr(directory, '/');
    assert(slash != NULL);
    *slash = '\0';
    assert(unlink(first) == 0);
    assert(unlink(second) == 0);
    assert(rmdir(directory) == 0);

    // no card mounted (a missing parent in a plain directory): nothing is created
    char nested[96], parent[64];
    snprintf(parent, sizeof(parent), "%s/DCIM", root);
    snprintf(nested, sizeof(nested), "%s/capture", parent);
    assert(ca_raw_thermal_open(&capture, nested) == 0);
    errno = 0;
    assert(ca_raw_thermal_write_at(capture, pixels, 2, 2, &captured_at) < 0 && errno == ENODEV);
    ca_raw_thermal_close(capture);
    assert(stat(parent, &status) < 0 && errno == ENOENT);
    assert(rmdir(root) == 0);
    puts("raw thermal capture tests passed");
    return 0;
}
