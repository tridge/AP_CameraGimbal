#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include "apcam/atomic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static long filesystem;
static bool sparse;
extern "C" int __wrap_fstatfs64(int fd, struct statfs64 *fs)
{
    (void)fd;
    if (filesystem == -1) { errno = EIO; return -1; }
    memset(fs, 0, sizeof(*fs)); fs->f_type = filesystem;
    return 0;
}
extern "C" ssize_t __real_pwrite64(int fd, const void *data, size_t size, off64_t offset);
extern "C" ssize_t __wrap_pwrite64(int fd, const void *data, size_t size, off64_t offset)
{
    /* Exercise real 64-bit offsets and FAT boundaries without storing GiBs of
     * padding. Small-file tests below use unmodified, decodable H.264 samples. */
    if (filesystem == 0x4d44) assert((uint64_t)offset + size <= UINT32_MAX);
    if (sparse && size > 1024*1024) {
        if (ftruncate64(fd, offset + size) < 0) return -1;
        return (ssize_t)size;
    }
    return __real_pwrite64(fd, data, size, offset);
}

int main(int argc, char **argv)
{
    assert(argc == 5 || argc == 6);
    if(argc==6) ca_video_metadata_set_format(CA_VIDEO_TELEMETRY_MISB);
    filesystem = strtol(argv[3], NULL, 0);
    sparse = !strcmp(argv[4], "large") || !strcmp(argv[4], "no-idr");
    bool expected_failure = !strcmp(argv[4], "collision") || filesystem == -1;
    struct stat st; assert(stat(argv[1], &st) == 0 && st.st_size > 0);
    size_t size = (size_t)st.st_size;
    uint8_t *data = (uint8_t*)(malloc(size)); assert(data);
    int fd = open(argv[1], O_RDONLY); assert(fd >= 0);
    assert(read(fd, data, size) == (ssize_t)size); close(fd);
    struct ca_mp4 *writer = NULL;
    int result = ca_mp4_open(&writer, argv[2], 320, 240, 10);
    if (filesystem == -1) { assert(result < 0 && errno == EIO); free(data); return 0; }
    assert(result == 0);
    size_t start = 0, scan = 0, nal, n;
    unsigned frame = 0;
    while (start < size) {
        size_t end = size;
        while (ca_annexb_next(data, size, &scan, &nal, &n)) {
            if (n && (data[nal] & 31) == 9 && nal > start + 4) {
                end = nal - (data[nal-4] == 0 ? 4 : 3);
                break;
            }
        }
        /* Strip repeated SPS/PPS: only the first access unit supplies them.
         * Deliberately lie in key_frame; rollover must inspect actual IDRs. */
        size_t at = start, used = 0;
        uint8_t *au = (uint8_t*)(malloc(end - start + 4096)); assert(au);
        while (ca_annexb_next(data, end, &at, &nal, &n)) {
            unsigned type = data[nal] & 31;
            if (frame && (type == 7 || type == 8)) continue;
            memcpy(au + used, "\0\0\0\1", 4); used += 4;
            memcpy(au + used, data + nal, n); used += n;
        }
        if (sparse) {
            size_t large = 16U * 1024U * 1024U;
            au = (uint8_t*)(realloc(au, used + large)); assert(au);
            memcpy(au + used, "\0\0\0\1\6", 5);
            memset(au + used + 5, 0xff, large - 5); used += large;
            /* One first IDR is enough; subsequent repeated access units only
             * need represent encoder traffic for the sparse boundary test. */
            for (unsigned i = 0; i < 260; i++) {
                if (i == 1 && !strcmp(argv[4], "no-idr")) {
                    size_t a = 0, b, c;
                    while (ca_annexb_next(au, used, &a, &b, &c))
                        if (c && (au[b] & 31) == 5) au[b] = (au[b] & 0xe0) | 1;
                }
                result = ca_mp4_write_h264(writer, au, used, i*9000U, false, 54.7f);
                if (result < 0) break;
            }
            free(au);
            if (!strcmp(argv[4], "no-idr")) assert(result < 0 && errno == EFBIG);
            else assert(result == 0);
            assert(ca_mp4_close(writer) == (result < 0 ? -1 : 0));
            free(data); return 0;
        }
        ca_metadata_set_position(-353632610 + (int32_t)frame, 1491652300, 620, 30, 0);
        ca_metadata_set_vehicle_attitude(.1f, -.2f, .3f);
        ca_metadata_set_gimbal_attitude(0, -.5f, .25f);
        result = ca_mp4_write_h264(writer, au, used, frame*9000U, false, 54.7f);
        free(au);
        if (result < 0) break;
        frame++; start = end;
        /* Give the real asynchronous final flush an opportunity to complete. */
        usleep(1000);
    }
    if (expected_failure) {
        assert(result < 0 && errno == EEXIST);
        assert(ca_mp4_write_h264(writer, data, size, 0, true, 54.7f) < 0 && errno == EEXIST);
        assert(ca_mp4_close(writer) < 0 && errno == EEXIST);
    } else {
        assert(result == 0 && ca_mp4_close(writer) == 0);
        printf("frames=%u\n", frame);
    }
    free(data);
    return 0;
}
