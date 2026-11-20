#ifndef APCAM_CONFIG_STATUS_H
#define APCAM_CONFIG_STATUS_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* Identifies the exact saved INI acknowledged by the app. Not a security hash. */
static inline uint64_t apcam_config_hash(const void *data, size_t length, uint64_t hash)
{
    const unsigned char *bytes = (const unsigned char*)(data);
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}
#define APCAM_CONFIG_HASH_INITIAL UINT64_C(14695981039346656037)
static inline int apcam_config_file_hash(const char *path, uint64_t *hash)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    unsigned char data[4096];
    size_t length;
    *hash = APCAM_CONFIG_HASH_INITIAL;
    while ((length = fread(data, 1, sizeof(data), file)) != 0)
        *hash = apcam_config_hash(data, length, *hash);
    int result = ferror(file) ? -1 : 0;
    fclose(file);
    return result;
}
#endif
