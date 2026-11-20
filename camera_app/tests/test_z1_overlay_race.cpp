#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../src/backends/z1mini/native.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static struct ca_z1_overlay_control control;
static atomic_bool stop;
static pthread_barrier_t barrier;
static bool inject;
static unsigned frames, errors;
static void before_store(const void *object)
{
    if (object == &control.applied && inject) {
        inject=false;
        pthread_barrier_wait(&barrier);
        pthread_barrier_wait(&barrier);
    }
}
/* Force the real receiver's acknowledgement store to race a control update. */
#undef atomic_store
#define atomic_store(object, value) do { before_store(object); atomic_store_explicit(object,value,memory_order_seq_cst); } while (0)
#include "../src/backends/z1mini/native.cpp"

void ca_log(const char *format, ...)
{
    if (strstr(format,"overlay request failed")) errors++;
}
static void *toggle(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&barrier);
    atomic_store_explicit(&control.desired,0,memory_order_seq_cst);
    atomic_store_explicit(&control.applied,-EINPROGRESS,memory_order_seq_cst);
    pthread_barrier_wait(&barrier);
    return NULL;
}
static void publish(void *unused, const uint8_t *data, size_t size, uint64_t pts, bool key, unsigned stream)
{
    (void)unused; (void)data; (void)size; (void)pts; (void)key; (void)stream;
    if (++frames==2) atomic_store(&stop,true);
}
static void write_all(const void *data, size_t size)
{
    const char *p=(const char *)data;
    while (size) {
        ssize_t n=write(3,p,size);
        if (n<0 && errno==EINTR) continue;
        assert(n>0); p+=n; size-=(size_t)n;
    }
}
static void read_all(void *data, size_t size)
{
    char *p=(char *)data;
    while (size) {
        ssize_t n=read(3,p,size);
        if (n<0 && errno==EINTR) continue;
        assert(n>0); p+=n; size-=(size_t)n;
    }
}
int main(int argc, char **argv)
{
    if (argc>1) {
        struct ca_z1_native_header ready={CA_Z1_NATIVE_OVERLAY_MAGIC,0,CA_Z1_NATIVE_OVERLAY_READY,0,0};
        write_all(&ready,sizeof(ready));
        for (unsigned i=0;i<2;i++) {
            struct ca_z1_overlay_request request;
            read_all(&request,sizeof(request));
            assert(request.desired==(i ? 0U : 1U));
            struct ca_z1_native_header ack={CA_Z1_NATIVE_OVERLAY_MAGIC,0,request.sequence,(uint32_t)(i ? EIO : 0),request.desired};
            write_all(&ack,sizeof(ack));
            struct ca_z1_native_header frame={CA_Z1_NATIVE_MAGIC,5,1,1,0};
            write_all(&frame,sizeof(frame)); write_all("\0\0\0\1\x65",5);
        }
        pause();
        return 0;
    }
    atomic_init(&control.desired,1); atomic_init(&control.applied,-EINPROGRESS);
    pthread_barrier_init(&barrier,NULL,2);
    pthread_t thread;
    assert(pthread_create(&thread,NULL,toggle,NULL)==0);
    inject=true;
    assert(ca_z1_native_receive(argv[0],&stop,publish,NULL,NULL,&control,false)==0);
    pthread_join(thread,NULL); pthread_barrier_destroy(&barrier);
    assert(frames==2 && atomic_load(&control.desired)==0);
    assert(atomic_load(&control.applied)==-EIO && errors==1);
    assert(waitpid(-1,NULL,WNOHANG)==-1);
    puts("PASS overlay acknowledgement/control race and helper error diagnostic");
}
