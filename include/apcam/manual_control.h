#ifndef APCAM_MANUAL_CONTROL_H
#define APCAM_MANUAL_CONTROL_H
#include "compiler.h"
#include <stdint.h>
/* Private loopback IPC shared by the web server and camera app. No persistent
 * state: a new app owns a new socket and issues new random lease tokens. */
#define APCAM_MANUAL_MAGIC UINT32_C(0x4d434131)
#define APCAM_MANUAL_LEASE_MS 5000U
#define APCAM_MANUAL_PULSE_MS 180U
enum apcam_manual_action {
    APCAM_MANUAL_ACQUIRE = 1,
    APCAM_MANUAL_RENEW,
    APCAM_MANUAL_RELEASE,
    APCAM_MANUAL_LEFT,
    APCAM_MANUAL_RIGHT,
    APCAM_MANUAL_UP,
    APCAM_MANUAL_DOWN,
    APCAM_MANUAL_CENTER,
    APCAM_MANUAL_ZOOM
};
struct apcam_manual_packet {
    uint32_t magic, action;
    uint8_t token[16];
    float value;
    int32_t result;
};
APC_STATIC_ASSERT(sizeof(struct apcam_manual_packet) == 32, "manual control IPC layout");
#endif
