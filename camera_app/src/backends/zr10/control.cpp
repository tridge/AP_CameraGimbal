#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "apcam/target.h"
/* ZR10 uses native controls and the SIYI MCU query tunnel. */
#include <stdint.h>
#ifdef __APPLE__
// Mach-O cannot leave an optional symbol unresolved without a provider.
// SITL has no hardware focus samples; a linked pipeline overrides this stub.
__attribute__((weak)) int ca_zr10_take_focus(uint8_t[27]) { return 0; }
#else
extern int ca_zr10_take_focus(uint8_t payload[27]) __attribute__((weak));
#endif
#include "../siyi/control.cpp"
