/* Flat ARM32 URB header only. Raw pointers are metadata, never dereferenced. */
#pragma once
#include "zt30_usb_memory_guard.h"
#include <stddef.h>
struct zt30_usb_urb_header {
    uint8_t type,endpoint;uint16_t padding;
    int32_t status;uint32_t flags,buffer;
    int32_t buffer_length,actual_length,start_frame,packets,error_count;
    uint32_t signal,usercontext;
};
_Static_assert(sizeof(struct zt30_usb_urb_header)==44,"fixed ARM32 URB layout on every host");
_Static_assert(offsetof(struct zt30_usb_urb_header,buffer)==12,"buffer offset");
_Static_assert(offsetof(struct zt30_usb_urb_header,packets)==28,"packet count offset");
_Static_assert(offsetof(struct zt30_usb_urb_header,usercontext)==40,"usercontext offset");
static __thread volatile sig_atomic_t zt30_usb_snapshot_busy;
static inline struct zt30_copy_result zt30_usb_urb_snapshot(const struct zt30_self_reader *r,
                                                          uintptr_t argument,struct zt30_usb_urb_header *out) {
    int incoming=errno;struct zt30_copy_result result={-1,EINVAL};
    if(!out)goto done;
    memset(out,0,sizeof(*out));
    if(!__sync_bool_compare_and_swap(&zt30_usb_snapshot_busy,0,1)) {result.error=EDEADLK;goto done;}
    if(!r || !zt30_usb_reader_matches(r)) {result.error=EPERM;goto release;}
    uint64_t now=zt30_guard_now();
    if(!now) {result.error=EIO;goto release;}
    struct zt30_usb_memory_guard g;
    result.error=zt30_usb_guard_init(&g,argument,sizeof(*out),now+ZT30_USB_COPY_DEADLINE_NS);
    if(result.error)goto release;
    result.error=zt30_usb_guard_check(&g,argument,sizeof(*out));if(result.error)goto release;
    result=zt30_self_copy(r,out,argument,sizeof(*out));
    if(result.bytes!=(ssize_t)sizeof(*out) || result.error) {
        if(!result.error)result.error=EIO;
        result.bytes=-1;memset(out,0,sizeof(*out));
    } else if(zt30_guard_now()>=g.deadline) {result=(struct zt30_copy_result){-1,ETIMEDOUT};memset(out,0,sizeof(*out));}
release:__sync_lock_release(&zt30_usb_snapshot_busy);
done:errno=incoming;return result;
}
