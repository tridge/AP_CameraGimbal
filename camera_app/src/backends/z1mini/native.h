#ifndef CA_Z1_NATIVE_H
#define CA_Z1_NATIVE_H
#include "apcam/compiler.h"
#include "apcam/atomic.h"
#include "camera_app/exposure.h"
#include "camera_app/image_tracker.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Experimental local ARM-native helper wire format, followed by size bytes of
 * one Annex-B access unit. PTS is microseconds. Data uses child fd 3; stdout and
 * stderr remain diagnostic logs. Stream 0 is live 1080p, stream 1 recording 4K.
 * CA_Z1_NATIVE_AE_MAGIC carries a ca_exposure payload (stream/key zero).
 * The helper exclusively owns the AX pipeline. */
#define CA_Z1_NATIVE_MAGIC UINT32_C(0x34363248)
/* Parent sends a sequenced request on the same duplex socket; the helper
 * replies with that sequence only after both encoders have applied it. */
struct ca_z1_overlay_control { atomic_int desired, applied; };
struct ca_z1_overlay_request {
    uint32_t sequence;
    uint8_t desired;
    uint8_t reserved[3];
};
APC_STATIC_ASSERT(sizeof(struct ca_z1_overlay_request) == 8, "overlay request ABI");
#define CA_Z1_NATIVE_OVERLAY_MAGIC UINT32_C(0x3144534f)
/* Capability announcement in an overlay header's PTS, before any frames.
 * Outside the uint32 request sequence range. The existing header format lets
 * older receivers continue streaming even without this capability.
 * READY and VERSION form one protocol revision: change both together whenever
 * request interpretation changes. Unknown revisions leave video available. */
#define CA_Z1_NATIVE_OVERLAY_READY (UINT64_MAX - 1U)
/* Reject legacy one-byte toggles even when eight accumulate into a request. */
#define CA_Z1_NATIVE_OVERLAY_VERSION 0xa5U
#define CA_Z1_NATIVE_AE_MAGIC UINT32_C(0x31454143)
#define CA_Z1_NATIVE_LUMA_MAGIC UINT32_C(0x314d554c)
#define CA_Z1_NATIVE_MAX_FRAME (8U * 1024U * 1024U)
struct ca_z1_native_header {
    uint32_t magic, size;
    uint64_t pts;
    uint32_t key, stream;
};
APC_STATIC_ASSERT(sizeof(struct ca_z1_native_header) == 24, "native frame ABI");
typedef void (*ca_z1_native_frame_fn)(void *, const uint8_t *, size_t, uint64_t, bool, unsigned);
typedef void (*ca_z1_native_exposure_fn)(void *, const struct ca_exposure *);
typedef void (*ca_z1_native_tracking_fn)(void *, const ca_tracking_frame &);
typedef int (*ca_z1_native_run_fn)(const atomic_bool *, ca_z1_native_frame_fn, void *);
int ca_z1_native_receive(const char *helper, const atomic_bool *stop,
                         ca_z1_native_frame_fn publish, ca_z1_native_exposure_fn exposure, void *opaque,
                         struct ca_z1_overlay_control *overlay, bool inverted,
                         ca_z1_native_tracking_fn tracking=nullptr);
#endif
