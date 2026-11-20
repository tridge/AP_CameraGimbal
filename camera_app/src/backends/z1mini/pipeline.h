#ifndef CA_Z1_PIPELINE_H
#define CA_Z1_PIPELINE_H
#include "apcam/atomic.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define CA_Z1_FRAME_MAX (4U * 1024U * 1024U)
typedef void (*ca_z1_frame_fn)(void *, const uint8_t *, size_t, uint64_t, bool);
/* Bounded H.264 RTP access-unit assembler, also exercised by host tests. */
struct ca_z1_rtp {
    uint8_t frame[CA_Z1_FRAME_MAX];
    uint8_t parameters[2048];
    size_t used, parameter_size;
    uint16_t sequence;
    uint32_t timestamp, last_timestamp;
    uint64_t ticks;
    bool have_sequence, have_timestamp, emitted, fu, damaged, key, wait_key;
    ca_z1_frame_fn consume;
    void *opaque;
};
int ca_z1_rtp_packet(struct ca_z1_rtp *, const uint8_t *, size_t);
int ca_z1_receive(atomic_bool *stop, ca_z1_frame_fn consume, void *opaque);
#endif
