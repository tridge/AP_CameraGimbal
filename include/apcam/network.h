#ifndef APCAM_NETWORK_H
#define APCAM_NETWORK_H
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Shared by config parsing, web validation and the network service. */
static inline bool apcam_ipv4_host(const char *text, struct in_addr *address)
{
    if (inet_pton(AF_INET, text, address) != 1)
        return false;
    uint32_t host = ntohl(address->s_addr);
    return (host >> 24) > 0 && (host >> 24) != 127 && (host >> 24) < 224;
}
/* A video URI override sent to the GCS as is: rtsp:// or rtsps://, a host name
 * or IPv4 address, an optional port and an optional path. Printable ASCII only,
 * no credentials, and short enough for VIDEO_STREAM_INFORMATION's uri field. */
#define APCAM_VIDEO_URI_SIZE 160
static inline bool apcam_video_uri_valid(const char *text)
{
    size_t length = strlen(text);
    if (length >= APCAM_VIDEO_URI_SIZE)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c <= 32 || c > 126 || c == '"')
            return false;
    }
    const char *host = !strncmp(text, "rtsp://", 7) ? text + 7 :
        !strncmp(text, "rtsps://", 8) ? text + 8 : NULL;
    if (!host)
        return false;
    size_t host_length = strcspn(host, ":/?#@");
    if (!host_length || strspn(host, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_") != host_length)
        return false;
    const char *rest = host + host_length;
    if (*rest == ':') {
        size_t digits = strspn(++rest, "0123456789");
        if (!digits || digits > 5 || atoi(rest) < 1 || atoi(rest) > 65535)
            return false;
        rest += digits;
    }
    return *rest == '\0' || *rest == '/';
}
/* True if text is blank, a /dev serial device, or udp:// or tcp:// IPv4:PORT. */
#define APCAM_UART_DEVICE_OVERRIDE_SIZE 160
static inline bool apcam_uart_device_override_valid(const char *text)
{
    size_t length = strlen(text);
    if (length >= APCAM_UART_DEVICE_OVERRIDE_SIZE)
        return false;
    if (!length)
        return true;
    if (!strncmp(text, "/dev/", 5))
        return length > 5 && !strstr(text, "..") &&
               strspn(text + 5, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._/-") == length - 5;
    if (strncmp(text, "udp://", 6) && strncmp(text, "tcp://", 6))
        return false;
    const char *colon = strchr(text + 6, ':');
    size_t host_length = colon ? (size_t)(colon - (text + 6)) : 0;
    if (!colon || host_length >= 16)
        return false;
    char host[16];
    memcpy(host, text + 6, host_length);
    host[host_length] = '\0';
    struct in_addr address;
    if (strcmp(host, "localhost") && inet_pton(AF_INET, host, &address) != 1)
        return false;
    size_t digits = strspn(colon + 1, "0123456789");
    return digits && digits <= 5 && colon[1 + digits] == '\0' && atoi(colon + 1) >= 1 && atoi(colon + 1) <= 65535;
}
static inline bool apcam_ipv4_prefix(const char *text, struct in_addr *address, unsigned *prefix)
{
    char copy[32], *end;
    if (strlen(text) >= sizeof(copy))
        return false;
    strcpy(copy, text);
    char *slash = strchr(copy, '/');
    if (!slash)
        return false;
    *slash++ = 0;
    if (!*slash || *slash == '0' || strspn(slash, "0123456789") != strlen(slash))
        return false;
    unsigned long bits = strtoul(slash, &end, 10);
    if (*end || bits < 1 || bits > 32 || !apcam_ipv4_host(copy, address))
        return false;
    uint32_t host = ntohl(address->s_addr), mask = UINT32_MAX << (32 - bits);
    if (bits < 31 && (!(host & ~mask) || (host & ~mask) == ~mask))
        return false;
    *prefix = (unsigned)bits;
    return true;
}
static inline bool apcam_gateway_on_link(uint32_t gateway, uint32_t address, unsigned prefix)
{
    uint32_t mask = UINT32_MAX << (32 - prefix);
    return !((gateway ^ address) & mask) &&
           (prefix >= 31 || ((gateway & ~mask) && (gateway & ~mask) != ~mask));
}
static inline bool apcam_network_valid(const char *primary, const char *secondary, const char *gateway)
{
    struct in_addr a = {0}, b = {0}, g = {0};
    unsigned pa = 32, pb = 32;
    if ((*primary && !apcam_ipv4_prefix(primary, &a, &pa)) ||
        (*secondary && !apcam_ipv4_prefix(secondary, &b, &pb)) || (*gateway && !apcam_ipv4_host(gateway, &g)))
        return false;
    if (*primary && *secondary && a.s_addr == b.s_addr)
        return false;
    if (*gateway && ((a.s_addr && a.s_addr == g.s_addr) || (b.s_addr && b.s_addr == g.s_addr)))
        return false;
    if (*primary && *gateway) {
        uint32_t ga = ntohl(g.s_addr), aa = ntohl(a.s_addr), ab = ntohl(b.s_addr);
        if (!apcam_gateway_on_link(ga, aa, pa) && (!*secondary || !apcam_gateway_on_link(ga, ab, pb)))
            return false;
    }
    return true;
}
#endif
