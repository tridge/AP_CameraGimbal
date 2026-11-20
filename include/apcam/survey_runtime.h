#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// The status heartbeat belongs in temporary storage, not on camera flash.
// Hash the config path so separate SITL cameras have separate status files.
inline bool apcam_survey_status_path(const char *config, char *path, size_t size,
                                     const char *directory = nullptr)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(config); *p; ++p) {
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    }
    const char *root = directory;
    if (!root || !*root) root = getenv("TMPDIR");
#ifdef _WIN32
    if (!root || !*root) root = getenv("TEMP");
    if (!root || !*root) root = ".";
#else
    // Embedded launchers set TMP to their writable RAM filesystem; /tmp
    // can belong to a read-only rootfs (as on MT11).
    if (!root || !*root) root = getenv("TMP");
    if (!root || !*root) root = "/tmp";
#endif
    const int n = snprintf(path, size, "%s/apcam-survey-%016llx.json", root,
                           (unsigned long long)hash);
    return n >= 0 && size_t(n) < size;
}
