/* Control metadata only: setup8 + allowlisted outgoing UVC probe/commit.
 * Input header is an already-copied local value. No ISO/bulk/IN-data reads. */
#pragma once
#include "zt30_usb_urb_snapshot.h"
struct zt30_usb_control_copy {
    uint8_t setup[8],data[34],setup_valid,data_length;
    int error;
};
static __thread volatile sig_atomic_t zt30_usb_control_busy;
static inline uint16_t zt30_usb_le16(const uint8_t *p) {return (uint16_t)p[0]|(uint16_t)p[1]<<8;}
/* Validate a word-aligned superset but copy no alignment-prefix bytes. */
static inline int zt30_usb_byte_guard(struct zt30_usb_memory_guard *g,uintptr_t source,size_t size,uint64_t deadline) {
    if(!zt30_user_range(source,size) || size>ZT30_SELF_COPY_MAX)return EINVAL;
    size_t prefix=source&3U;if(size>ZT30_SELF_COPY_MAX-prefix)return E2BIG;
    return zt30_usb_guard_init(g,source-prefix,size+prefix,deadline);
}
static inline struct zt30_copy_result zt30_usb_copy_bytes(const struct zt30_self_reader *reader,
    const struct zt30_usb_memory_guard *g,void *out,uintptr_t source,size_t size) {
    int error=0;
    if(!zt30_user_range(source,size) || size>ZT30_SELF_COPY_MAX)error=EINVAL;
    else if(!zt30_usb_reader_matches(reader) || g->owner!=getpid() || g->tid!=(pid_t)syscall(SYS_gettid))error=EPERM;
    else if(zt30_guard_now()>=g->deadline)error=ETIMEDOUT;
    else if(source<g->low || source>=g->high || size>g->high-source)error=EPERM;
    if(error)return (struct zt30_copy_result){-1,error};
    struct zt30_copy_result result=zt30_self_copy(reader,out,source,size);
    if(zt30_guard_now()>=g->deadline)result=(struct zt30_copy_result){-1,ETIMEDOUT};
    return result;
}
static inline void zt30_usb_control_snapshot(const struct zt30_self_reader *reader,
    const struct zt30_usb_urb_header *local,uint64_t deadline,struct zt30_usb_control_copy *out) {
    int incoming=errno;memset(out,0,sizeof(*out));
    if(!__sync_bool_compare_and_swap(&zt30_usb_control_busy,0,1)) {out->error=EDEADLK;goto done;}
    if(!local || local->type!=2 || local->endpoint || local->packets ||
       (local->buffer_length!=34 && local->buffer_length!=42))goto release;
    uintptr_t source=local->buffer;struct zt30_usb_memory_guard guard;
    out->error=zt30_usb_byte_guard(&guard,source,(size_t)local->buffer_length,deadline);
    if(out->error)goto release;
    struct zt30_copy_result result=zt30_usb_copy_bytes(reader,&guard,out->setup,source,8);
    if(result.error || result.bytes!=8) {out->error=result.error?result.error:EIO;goto clear;}
    uint16_t value=zt30_usb_le16(out->setup+2),index=zt30_usb_le16(out->setup+4),length=zt30_usb_le16(out->setup+6);
    out->setup_valid=1;
    /* IN requests retain setup metadata only; never capture unfilled data. */
    if(out->setup[0]!=0x21 || out->setup[1]!=1 || (value!=0x0100 && value!=0x0200) ||
       index!=1 || (length!=26 && length!=34) || local->buffer_length!=8+length)goto release;
    result=zt30_usb_copy_bytes(reader,&guard,out->data,source+8,length);
    if(result.error || result.bytes!=length) {out->error=result.error?result.error:EIO;goto clear;}
    out->data_length=(uint8_t)length;goto release;
clear:memset(out->setup,0,sizeof(out->setup));memset(out->data,0,sizeof(out->data));out->setup_valid=out->data_length=0;
release:__sync_lock_release(&zt30_usb_control_busy);
done:errno=incoming;
}
