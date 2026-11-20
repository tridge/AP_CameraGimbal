#include "apcam/compiler.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include <new>
#define MINIMP4_IMPLEMENTATION
#include "minimp4.h"

#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"
#include "camera_app/misb.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#ifdef __APPLE__
#include <sys/mount.h>
#else
#include <sys/vfs.h>
#endif
#include <unistd.h>

/* Linux vfat/msdos share this magic; exFAT has its own filesystem type.
 * Keep headroom for an IDR rather than ever splitting a GOP between files. */
#define CA_MSDOS_SUPER_MAGIC 0x4d44
#define CA_FAT_MAX_BYTES UINT64_C(4294967295)
#define CA_FAT_ROLL_BYTES (CA_FAT_MAX_BYTES - UINT64_C(67108863))
#define CA_MP4_HEADERS_MAX 65536U

APC_STATIC_ASSERT(sizeof(off_t) >= 8, "Recording requires 64-bit file offsets");

struct ca_mp4_keyframe { uint64_t offset, time; };

struct ca_mp4_file {
    ca_video_telemetry_format format;
    ca_misb_ts misb;
    int fd;
    int directory_fd;
    pthread_t sync_thread;
    pthread_mutex_t sync_lock;
    pthread_cond_t sync_changed;
    bool sync_started;
    bool sync_dirty;
    bool sync_stopping;
    atomic_int sync_error;
    MP4E_mux_t *mux;
    mp4_h26x_writer_t h264;
    unsigned frame_duration;
    uint64_t next_pts90k;
    uint64_t bytes;
    uint64_t hard_limit;
    /* Space reserved after the moov holds a sidx written at close. Without
     * an index covering the whole file, FFmpeg-based players (Chrome among
     * them) walk every fragment before playback starts. */
    uint64_t index_at;   /* muxer offset of the reserved box; 0 before the moov */
    uint64_t shift;      /* bytes inserted there; later muxer offsets move by this */
    uint64_t mvhd_at, tkhd_at, mdhd_at; /* zero-duration headers patched at close */
    uint32_t movie_timescale, media_timescale;
    struct ca_mp4_keyframe *keyframes;
    size_t keyframe_count, keyframe_capacity;
    bool pending_key;
    bool index_failed;
};
#define CA_MP4_INDEX_RESERVE (64U * 1024U)
#define CA_MP4_SIDX_HEADER 40U
#define CA_MP4_SIDX_ENTRY 12U

struct ca_mp4 {
    struct ca_mp4_file *file;
    char *path;
    unsigned width, height, frame_rate, part;
    uint64_t roll_bytes;
    std::vector<uint8_t> headers;
    int error;
    /* At most one completed file can await its final background flush. */
    struct ca_mp4_file *retired;
    pthread_t retire_thread;
    atomic_bool retire_done;
    int retire_error;
};

struct ca_fmp4 {
    MP4E_mux_t *mux;
    mp4_h26x_writer_t h264;
    unsigned frame_duration;
    uint64_t next_pts90k;
    int64_t next_offset;
    ca_fmp4_write_fn write;
    void *opaque;
    bool failed;
};

static uint32_t read32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

static int pwrite_all(int fd, const void *buffer, size_t size, uint64_t at)
{
    const uint8_t *data = (const uint8_t*)(buffer);
    size_t done = 0;
    while (done < size) {
        ssize_t written = pwrite(fd, data + done, size - done, (off_t)(at + done));
        if (written > 0) done += (size_t)written;
        else if (written < 0 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

/* Remember where the version 0 duration fields of the moov live. */
static void locate_headers(struct ca_mp4_file *writer, const uint8_t *moov, size_t size, uint64_t at)
{
    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t box = read32(moov + pos);
        if (box < 8 || box > size - pos) return;
        if (!memcmp(moov + pos + 4, "mvhd", 4) && box >= 28 && moov[pos + 8] == 0) {
            writer->mvhd_at = at + pos;
            writer->movie_timescale = read32(moov + pos + 20);
        } else if (!memcmp(moov + pos + 4, "trak", 4)) {
            size_t p2 = pos + 8;
            while (p2 + 8 <= pos + box) {
                uint32_t b2 = read32(moov + p2);
                if (b2 < 8 || b2 > pos + box - p2) return;
                if (!memcmp(moov + p2 + 4, "tkhd", 4) && b2 >= 32 && moov[p2 + 8] == 0 && !writer->tkhd_at)
                    writer->tkhd_at = at + p2;
                if (!memcmp(moov + p2 + 4, "mdia", 4)) {
                    size_t p3 = p2 + 8;
                    while (p3 + 8 <= p2 + b2) {
                        uint32_t b3 = read32(moov + p3);
                        if (b3 < 8 || b3 > p2 + b2 - p3) return;
                        if (!memcmp(moov + p3 + 4, "mdhd", 4) && b3 >= 28 && moov[p3 + 8] == 0 && !writer->mdhd_at) {
                            writer->mdhd_at = at + p3;
                            writer->media_timescale = read32(moov + p3 + 20);
                        }
                        p3 += b3;
                    }
                }
                p2 += b2;
            }
        }
        pos += box;
    }
}

static void note_keyframe(struct ca_mp4_file *writer, uint64_t offset)
{
    if (writer->index_failed) return;
    if (writer->keyframe_count == writer->keyframe_capacity) {
        size_t capacity = writer->keyframe_capacity ? writer->keyframe_capacity * 2 : 256;
        struct ca_mp4_keyframe *grown = (struct ca_mp4_keyframe*)(realloc(writer->keyframes, capacity * sizeof(*grown)));
        if (grown == NULL) { writer->index_failed = true; return; }
        writer->keyframes = grown;
        writer->keyframe_capacity = capacity;
    }
    writer->keyframes[writer->keyframe_count++] =
        (struct ca_mp4_keyframe){offset, writer->next_pts90k};
}

static int write_at(int64_t offset, const void *buffer, size_t size, void *opaque)
{
    struct ca_mp4_file *writer = (struct ca_mp4_file*)(opaque);
    const uint8_t *data = (const uint8_t*)(buffer);

    if (offset < 0) { errno = EINVAL; return 1; }
    uint64_t at = (uint64_t)offset;
    if (writer->shift && at >= writer->index_at) at += writer->shift;
    if (at > writer->hard_limit || size > writer->hard_limit - at) {
        errno = EFBIG;
        return 1;
    }
    if (pwrite_all(writer->fd, data, size, at) < 0) return 1;
    uint64_t end = at + size;
    if (end > writer->bytes) writer->bytes = end;
    if (size >= 8 && !memcmp(data + 4, "moof", 4) && writer->pending_key) {
        note_keyframe(writer, at);
    } else if (size >= 8 && !memcmp(data + 4, "moov", 4) && !writer->shift) {
        /* reserve the index space as a free box right after the moov */
        uint8_t box[8];
        put32(box, CA_MP4_INDEX_RESERVE);
        memcpy(box + 4, "free", 4);
        if (end + CA_MP4_INDEX_RESERVE > writer->hard_limit) { errno = EFBIG; return 1; }
        if (pwrite_all(writer->fd, box, sizeof(box), end) < 0) return 1;
        locate_headers(writer, data, size, at);
        writer->index_at = end;
        writer->shift = CA_MP4_INDEX_RESERVE;
        writer->bytes = end + CA_MP4_INDEX_RESERVE;
    }
    return 0;
}

/* Shrink the reserved box and end it with a sidx of keyframe-aligned
 * references covering the file, so the first fragment directly follows the
 * index (Chrome ignores an index whose first_offset skips padding). Then
 * give the headers the final duration. */
static int write_index(struct ca_mp4_file *writer)
{
    if (!writer->shift || writer->index_failed || writer->keyframe_count == 0) return 0;
    size_t max_refs = (CA_MP4_INDEX_RESERVE - 8U - CA_MP4_SIDX_HEADER) / CA_MP4_SIDX_ENTRY;
    size_t stride = (writer->keyframe_count + max_refs - 1) / max_refs;
    size_t count = (writer->keyframe_count + stride - 1) / stride;
    size_t sidx_size = CA_MP4_SIDX_HEADER + count * CA_MP4_SIDX_ENTRY;
    uint8_t *box = (uint8_t*)(calloc(1, sidx_size));
    if (box == NULL) return -1;
    put32(box, (uint32_t)sidx_size);
    memcpy(box + 4, "sidx", 4);
    box[8] = 1; /* version 1: 64-bit time and offset */
    put32(box + 12, 1); /* reference_ID: the video track */
    put32(box + 16, writer->media_timescale ? writer->media_timescale : 90000U);
    put64(box + 20, writer->keyframes[0].time);
    put64(box + 28, 0); /* first_offset: the first moof follows directly */
    box[38] = (uint8_t)(count >> 8);
    box[39] = (uint8_t)count;
    uint8_t *entry = box + CA_MP4_SIDX_HEADER;
    for (size_t i = 0; i < count; i++, entry += CA_MP4_SIDX_ENTRY) {
        const struct ca_mp4_keyframe *start = &writer->keyframes[i * stride];
        struct ca_mp4_keyframe next = {writer->bytes, writer->next_pts90k};
        if ((i + 1) * stride < writer->keyframe_count) next = writer->keyframes[(i + 1) * stride];
        uint64_t bytes = next.offset - start->offset, duration = next.time - start->time;
        if (bytes >= 0x80000000ULL || duration > UINT32_MAX) { free(box); return 0; }
        put32(entry, (uint32_t)bytes);
        put32(entry + 4, (uint32_t)duration);
        put32(entry + 8, 0x90000000U); /* starts with a type 1 SAP at delta 0 */
    }
    uint8_t padding[8];
    put32(padding, (uint32_t)(CA_MP4_INDEX_RESERVE - sidx_size));
    memcpy(padding + 4, "free", 4);
    int result = pwrite_all(writer->fd, box, sidx_size,
                            writer->index_at + CA_MP4_INDEX_RESERVE - sidx_size);
    if (result == 0) result = pwrite_all(writer->fd, padding, sizeof(padding), writer->index_at);
    free(box);
    if (result < 0) return -1;
    uint64_t media = writer->next_pts90k;
    uint32_t ts = writer->media_timescale ? writer->media_timescale : 90000U;
    uint64_t movie = media * writer->movie_timescale / ts;
    uint8_t value[4];
    if (writer->mdhd_at && media <= UINT32_MAX) {
        put32(value, (uint32_t)media);
        if (pwrite_all(writer->fd, value, 4, writer->mdhd_at + 24) < 0) return -1;
    }
    if (movie <= UINT32_MAX) {
        put32(value, (uint32_t)movie);
        if (writer->mvhd_at && pwrite_all(writer->fd, value, 4, writer->mvhd_at + 24) < 0) return -1;
        if (writer->tkhd_at && pwrite_all(writer->fd, value, 4, writer->tkhd_at + 28) < 0) return -1;
    }
    return 0;
}

static int recording_directory(const char *path)
{
    char *parent = strdup(path);
    if (parent == NULL) return -1;
    char *slash = strrchr(parent, '/');
    if (slash != NULL) {
        if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
    }
    int fd = open(slash != NULL ? parent : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    return fd;
}

/* The worker never holds the frame-write flock or sync mutex during I/O.
 * Dirty notifications coalesce; a slow card cannot build a queue of flushes.
 * Wait a full interval after completion before starting another background
 * flush. Only normal close waits for this worker and its final flush. */
static void *sync_recording(void *opaque)
{
    struct ca_mp4_file *writer = (struct ca_mp4_file*)(opaque);
    struct timespec next_sync = {};
    bool have_synced = false;
    pthread_mutex_lock(&writer->sync_lock);
    for (;;) {
        while (!writer->sync_stopping && !writer->sync_dirty)
            pthread_cond_wait(&writer->sync_changed, &writer->sync_lock);
        if (!writer->sync_stopping && have_synced) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
                atomic_store(&writer->sync_error, errno);
                break;
            }
            if (now.tv_sec < next_sync.tv_sec ||
                (now.tv_sec == next_sync.tv_sec && now.tv_nsec < next_sync.tv_nsec)) {
#ifdef __APPLE__
                // Darwin condition variables have no selectable clock. Relative
                // waits preserve the monotonic interval through clock changes.
                struct timespec remaining = {next_sync.tv_sec - now.tv_sec,
                                             next_sync.tv_nsec - now.tv_nsec};
                if (remaining.tv_nsec < 0) { remaining.tv_sec--; remaining.tv_nsec += 1000000000; }
                int result = pthread_cond_timedwait_relative_np(&writer->sync_changed,
                                                               &writer->sync_lock, &remaining);
#else
                int result = pthread_cond_timedwait(&writer->sync_changed,
                                                    &writer->sync_lock, &next_sync);
#endif
                if (result != 0 && result != ETIMEDOUT) {
                    atomic_store(&writer->sync_error, result);
                    break;
                }
                continue;
            }
        }
        bool stopping = writer->sync_stopping;
        writer->sync_dirty = false;
        pthread_mutex_unlock(&writer->sync_lock);
        int error = 0;
        if (fdatasync(writer->fd) < 0) error = errno;
        if (error == 0 && writer->directory_fd >= 0) {
            if (fsync(writer->directory_fd) < 0) error = errno;
            else {
                close(writer->directory_fd);
                writer->directory_fd = -1;
            }
        }
        if (error == 0 && clock_gettime(CLOCK_MONOTONIC, &next_sync) < 0)
            error = errno;
        next_sync.tv_sec++;
        have_synced = true;
        pthread_mutex_lock(&writer->sync_lock);
        if (error != 0) atomic_store(&writer->sync_error, error);
        if (error != 0 || stopping) break;
    }
    pthread_mutex_unlock(&writer->sync_lock);
    return NULL;
}

static int start_recording_sync(struct ca_mp4_file *writer)
{
    pthread_condattr_t attr;
    int result = pthread_mutex_init(&writer->sync_lock, NULL);
    if (result != 0) { errno = result; return -1; }
    result = pthread_condattr_init(&attr);
    if (result != 0) goto fail_mutex;
#ifndef __APPLE__
    result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
#endif
    if (result == 0) result = pthread_cond_init(&writer->sync_changed, &attr);
    pthread_condattr_destroy(&attr);
    if (result != 0) goto fail_mutex;
    result = pthread_create(&writer->sync_thread, NULL, sync_recording, writer);
    if (result != 0) {
        pthread_cond_destroy(&writer->sync_changed);
        goto fail_mutex;
    }
    writer->sync_started = true;
    return 0;
fail_mutex:
    pthread_mutex_destroy(&writer->sync_lock);
    errno = result;
    return -1;
}

static int file_open(struct ca_mp4_file **result, const char *path,
                     unsigned width, unsigned height, unsigned frame_rate)
{
    struct ca_mp4_file *writer;

    if (result == NULL || path == NULL || width == 0U || height == 0U ||
        frame_rate == 0U) {
        errno = EINVAL;
        return -1;
    }
    writer = new (std::nothrow) ca_mp4_file{};
    if (writer == NULL) return -1;
    atomic_init(&writer->sync_error, 0);
    writer->directory_fd = recording_directory(path);
    if (writer->directory_fd < 0) {
        delete writer;
        return -1;
    }
    writer->fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (writer->fd < 0) {
        close(writer->directory_fd);
        delete writer;
        return -1;
    }
    struct statfs fs;
    if (fstatfs(writer->fd, &fs) < 0) {
        int saved_errno = errno;
        close(writer->fd);
        close(writer->directory_fd);
        unlink(path);
        delete writer;
        errno = saved_errno;
        return -1;
    }
#ifdef __APPLE__
    writer->hard_limit = !strcmp(fs.f_fstypename, "msdos") ? CA_FAT_MAX_BYTES : INT64_MAX;
#else
    writer->hard_limit = fs.f_type == CA_MSDOS_SUPER_MAGIC ? CA_FAT_MAX_BYTES : INT64_MAX;
#endif
    writer->frame_duration = 90000U / frame_rate;
    writer->format = ca_video_metadata_format();
    writer->misb.width=width; writer->misb.height=height;
    /* Append-only fragments keep completed frames playable before close and
     * after a crash; the movie header is emitted with the first video frame. */
    if(writer->format!=CA_VIDEO_TELEMETRY_MISB)
        writer->mux = MP4E_open(1, 1, writer, write_at);
    if ((writer->format!=CA_VIDEO_TELEMETRY_MISB && (writer->mux == NULL ||
        mp4_h26x_write_init(&writer->h264, writer->mux,
                            (int)width, (int)height, 0) != MP4E_STATUS_OK)) ||
        start_recording_sync(writer) < 0) {
        int saved_errno = errno != 0 ? errno : EIO;
        if (writer->mux != NULL) (void)MP4E_close(writer->mux);
        close(writer->fd);
        close(writer->directory_fd);
        unlink(path);
        delete writer;
        errno = saved_errno;
        return -1;
    }
    *result = writer;
    return 0;
}

/* Each call is one complete encoder access unit, including all slices and
 * SEI. Use MP4E_put_sample once: treating each NAL as a picture breaks SEI
 * association and fragmented MP4 with multi-slice encoders. */
static int write_h264(mp4_h26x_writer_t *h264, unsigned frame_duration,
                      const uint8_t *annex_b, size_t length, uint64_t pts90k,
                      char *json, size_t *json_length, float hfov_deg)
{
    size_t offset = 0U, nal, nal_length, used = 0U;
    bool picture = false, key = false;
    uint8_t sei[CA_VIDEO_METADATA_SEI_MAX];
    struct ca_metadata metadata;
    struct timespec utc;
    if (annex_b == NULL || length == 0U || length > INT_MAX - sizeof(sei)) {
        errno = EINVAL;
        return -1;
    }
    while (ca_annexb_next(annex_b, length, &offset, &nal, &nal_length)) {
        if (nal_length == 0U) continue;
        unsigned type = annex_b[nal] & 31U;
        int result = MP4E_STATUS_OK;
        if (type == 7U) {
            result = MP4E_set_sps(h264->mux, h264->mux_track_id, annex_b + nal, (int)nal_length);
            h264->need_sps = 0;
        } else if (type == 8U) {
            result = MP4E_set_pps(h264->mux, h264->mux_track_id, annex_b + nal, (int)nal_length);
            h264->need_pps = 0;
        } else if (ca_video_nal_is_vcl(CA_VIDEO_H264, annex_b[nal])) {
            picture = true;
            if (type == 5U) key = true;
        }
        if (result != MP4E_STATUS_OK) { errno = EIO; return -1; }
    }
    if (!picture) return 0;
    if (h264->need_sps || h264->need_pps || (h264->need_idr && !key)) return 0;
    ca_metadata_snapshot(&metadata);
    metadata.hfov_deg = hfov_deg;
    if (clock_gettime(CLOCK_REALTIME, &utc) < 0) return -1;
    *json_length = ca_video_metadata_json(&metadata, &utc, pts90k,
                                         json, CA_VIDEO_METADATA_JSON_MAX);
    size_t sei_length = ca_video_metadata_sei(CA_VIDEO_H264, json, *json_length,
                                              sei, sizeof(sei));
    if (sei_length == 0U) { errno = EOVERFLOW; return -1; }
    /* Three-byte Annex-B start codes expand by one byte in MP4. */
    if (length > (INT_MAX - sizeof(sei)) / 2U) { errno = EOVERFLOW; return -1; }
    uint8_t *sample = (uint8_t*)(malloc(length * 2U + sizeof(sei)));
    if (sample == NULL) return -1;
#define APPEND_NAL(data, size) do { \
    size_t n = (size); \
    sample[used++] = (uint8_t)(n >> 24); sample[used++] = (uint8_t)(n >> 16); \
    sample[used++] = (uint8_t)(n >> 8); sample[used++] = (uint8_t)n; \
    memcpy(sample + used, (data), n); used += n; \
} while (0)
    bool inserted = false;
    offset = 0U;
    while (ca_annexb_next(annex_b, length, &offset, &nal, &nal_length)) {
        if (nal_length == 0U) continue;
        unsigned type = annex_b[nal] & 31U;
        bool vcl = ca_video_nal_is_vcl(CA_VIDEO_H264, annex_b[nal]);
        if (vcl && !inserted) {
            APPEND_NAL(sei + 4U, sei_length - 4U);
            inserted = true;
        }
        if (vcl || type == 6U) APPEND_NAL(annex_b + nal, nal_length);
    }
#undef APPEND_NAL
    int result = MP4E_put_sample(h264->mux, h264->mux_track_id,
                                 sample, (int)used, (int)frame_duration,
                                 key ? MP4E_SAMPLE_RANDOM_ACCESS : MP4E_SAMPLE_DEFAULT);
    free(sample);
    if (result != MP4E_STATUS_OK) { errno = EIO; return -1; }
    h264->need_idr = 0;
    return 1;
}

static int write_ts(void *opaque,const void *data,size_t length)
{
    auto *writer=static_cast<ca_mp4_file *>(opaque);
    if(length>writer->hard_limit-writer->bytes) { errno=EFBIG; return -1; }
    if(pwrite_all(writer->fd,static_cast<const uint8_t *>(data),length,writer->bytes)<0) return -1;
    writer->bytes+=length;
    return 0;
}

static int write_recording_frame(struct ca_mp4_file *writer, const uint8_t *annex_b,
                      size_t length, uint64_t pts, bool key_frame, float hfov_deg)
{
    /* The encoders provide platform-specific PTS. MP4 has always used the
     * configured constant frame rate; telemetry uses that same sample clock. */
    (void)pts;
    (void)key_frame;
    char json[CA_VIDEO_METADATA_JSON_MAX];
    size_t json_length = 0U;
    if (writer == NULL) { errno = EINVAL; return -1; }
    if(writer->format==CA_VIDEO_TELEMETRY_MISB) {
        ca_metadata metadata;
        timespec utc;
        ca_metadata_snapshot(&metadata); metadata.hfov_deg=hfov_deg;
        if(clock_gettime(CLOCK_REALTIME,&utc)<0) return -1;
        std::vector<uint8_t> bytes;
        const auto collect=[](void *opaque,const void *data,size_t size) {
            auto &out=*static_cast<std::vector<uint8_t> *>(opaque);
            const auto *p=static_cast<const uint8_t *>(data);
            out.insert(out.end(),p,p+size);
            return 0;
        };
        int result=ca_misb_ts_write(&writer->misb,CA_VIDEO_H264,annex_b,length,
            writer->next_pts90k,key_frame,&metadata,&utc,collect,&bytes);
        if(result>0 && write_ts(writer,bytes.data(),bytes.size())<0) return -1;
        if(result>0) writer->next_pts90k+=writer->frame_duration;
        return result;
    }
    /* index the fragments that start with an IDR, as the muxer marks them */
    size_t offset = 0U, nal, nal_length;
    writer->pending_key = false;
    while (ca_annexb_next(annex_b, length, &offset, &nal, &nal_length)) {
        if (nal_length != 0U && (annex_b[nal] & 31U) == 5U) writer->pending_key = true;
    }
    int result = write_h264(&writer->h264, writer->frame_duration, annex_b, length,
                            writer->next_pts90k, json, &json_length, hfov_deg);
    if (result <= 0) return result;
    writer->next_pts90k += writer->frame_duration;
    return 1;
}

static int file_write_h264(struct ca_mp4_file *writer, const uint8_t *annex_b,
                      size_t length, uint64_t pts, bool key_frame, float hfov_deg)
{
    if (writer == NULL) { errno = EINVAL; return -1; }
    int sync_error = atomic_load(&writer->sync_error);
    if (sync_error != 0) { errno = sync_error; return -1; }
    /* The Files endpoint shares this lock only while taking its size snapshot,
     * so downloads end at a complete fragment without delaying recording. */
    if (flock(writer->fd, LOCK_EX) < 0) return -1;
    int result = write_recording_frame(writer, annex_b, length, pts, key_frame, hfov_deg);
    int saved_errno = errno;
    if (flock(writer->fd, LOCK_UN) < 0 && result >= 0) return -1;
    if (result > 0) {
        pthread_mutex_lock(&writer->sync_lock);
        if (!writer->sync_dirty) {
            writer->sync_dirty = true;
            pthread_cond_signal(&writer->sync_changed);
        }
        pthread_mutex_unlock(&writer->sync_lock);
    }
    errno = saved_errno;
    return result < 0 ? -1 : 0;
}

static int file_close(struct ca_mp4_file *writer)
{
    int result = 0;
    if (writer == NULL) return 0;
    if(writer->mux) {
        mp4_h26x_write_close(&writer->h264);
        if (MP4E_close(writer->mux) != MP4E_STATUS_OK) result = -1;
        if (result == 0 && write_index(writer) < 0) result = -1;
    }
    free(writer->keyframes);
    writer->keyframes = NULL;
    if (writer->sync_started) {
        pthread_mutex_lock(&writer->sync_lock);
        writer->sync_stopping = true;
        pthread_cond_signal(&writer->sync_changed);
        pthread_mutex_unlock(&writer->sync_lock);
        pthread_join(writer->sync_thread, NULL);
        pthread_cond_destroy(&writer->sync_changed);
        pthread_mutex_destroy(&writer->sync_lock);
    }
    int sync_error = atomic_load(&writer->sync_error);
    if (sync_error != 0) result = -1;
    if (close(writer->fd) < 0) result = -1;
    if (writer->directory_fd >= 0) close(writer->directory_fd);
    delete writer;
    if (sync_error != 0) errno = sync_error;
    return result;
}

/* Retain all distinct parameter sets, including headers sent separately from
 * IDRs. A continuation must decode even when the encoder never repeats them. */
static int cache_headers(struct ca_mp4 *writer, const uint8_t *data, size_t length,
                         bool *idr)
{
    size_t offset = 0, nal, n;
    *idr = false;
    while (ca_annexb_next(data, length, &offset, &nal, &n)) {
        if (!n) continue;
        unsigned type = data[nal] & 31U;
        if (type == 5U) *idr = true;
        if (type != 7U && type != 8U) continue;
        if (ca_video_cache_parameter_set(writer->headers, CA_VIDEO_H264, data + nal, n) < 0)
            return -1;
    }
    return 0;
}

static void *retire_file(void *opaque)
{
    struct ca_mp4 *writer = (struct ca_mp4*)(opaque);
    writer->retire_error = file_close(writer->retired) < 0 ? (errno ? errno : EIO) : 0;
    atomic_store(&writer->retire_done, true);
    return NULL;
}

static int reap_file(struct ca_mp4 *writer, bool wait)
{
    if (!writer->retired) return 0;
    if (!wait && !atomic_load(&writer->retire_done)) return 1;
    pthread_join(writer->retire_thread, NULL);
    writer->retired = NULL;
    if (writer->retire_error) { errno = writer->retire_error; return -1; }
    return 0;
}

static int roll_file(struct ca_mp4 *writer)
{
    char *path = NULL;
    const char *slash = strrchr(writer->path, '/');
    const char *extension = strrchr(writer->path, '.');
    if (!extension || (slash && extension < slash)) extension = writer->path + strlen(writer->path);
    size_t stem = (size_t)(extension - writer->path);
    if (stem > INT_MAX || writer->part == UINT_MAX) { errno = EOVERFLOW; return -1; }
    if (asprintf(&path, "%.*s_part%04u%s", (int)stem, writer->path,
                 writer->part + 1U, extension) < 0) return -1;
    struct ca_mp4_file *next = NULL;
    if (file_open(&next, path, writer->width, writer->height, writer->frame_rate) < 0) {
        free(path);
        return -1;
    }
    {
    /* Seed the decoder configuration without adding a video sample or SEI. */
    if (!writer->headers.empty() &&
        file_write_h264(next, writer->headers.data(), writer->headers.size(), 0, false, 0) < 0)
        goto fail;
    writer->retired = writer->file;
    atomic_store(&writer->retire_done, false);
    int error = pthread_create(&writer->retire_thread, NULL, retire_file, writer);
    if (error) {
        writer->retired = NULL;
        errno = error;
        goto fail;
    }
    writer->file = next;
    writer->part++;
    fprintf(stderr, "recording continued in %s\n", path);
    free(path);
    return 0;
    }
fail:;
    int saved_errno = errno;
    (void)file_close(next);
    unlink(path);
    free(path);
    errno = saved_errno;
    return -1;
}

int ca_mp4_open(struct ca_mp4 **result, const char *path,
                unsigned width, unsigned height, unsigned frame_rate)
{
    if (!result || !path || !width || !height || !frame_rate) { errno = EINVAL; return -1; }
    struct ca_mp4 *writer = new (std::nothrow) ca_mp4{};
    if (!writer) return -1;
    writer->path = strdup(path);
    if (!writer->path) { delete writer; return -1; }
    if (file_open(&writer->file, path, width, height, frame_rate) < 0) {
        int saved_errno = errno;
        free(writer->path);
        delete writer;
        errno = saved_errno;
        return -1;
    }
    writer->width = width;
    writer->height = height;
    writer->frame_rate = frame_rate;
    writer->part = 1;
    atomic_init(&writer->retire_done, false);
    if (writer->file->hard_limit == CA_FAT_MAX_BYTES) {
        writer->roll_bytes = CA_FAT_ROLL_BYTES;
        /* A lower FAT threshold permits bounded bench tests. It cannot enable
         * splitting on exFAT or raise the production safety threshold. */
        const char *setting = getenv("CAMERA_APP_RECORD_SEGMENT_BYTES");
        if (setting && *setting >= '0' && *setting <= '9') {
            char *end;
            errno = 0;
            unsigned long long bytes = strtoull(setting, &end, 10);
            if (!errno && !*end && bytes >= 65536 && bytes < writer->roll_bytes)
                writer->roll_bytes = bytes;
        }
    }
    *result = writer;
    return 0;
}

int ca_mp4_write_h264(struct ca_mp4 *writer, const uint8_t *annex_b,
                      size_t length, uint64_t pts, bool key_frame, float hfov_deg)
{
    if (!writer || !annex_b || !length || length > INT_MAX / 2U) { errno = EINVAL; return -1; }
    if (writer->error) { errno = writer->error; return -1; }
    int retired = reap_file(writer, false);
    if (retired < 0) goto fail;
    if (writer->roll_bytes) {
        bool idr;
        if (cache_headers(writer, annex_b, length, &idr) < 0) goto fail;
        if (idr && writer->file->bytes >= writer->roll_bytes && retired == 0) {
            if (roll_file(writer) < 0) goto fail;
        }
    }
    /* Refuse an access unit before writing any of it if even the conservative
     * size bound reaches the filesystem limit. Broken encoders without IDRs
     * and indefinitely blocked final syncs must not grow a file past FAT32.
     * Include start-code expansion, telemetry, decoder headers and MP4 boxes. */
    {
    uint64_t bound = (uint64_t)length * 2U + CA_VIDEO_METADATA_SEI_MAX +
                     CA_MP4_HEADERS_MAX + 4096U;
    if (bound > writer->file->hard_limit - writer->file->bytes) {
        errno = EFBIG;
        goto fail;
    }
    if (file_write_h264(writer->file, annex_b, length, pts, key_frame, hfov_deg) < 0) goto fail;
    return 0;
    }
fail:
    writer->error = errno ? errno : EIO;
    errno = writer->error;
    return -1;
}

int ca_mp4_close(struct ca_mp4 *writer)
{
    if (!writer) return 0;
    int error = writer->error;
    if (file_close(writer->file) < 0 && !error) error = errno ? errno : EIO;
    if (reap_file(writer, true) < 0 && !error) error = errno ? errno : EIO;
    free(writer->path);
    delete writer;
    if (error) { errno = error; return -1; }
    return 0;
}

static int stream_write_at(int64_t offset, const void *buffer, size_t size,
                           void *opaque)
{
    struct ca_fmp4 *writer = (struct ca_fmp4*)(opaque);

    if (writer->failed || offset != writer->next_offset ||
        size > (size_t)(INT64_MAX - writer->next_offset) ||
        writer->write(writer->opaque, buffer, size) != 0) {
        writer->failed = true;
        return 1;
    }
    writer->next_offset += (int64_t)size;
    return 0;
}

int ca_fmp4_open(struct ca_fmp4 **result, unsigned width, unsigned height,
                 unsigned frame_rate, ca_fmp4_write_fn write, void *opaque)
{
    struct ca_fmp4 *writer;

    if (result == NULL || width == 0U || height == 0U || frame_rate == 0U ||
        write == NULL) {
        errno = EINVAL;
        return -1;
    }
    writer = new (std::nothrow) ca_fmp4{};
    if (writer == NULL) return -1;
    writer->frame_duration = 90000U / frame_rate;
    writer->write = write;
    writer->opaque = opaque;
    writer->mux = MP4E_open(1, 1, writer, stream_write_at);
    if (writer->mux == NULL || writer->failed ||
        mp4_h26x_write_init(&writer->h264, writer->mux,
                            (int)width, (int)height, 0) != MP4E_STATUS_OK) {
        int saved_errno = errno != 0 ? errno : EIO;
        if (writer->mux != NULL) (void)MP4E_close(writer->mux);
        delete writer;
        errno = saved_errno;
        return -1;
    }
    *result = writer;
    return 0;
}

int ca_fmp4_write_h264(struct ca_fmp4 *writer, const uint8_t *annex_b,
                       size_t length, uint64_t pts, bool key_frame, float hfov_deg)
{
    (void)pts;
    (void)key_frame;
    if (writer == NULL || writer->failed) {
        errno = writer != NULL && writer->failed ? EPIPE : EINVAL;
        return -1;
    }
    char json[CA_VIDEO_METADATA_JSON_MAX];
    size_t json_length;
    int result = write_h264(&writer->h264, writer->frame_duration, annex_b, length,
                            writer->next_pts90k, json, &json_length, hfov_deg);
    if (result > 0) writer->next_pts90k += writer->frame_duration;
    return result < 0 ? -1 : 0;
}

int ca_fmp4_close(struct ca_fmp4 *writer)
{
    int result = 0;

    if (writer == NULL) return 0;
    mp4_h26x_write_close(&writer->h264);
    if (MP4E_close(writer->mux) != MP4E_STATUS_OK || writer->failed) {
        result = -1;
        errno = EIO;
    }
    delete writer;
    return result;
}
