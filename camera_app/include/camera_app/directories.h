#pragma once

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* mkdir -p: creates path and any missing parents. An existing component
 * that is not a directory is ENOTDIR. */
static inline int ca_mkdir_p(const char *path, mode_t mode)
{
    char partial[PATH_MAX];
    const size_t length = path ? strlen(path) : 0U;

    if (!length) { errno = EINVAL; return -1; }
    if (length >= sizeof(partial)) { errno = ENAMETOOLONG; return -1; }
    memcpy(partial, path, length + 1U);
    for (char *p = partial + 1; ; p++) {
        if (*p != '/' && *p != '\0') continue;
        const char end = *p;
        *p = '\0';
        if (mkdir(partial, mode) < 0) {
            struct stat status;
            if (errno != EEXIST) return -1;
            if (stat(partial, &status) < 0) return -1;
            if (!S_ISDIR(status.st_mode)) { errno = ENOTDIR; return -1; }
        }
        if (end == '\0') return 0;
        *p = '/';
    }
}

/* For folders on a card, such as /mnt/DCIM/capture. A missing parent is made
 * only directly under a mount point (a card with no DCIM yet). In a plain
 * directory it means no card is mounted, and nothing is created: ENODEV. */
static inline int ca_mkdir_p_mounted(const char *path, mode_t mode)
{
    char ancestor[PATH_MAX], above[PATH_MAX + 4];
    struct stat status, parent;
    const size_t length = path ? strlen(path) : 0U;

    if (!length) { errno = EINVAL; return -1; }
    if (length >= sizeof(ancestor)) { errno = ENAMETOOLONG; return -1; }
    memcpy(ancestor, path, length + 1U);
    char *slash = strrchr(ancestor, '/');
    if (!slash || slash == ancestor) return ca_mkdir_p(path, mode);
    *slash = '\0';
    if (stat(ancestor, &status) == 0) return ca_mkdir_p(path, mode);
    // the deepest folder that exists
    while (stat(ancestor, &status) < 0) {
        if (errno != ENOENT) return -1;
        slash = strrchr(ancestor, '/');
        if (!slash) { errno = ENODEV; return -1; }
        if (slash == ancestor) { ancestor[1] = '\0'; break; }
        *slash = '\0';
    }
    if (stat(ancestor, &status) < 0) return -1;
    snprintf(above, sizeof(above), "%s/..", ancestor);
    if (stat(above, &parent) < 0) return -1;
    if (status.st_dev == parent.st_dev && status.st_ino != parent.st_ino) { errno = ENODEV; return -1; }
    return ca_mkdir_p(path, mode);
}
