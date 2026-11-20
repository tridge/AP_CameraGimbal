#pragma once
#include <stdint.h>
// The worker owns all disk I/O. Queries never wait for SD reads.
// READY is preferred over FC terrain. PENDING lets the caller wait for a
// target height instead of freezing a temporary fallback into a burst.
enum ca_terrain_state { CA_TERRAIN_MISSING, CA_TERRAIN_PENDING, CA_TERRAIN_READY };
void ca_terrain_start(const char *directory);
void ca_terrain_stop();
ca_terrain_state ca_terrain_height(int32_t lat, int32_t lon, float &height);
