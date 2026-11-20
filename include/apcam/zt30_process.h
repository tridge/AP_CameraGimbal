#pragma once
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

static inline bool zt30_staged_app_pid(pid_t pid, const char *executable,
                                      const char *launch_path, const char *prefix)
{
    const size_t n = strlen(prefix);
    if (strncmp(executable, prefix, n) != 0) return false;
    const char *tail = executable + n;
    if (strlen(tail) != 6U + strlen("/camera-app")) return false;
    for (size_t i = 0; i < 6U; ++i)
        if (!isalnum((unsigned char)tail[i])) return false;
    if (strcmp(tail + 6U, "/camera-app") != 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char argument[4096];
    const size_t length = fread(argument, 1, sizeof(argument), file);
    fclose(file);
    const size_t expected = strlen(launch_path);
    return length > expected && argument[expected] == '\0' &&
           memcmp(argument, launch_path, expected) == 0;
}

#include "zt30_slots.h"

static inline bool zt30_slot_app_pid(pid_t pid, const char *executable, const char *root)
{
    // Check actual executable identity, not only argv[0]. Retained old slots
    // remain recognizable after active switches to another release.
    char proc[64], argument[4096];
    snprintf(proc, sizeof(proc), "/proc/%ld/cmdline", (long)pid);
    FILE *file = fopen(proc, "rb"); if (!file) return false;
    size_t length = fread(argument, 1, sizeof(argument), file); fclose(file);
    const char *end = (const char *)memchr(argument, 0, length);
    if (!end) return false;
    std::string stable = std::string(root) + "/camera-app";
    const char *launch = strcmp(argument, stable.c_str()) ? argument : executable;
    std::string prefix = std::string(root) + "/slots/";
    if (strncmp(launch, prefix.c_str(), prefix.size())) return false;
    const char *tail = launch + prefix.size();
    if (strlen(tail) != 64 + strlen("/camera-app") || strcmp(tail + 64, "/camera-app")) return false;
    char id[65]; memcpy(id, tail, 64); id[64] = 0;
    if (!apcam_zt30::slot_id_valid(id)) return false;
    std::string path, error;
    if (!apcam_zt30::slot_directory(root, id, path, error)) return false;
    path += "/camera-app";
    snprintf(proc, sizeof(proc), "/proc/%ld/exe", (long)pid);
    struct stat actual{}, expected{};
    if (stat(proc, &actual) < 0 || actual.st_uid != geteuid() || !S_ISREG(actual.st_mode)) return false;
    const bool unlinked = actual.st_nlink == 0 && !strcmp(executable, path.c_str());
    if (!unlinked) {
        if (lstat(path.c_str(), &expected) < 0 || !S_ISREG(expected.st_mode) ||
            actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino) return false;
    }
    // An unlinked old executable remains stoppable using its kernel-reported
    // slot path and managed config identity; never launch that incomplete slot.
    snprintf(proc, sizeof(proc), "/proc/%ld/environ", (long)pid);
    file = fopen(proc, "rb"); if (!file) return false;
    char *entry = nullptr; size_t capacity = 0; bool belongs = false;
    std::string config = std::string("CAMERA_APP_CONFIG=") + root + "/camera.ini";
    while (getdelim(&entry, &capacity, 0, file) > 0)
        if (config == entry) { belongs = true; break; }
    free(entry); fclose(file); return belongs;
}
