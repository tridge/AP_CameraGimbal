#ifndef CAMERA_APP_RAW_THERMAL_SERVER_H
#define CAMERA_APP_RAW_THERMAL_SERVER_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CA_RAW_THERMAL_FILENAME_BYTES 128U
#define CA_RAW_THERMAL_HEADER_BYTES \
    (CA_RAW_THERMAL_FILENAME_BYTES + sizeof(double))

struct ca_raw_thermal_server;

int ca_raw_thermal_server_open(struct ca_raw_thermal_server **server,
                               const char *capture_root, unsigned port,
                               uint32_t width, uint32_t height);
int ca_raw_thermal_server_publish(struct ca_raw_thermal_server *server,
                                  const uint16_t *pixels,
                                  const struct timespec *captured_at);
unsigned ca_raw_thermal_server_port(const struct ca_raw_thermal_server *server);
void ca_raw_thermal_server_source_timeout(struct ca_raw_thermal_server *, unsigned milliseconds);
void ca_raw_thermal_server_close(struct ca_raw_thermal_server *server);

#endif
