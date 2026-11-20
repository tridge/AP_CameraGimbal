#ifndef CAMERA_APP_CADDX_H
#define CAMERA_APP_CADDX_H

#include <stdint.h>

/* Encode a CADDX angle command (radians; earth-frame roll and pitch). */
void ca_caddx_pack(uint8_t out[10], float roll, float pitch, float yaw);

#endif
