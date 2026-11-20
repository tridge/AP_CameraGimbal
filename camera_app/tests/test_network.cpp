#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/network.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    /* Refuse to alter the host network, even when invoked directly as root. */
    char current[128] = {}, initial[128] = {};
    assert(readlink("/proc/self/ns/net", current, sizeof(current) - 1) > 0);
    assert(readlink("/proc/1/ns/net", initial, sizeof(initial) - 1) > 0);
    assert(strcmp(current, initial) != 0);
    assert(argc == 6);
    struct ca_network_config config = {};
    snprintf(config.interface, sizeof(config.interface), "%s", argv[1]);
    snprintf(config.primary_address, sizeof(config.primary_address), "%s", argv[2]);
    snprintf(config.secondary_address, sizeof(config.secondary_address), "%s", argv[3]);
    snprintf(config.gateway, sizeof(config.gateway), "%s", argv[4]);
    if (ca_network_configure(&config, argv[5]) < 0) {
        fprintf(stderr, "network: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
