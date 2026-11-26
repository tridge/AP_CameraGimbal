#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool hold_sync = true;
static bool fail_sync;
static unsigned sync_calls;
static pthread_t frame_thread;
static atomic_bool closed;
static int close_result;
static bool fat;
static bool hold_first_only;
static int first_sync_fd = -1;

extern "C" int __real_fstatfs64(int fd, struct statfs64 *fs);
extern "C" int __wrap_fstatfs64(int fd, struct statfs64 *fs)
{
    int result = __real_fstatfs64(fd, fs);
    if (result == 0 && fat) fs->f_type = 0x4d44;
    return result;
}

extern "C" int __real_fdatasync(int fd);
extern "C" int __wrap_fdatasync(int fd)
{
    /* A blocked storage operation must never run on the frame producer. */
    assert(!pthread_equal(pthread_self(), frame_thread));
    pthread_mutex_lock(&lock);
    if (first_sync_fd < 0) first_sync_fd = fd;
    sync_calls++;
    pthread_cond_broadcast(&changed);
    while (hold_sync && (!hold_first_only || fd == first_sync_fd))
        pthread_cond_wait(&changed, &lock);
    bool fail = fail_sync;
    pthread_mutex_unlock(&lock);
    if (fail) { errno = EIO; return -1; }
    return __real_fdatasync(fd);
}

static void wait_for_sync(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&lock);
    while (sync_calls == 0)
        assert(pthread_cond_timedwait(&changed, &lock, &deadline) == 0);
    pthread_mutex_unlock(&lock);
}

static void *close_writer(void *opaque)
{
    close_result = ca_mp4_close((struct ca_mp4*)(opaque));
    atomic_store(&closed, true);
    return NULL;
}

static void release_sync(void)
{
    pthread_mutex_lock(&lock);
    hold_sync = false;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

int main(int argc, char **argv)
{
    struct stat st;
    assert((argc == 2 || argc == 3) && stat(argv[1], &st) == 0 && st.st_size > 0);
    if(argc==3) ca_video_metadata_set_format(CA_VIDEO_TELEMETRY_MISB);
    uint8_t *data = (uint8_t*)(malloc((size_t)st.st_size));
    assert(data != NULL);
    int input = open(argv[1], O_RDONLY);
    assert(input >= 0 && read(input, data, (size_t)st.st_size) == st.st_size);
    close(input);
    size_t frame_length = (size_t)st.st_size;
    size_t scan = 0, nal, length;
    while (ca_annexb_next(data, frame_length, &scan, &nal, &length)) {
        if (length && (data[nal] & 31U) == 9U && nal > 4U) {
            frame_length = nal - (data[nal - 4U] == 0 ? 4U : 3U);
            break;
        }
    }
    char directory[] = "/tmp/mp4-sync-test-XXXXXX";
    assert(mkdtemp(directory) != NULL);
    char path[256];
    assert(snprintf(path, sizeof(path), "%s/video.mp4", directory) > 0);
    frame_thread = pthread_self();
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    assert(ca_mp4_write_h264(writer, data, frame_length, 0, true, 88) == 0);
    wait_for_sync();
    /* Keep the sync blocked for longer than the normal interval while many
     * frames arrive. No frame call may wait for it or spawn additional syncs. */
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000};
    for (unsigned i = 0; i < 60; i++) {
        assert(ca_mp4_write_h264(writer, data, frame_length, 0, true, 88) == 0);
        nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&lock);
    assert(sync_calls == 1);
    pthread_mutex_unlock(&lock);
    /* Files downloads can snapshot the file even during slow sync; the frames
     * since then wait in memory, not in the frame producer. */
    int snapshot = open(path, O_RDONLY);
    assert(snapshot >= 0 && flock(snapshot, LOCK_SH | LOCK_NB) == 0);
    assert(fstat(snapshot, &st) == 0 && st.st_size > 0 && st.st_size < (off_t)(30 * frame_length));
    assert(flock(snapshot, LOCK_UN) == 0);
    pthread_t closer;
    assert(pthread_create(&closer, NULL, close_writer, writer) == 0);
    nanosleep(&delay, NULL);
    assert(!atomic_load(&closed));
    release_sync();
    assert(pthread_join(closer, NULL) == 0);
    assert(close_result == 0 && atomic_load(&closed));
    assert(sync_calls == 2); /* in-flight flush, then one final flush; no backlog */
    assert(fstat(snapshot, &st) == 0 && st.st_size > (off_t)(60 * frame_length));
    close(snapshot);
    unlink(path);

    /* Rollover must keep video moving even when the old file's final sync is
     * stuck. Only one retired file is retained; there is no unbounded queue. */
    fat = true;
    hold_first_only = true;
    first_sync_fd = -1;
    hold_sync = true;
    sync_calls = 0;
    assert(setenv("CAMERA_APP_RECORD_SEGMENT_BYTES", "65536", 1) == 0);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    assert(ca_mp4_write_h264(writer, data, frame_length, 0, true, 88) == 0);
    wait_for_sync();
    for (unsigned i = 0; i < 120; i++)
        assert(ca_mp4_write_h264(writer, data, frame_length, 0, true, 88) == 0);
    char part2[256], part3[256];
    snprintf(part2, sizeof(part2), "%s/video_part0002.mp4", directory);
    snprintf(part3, sizeof(part3), "%s/video_part0003.mp4", directory);
    assert(stat(part2, &st) == 0);
    assert(access(part3, F_OK) < 0 && errno == ENOENT);
    release_sync();
    assert(ca_mp4_close(writer) == 0);
    assert(stat(part2, &st) == 0 && st.st_size > 65536);
    unlink(path); unlink(part2);
    fat = false;
    hold_first_only = false;
    unsetenv("CAMERA_APP_RECORD_SEGMENT_BYTES");

    /* A card that stalls for longer than the limit costs frames, not the
     * recording: the producer never waits and writing resumes at an IDR. */
    hold_sync = true;
    sync_calls = 0;
    assert(setenv("CAMERA_APP_RECORD_STALL_MS", "1500", 1) == 0);
    /* The encoder may deliver SPS/PPS apart from the IDR that needs them:
     * recording must still resume, so split the access unit that way. */
    uint8_t *headers = (uint8_t*)(malloc(frame_length)), *picture = (uint8_t*)(malloc(frame_length + 16));
    size_t headers_length = 0, picture_length = 0;
    assert(headers && picture);
    scan = 0;
    while (ca_annexb_next(data, frame_length, &scan, &nal, &length)) {
        if (!length) continue;
        unsigned type = data[nal] & 31U;
        uint8_t *out = type == 7U || type == 8U ? headers : picture;
        size_t *used = type == 7U || type == 8U ? &headers_length : &picture_length;
        memcpy(out + *used, "\0\0\0\1", 4); *used += 4;
        memcpy(out + *used, data + nal, length); *used += length;
    }
    assert(headers_length > 8 && picture_length > 8);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    assert(ca_mp4_write_h264(writer, data, frame_length, 0, true, 88) == 0);
    wait_for_sync();
    for (unsigned i = 0; i < 150; i++) {
        assert(ca_mp4_write_h264(writer, headers, headers_length, 0, false, 88) == 0);
        assert(ca_mp4_write_h264(writer, picture, picture_length, 0, true, 88) == 0);
        nanosleep(&delay, NULL);
    }
    release_sync();
    /* the queue drains within a write interval; frames then record again */
    const struct timespec settle = {.tv_sec = 1, .tv_nsec = 300000000};
    nanosleep(&settle, NULL);
    for (unsigned i = 0; i < 5; i++) {
        assert(ca_mp4_write_h264(writer, headers, headers_length, 0, false, 88) == 0);
        assert(ca_mp4_write_h264(writer, picture, picture_length, 0, true, 88) == 0);
    }
    free(headers); free(picture);
    assert(ca_mp4_close(writer) == 0);
    /* about 1.5 s of the 3 s stall was kept, then the last five frames */
    assert(stat(path, &st) == 0 && st.st_size > (off_t)(60 * frame_length) &&
           st.st_size < (off_t)(120 * frame_length));
    unlink(path);
    unsetenv("CAMERA_APP_RECORD_STALL_MS");

    /* A first frame that arrives long after open is not a stalled card: the
     * header queued at open must not count against the stall limit. */
    hold_sync = false;
    sync_calls = 0;
    assert(setenv("CAMERA_APP_RECORD_STALL_MS", "1000", 1) == 0);
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    const struct timespec late_start = {.tv_sec = 1, .tv_nsec = 300000000};
    nanosleep(&late_start, NULL);
    for (unsigned i = 0; i < 10; i++)
        assert(ca_mp4_write_h264(writer, data, frame_length, 0, true, 88) == 0);
    /* the first sync is done; the index written at close needs another */
    wait_for_sync();
    nanosleep(&delay, NULL);
    pthread_mutex_lock(&lock);
    unsigned before_close = sync_calls;
    pthread_mutex_unlock(&lock);
    assert(ca_mp4_close(writer) == 0);
    /* MPEG-TS has no index, so its close only syncs if frames were pending */
    assert(argc == 3 ? sync_calls >= before_close : sync_calls == before_close + 1);
    assert(stat(path, &st) == 0 && st.st_size > (off_t)(10 * frame_length));
    unlink(path);
    unsetenv("CAMERA_APP_RECORD_STALL_MS");

    /* An asynchronous I/O failure must still reach the caller at close. */
    sync_calls = 0;
    hold_sync = true;
    fail_sync = true;
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    assert(ca_mp4_write_h264(writer, data, frame_length, 0, true, 88) == 0);
    wait_for_sync();
    release_sync();
    errno = 0;
    assert(ca_mp4_close(writer) < 0 && errno == EIO);
    unlink(path);

    /* Closing before the first frame also shuts down the waiting worker. */
    fail_sync = false;
    sync_calls = 0;
    assert(ca_mp4_open(&writer, path, 320, 240, 10) == 0);
    assert(ca_mp4_close(writer) == 0 && sync_calls == 1);
    unlink(path);
    rmdir(directory);
    free(data);
    puts("PASS blocked background sync: frame progress, bounded requests, download locks, stall drops, close and I/O failure");
    return 0;
}
