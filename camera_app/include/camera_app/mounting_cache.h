#pragma once

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* The ZT30 gimbal MCU answers the mounting query with 0, not 1 (upright) or
 * 2 (inverted), while the gimbal is pitched well away from level: 0 was seen
 * at -33 and -49 degrees, 1 at 0 and -26. Its last report is kept for an app
 * that starts while the gimbal is held pitched down. */
static inline uint8_t ca_mounting_cache_load(const char *path)
{
    char value = 0;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const ssize_t n = read(fd, &value, 1);
    close(fd);
    return n == 1 && (value == '1' || value == '2') ? (uint8_t)(value - '0') : 0;
}

/* Written only on a change, so flash is touched when the camera is remounted.
 * Replaced by rename: an interrupted write keeps the previous report. */
static inline void ca_mounting_cache_save(const char *path, uint8_t direction)
{
    char temporary[512];
    if ((direction != 1 && direction != 2) || ca_mounting_cache_load(path) == direction) return;
    if (snprintf(temporary, sizeof(temporary), "%s.new", path) >= (int)sizeof(temporary)) return;
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const char value = (char)('0' + direction);
    const bool written = write(fd, &value, 1) == 1 && fsync(fd) == 0;
    if (close(fd) == 0 && written) (void)rename(temporary, path);
    else (void)unlink(temporary);
}
