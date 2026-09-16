#define _GNU_SOURCE
#include "../src/backends/z1mini/native.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static atomic_bool stopped;
static unsigned count, exposure_count, stop_after=2;
static void exposure(void *unused, const struct ca_exposure *s)
{
    (void)unused;
    assert(s->time_us==1234567 && s->shutter_us==10000 && s->analog_gain==2);
    assert(s->valid==(CA_AE_SHUTTER|CA_AE_AGAIN));
    exposure_count++;
}
static void frame(void *unused, const uint8_t *data, size_t length, uint64_t pts, bool key, unsigned stream)
{
    (void)unused;
    assert(length == 5 && !memcmp(data, "\0\0\0\1\x65", 5));
    assert(pts == 1234567 && key);
    assert(stream == (count & 1U));
    count++;
    if (count == stop_after) atomic_store(&stopped, true);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "fd:3")) {
        const char *mode = getenv("Z1_NATIVE_TEST_MODE");
        bool valid=!strcmp(mode,"valid") || !strcmp(mode,"overlay");
        if (!strcmp(mode,"overlay")) {
            uint8_t request;
            if (read(3,&request,1)!=1 || request!=1) return 1;
            struct ca_z1_native_header reply={CA_Z1_NATIVE_OVERLAY_MAGIC,0,0,0,request};
            if (write(3,&reply,sizeof(reply))!=sizeof(reply)) return 1;
        }
        if (!strcmp(mode,"retry")) {
            uint8_t request;
            if (read(3,&request,1)!=1 || request!=1) return 1;
            struct ca_z1_native_header frame_header={CA_Z1_NATIVE_MAGIC,5,1234567,1,0};
            for (unsigned i=0;i<190;i++) {
                frame_header.stream=i&1U;
                if (write(3,&frame_header,sizeof(frame_header))!=sizeof(frame_header) ||
                    write(3,"\0\0\0\1\x65",5)!=5) return 1;
            }
            if (read(3,&request,1)!=1 || request!=1) return 1;
            struct ca_z1_native_header reply={CA_Z1_NATIVE_OVERLAY_MAGIC,0,0,0,request};
            if (write(3,&reply,sizeof(reply))!=sizeof(reply)) return 1;
            for (unsigned i=190;i<200;i++) {
                frame_header.stream=i&1U;
                if (write(3,&frame_header,sizeof(frame_header))!=sizeof(frame_header) ||
                    write(3,"\0\0\0\1\x65",5)!=5) return 1;
            }
            return 0;
        }
        struct ca_z1_native_header h = {CA_Z1_NATIVE_MAGIC, 5, 1234567, 1, 0};
        if (valid || !strcmp(mode,"ae-size")) {
            struct ca_exposure s=ca_exposure_empty(0,1234567);
            s.shutter_us=10000; s.analog_gain=2; s.valid=CA_AE_SHUTTER|CA_AE_AGAIN;
            struct ca_z1_native_header a={CA_Z1_NATIVE_AE_MAGIC,sizeof(s),1234567,0,0};
            if (!strcmp(mode,"ae-size")) a.size++;
            if (write(3,&a,sizeof(a))!=sizeof(a)) return 1;
            for (size_t i=0;i<sizeof(s);i++) if (write(3,(char *)&s+i,1)!=1) return 0;
        }
        if (!strcmp(mode, "stream")) h.stream = 2;
        if (!strcmp(mode, "oversized")) h.size = CA_Z1_NATIVE_MAX_FRAME + 1U;
        size_t size = !strcmp(mode, "truncated") ? 7 : sizeof(h);
        /* Exercise socket fragmentation rather than assuming full reads. */
        for (size_t i = 0; i < size; i++) {
            if (write(3, (char *)&h + i, 1) != 1) return 0;
        }
        if (valid) {
            if (write(3, "\0\0\0\1\x65", 5) != 5) return 1;
            h.stream = 1;
            if (write(3, &h, sizeof(h)) != sizeof(h) ||
                write(3, "\0\0\0\1\x65", 5) != 5) return 1;
            pause(); /* parent must terminate and reap us after callback stop */
        }
        return 0;
    }
    const char *modes[] = {"valid", "oversized", "truncated", "stream", "ae-size", "overlay", "retry"};
    for (unsigned i = 0; i < 7; i++) {
        setenv("Z1_NATIVE_TEST_MODE", modes[i], 1);
        atomic_store(&stopped, false);
        count = exposure_count = 0;
        stop_after = i==6 ? 195 : 2;
        struct ca_z1_overlay_control overlay={.desired=1,.applied=-EINPROGRESS};
        bool valid=i==0 || i==5;
        bool overlay_test=i==5 || i==6;
        int result = ca_z1_native_receive(argv[0], &stopped, frame, exposure, NULL, overlay_test ? &overlay : NULL);
        if (overlay_test) assert(atomic_load(&overlay.applied)==1);
        assert(result == (valid || i==6 ? 0 : -1));
        assert(exposure_count == (valid ? 1U : 0U));
        assert(count == (valid ? 2U : i==6 ? 195U : 0U));
        assert(waitpid(-1, NULL, WNOHANG) == -1); /* no leaked helper children */
    }
    puts("Z1 native IPC: two fragmented streams, malformed size/stream, truncated header and child cleanup passed");
    return 0;
}
