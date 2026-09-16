#ifndef CA_Z1_NATIVE_H
#define CA_Z1_NATIVE_H
#include <stdatomic.h>
#include "camera_app/exposure.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Experimental local ARM-native helper wire format, followed by size bytes of
 * one Annex-B access unit. PTS is microseconds. Data uses child fd 3; stdout and
 * stderr remain diagnostic logs. Stream 0 is live 1080p, stream 1 recording 4K.
 * CA_Z1_NATIVE_AE_MAGIC carries a ca_exposure payload (stream/key zero).
 * The helper exclusively owns the AX pipeline. */
#define CA_Z1_NATIVE_MAGIC UINT32_C(0x34363248)
/* Parent sends one byte (0/1) on the same duplex socket; helper replies only
 * after both encoder inputs have applied it. Negative applied values are errno. */
struct ca_z1_overlay_control { atomic_int desired, applied; };
#define CA_Z1_NATIVE_OVERLAY_MAGIC UINT32_C(0x3144534f)
#define CA_Z1_NATIVE_AE_MAGIC UINT32_C(0x31454143)
#define CA_Z1_NATIVE_MAX_FRAME (8U * 1024U * 1024U)
struct ca_z1_native_header {
    uint32_t magic, size;
    uint64_t pts;
    uint32_t key, stream;
};
_Static_assert(sizeof(struct ca_z1_native_header) == 24, "native frame ABI");
typedef void (*ca_z1_native_frame_fn)(void *, const uint8_t *, size_t, uint64_t, bool, unsigned);
typedef void (*ca_z1_native_exposure_fn)(void *, const struct ca_exposure *);
typedef int (*ca_z1_native_run_fn)(const atomic_bool *, ca_z1_native_frame_fn, void *);
int ca_z1_native_receive(const char *helper, const atomic_bool *stop,
                         ca_z1_native_frame_fn publish, ca_z1_native_exposure_fn exposure, void *opaque,
                         struct ca_z1_overlay_control *overlay);
#endif
