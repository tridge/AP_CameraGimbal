#pragma once

#include "camera_app/video_metadata.h"
#include <vector>

#define CA_MISB_KLV_MAX 512U

/* MISB ST 0601.8 UAS Datalink Local Set. Unknown or invalid measurements are
 * omitted, never replaced with zero. UTC is the metadata snapshot time.
 * Dimensions describe this stream's displayed image, including any crop. */
size_t ca_misb_klv(const ca_metadata *metadata, const timespec *utc,
                   unsigned width, unsigned height, uint8_t *output, size_t capacity);

/* Small dependency-free ST 1402 synchronous KLV transport muxer. One context
 * per output, with 90 kHz presentation times shared by video and metadata.
 * No B-frame reordering: sources must supply access units in display order. */
struct ca_misb_ts {
    uint8_t continuity[4]{};
    uint8_t sequence = 0;
    bool started = false;
    bool headers_pending = false;
    uint64_t last_pts = 0;
    unsigned width = 0, height = 0;
    std::vector<uint8_t> headers;
};
using ca_misb_write_fn = int (*)(void *, const void *, size_t);
int ca_misb_ts_write(ca_misb_ts *mux, ca_video_codec codec,
                      const uint8_t *annex_b, size_t length, uint64_t pts90k,
                      bool key_frame, const ca_metadata *metadata, const timespec *utc,
                      ca_misb_write_fn write, void *opaque);

/* Cache only the current version of each H.264/H.265 parameter-set ID.
 * Returns 1 on replacement/insertion, 0 for unchanged or unusable NALs,
 * or -1 if the set of active headers exceeds the bounded cache. */
int ca_video_cache_parameter_set(std::vector<uint8_t> &headers, ca_video_codec codec,
                                  const uint8_t *nal, size_t length);

/* Remember a single parameter-set NAL while a live output is idle, without
 * generating metadata or transport packets. Returns the cache change status. */
int ca_misb_ts_cache_nal(ca_misb_ts *mux, ca_video_codec codec,
                         const uint8_t *nal, size_t length);
