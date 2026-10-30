#ifndef CAMERA_APP_MP4_H
#define CAMERA_APP_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ca_mp4;
struct ca_fmp4;

typedef int (*ca_fmp4_write_fn)(void *opaque, const void *data,
                                size_t length);

/* Shared recorder: APCG uses fragmented MP4 with per-frame telemetry SEI;
 * MISB uses MPEG-TS with frame-synchronous ST 0601 KLV. The format is captured
 * when opening; callers name files with ca_video_recording_extension().
 * Complete fragments/transport packets are playable
 * before close; background data syncs limit buffered loss on a crash.
 * FAT/vfat recordings roll on an IDR after 4 GiB minus 64 MiB, with a hard
 * ceiling of 4 GiB minus one byte. Other filesystems (including exFAT) do not
 * roll. Continuations use <stem>_part0002.<extension>, preserve decoder headers,
 * and restart the sample/telemetry timeline at zero. Close waits for the last
 * sync of every part and reports asynchronous I/O errors. */
int ca_mp4_open(struct ca_mp4 **writer, const char *path,
                unsigned width, unsigned height, unsigned frame_rate);
/* One complete Annex-B access unit per call, including all slices/SEI.
 * Playback uses the configured constant frame rate (pts is encoder-specific). */
int ca_mp4_write_h264(struct ca_mp4 *writer, const uint8_t *annex_b,
                      size_t length, uint64_t pts, bool key_frame, float hfov_deg);
int ca_mp4_close(struct ca_mp4 *writer);

/* Sequential fragmented MP4 for non-seekable transports.  The callback is
 * invoked synchronously and must consume the complete buffer before returning.
 */
int ca_fmp4_open(struct ca_fmp4 **writer, unsigned width, unsigned height,
                 unsigned frame_rate, ca_fmp4_write_fn write, void *opaque);
int ca_fmp4_write_h264(struct ca_fmp4 *writer, const uint8_t *annex_b,
                       size_t length, uint64_t pts, bool key_frame, float hfov_deg);
int ca_fmp4_close(struct ca_fmp4 *writer);

#endif
