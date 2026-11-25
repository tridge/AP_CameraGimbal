#pragma once

#include <errno.h>
#include <limits.h>
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
