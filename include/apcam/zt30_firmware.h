#pragma once
#include "sha256.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>

namespace apcam_zt30 {
constexpr uint64_t firmware_limit = 112ULL*1024*1024;
// Little-endian APCZTF01 index immediately before the stock 24-byte trailer.
inline uint64_t firmware_u64(const unsigned char *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; i++) n |= uint64_t(p[i]) << (8*i);
    return n;
}
inline bool firmware_read(int fd, void *out, size_t size)
{
    size_t at = 0;
    while (at < size) {
        ssize_t n = read(fd, static_cast<char *>(out)+at, size-at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        at += size_t(n);
    }
    return true;
}
inline bool firmware_digest(int fd, uint64_t offset, uint64_t size, const unsigned char *expected)
{
    if (lseek(fd, off_t(offset), SEEK_SET) < 0) return false;
    apcam::SHA256 hash;
    char buffer[65536];
    while (size) {
        size_t n = size_t(std::min<uint64_t>(size, sizeof(buffer)));
        if (!firmware_read(fd, buffer, n)) return false;
        hash.update(buffer, n); size -= n;
    }
    static const char hex[] = "0123456789abcdef";
    std::string value;
    for (unsigned i = 0; i < 32; i++) {value += hex[expected[i]>>4]; value += hex[expected[i]&15];}
    return hash.finish() == value;
}
inline bool firmware_bundle_span(int fd, uint64_t total, uint64_t &offset, uint64_t &size)
{
    unsigned char first[16], tail[112];
    if (total < 16 || total > firmware_limit || lseek(fd, 0, SEEK_SET) < 0 || !firmware_read(fd, first, sizeof(first))) return false;
    if (!memcmp(first, "APCZT301", 8)) {offset = 0; size = total; return total <= 32*1024*1024;}
    if (total < 16384+3*126976+112+13 || lseek(fd, off_t(total-sizeof(tail)), SEEK_SET) < 0 ||
        !firmware_read(fd, tail, sizeof(tail)) || memcmp(tail, "APCZTF01", 8) ||
        memcmp(tail+88, "12345678", 8) || memcmp(tail+96, first, 16)) return false;
    uint64_t payload = firmware_u64(tail+8);
    size = firmware_u64(tail+16); offset = 16384+payload;
    if (payload < 3*126976 || payload > 659*126976ULL || payload%126976 || size <= 12 ||
        size > 32*1024*1024 || offset+size+sizeof(tail) != total) return false;
    // Verify the unused SD image too: a damaged download must fail before
    // runtime checks, slot creation, cleanup, or active-selection changes.
    return firmware_digest(fd, 0, offset, tail+24) && firmware_digest(fd, offset, size, tail+56);
}
}
