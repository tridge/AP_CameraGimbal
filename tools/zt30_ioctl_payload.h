/* Flat snapshots only. Not wired into the vendor observer until qualification.
 * The pointer captured before forwarding is reused after the call; never follow
 * nested fields or reload a descriptor supplied by another thread. */
#pragma once
#include "zt30_self_read.h"
#include "zt30_ioctl_registry.h"
#include <string.h>
#if !defined(__arm__) && !defined(ZT30_PAYLOAD_HOST_FIXTURE)
#error "ZT30 payload ABI is ARM32; host tests need an explicit fixture override"
#endif
#define ZT30_PAYLOAD_DESC 1U
#define ZT30_PAYLOAD_COMPLETE 2U
#define ZT30_PAYLOAD_POST 4U
#define ZT30_PAYLOAD_REFRESHED 16U
#define ZT30_PAYLOAD_RAW_ZERO 8U /* raw return only, never semantic output validity */
struct zt30_payload_record {
    uint64_t ns;
    uint32_t call_id,tid,command,device;
    int32_t raw_status,copy_error,bytes;
    uint32_t flags,length,pointer_low,pointer_high,descriptor[4],phase;
    unsigned char data[256];
    uint32_t capture_length,device_major,device_minor,reserved[3];
};
_Static_assert(sizeof(struct zt30_payload_record)==352,"fixed flat payload record");
struct zt30_payload_call {
    struct zt30_stack_guard guard;
    const struct zt30_ioctl_layout *layout;
    uintptr_t pointer;
    uint32_t descriptor[4],call_id;
    int descriptor_valid;
};
static inline int zt30_arm_payload_pointer(uint32_t low,uint32_t high,uintptr_t *pointer) {
    if(high!=(low&0x80000000U?UINT32_MAX:0) || low<4096 || low>=0xc0000000U || (low&3))return EINVAL;
    *pointer=(uintptr_t)low;return 0;
}
static inline void zt30_payload_record_init(struct zt30_payload_record *record,const struct zt30_payload_call *call,unsigned phase,int status) {
    memset(record,0,sizeof(*record));record->ns=zt30_guard_now();record->tid=(uint32_t)syscall(SYS_gettid);
    record->call_id=call->call_id;record->command=call->layout->command;record->device=call->layout->device;
    record->device_major=zt30_ioctl_payload_ids[call->layout->device].major;
    record->device_minor=zt30_ioctl_payload_ids[call->layout->device].minor;
    record->phase=phase;record->raw_status=status;record->length=call->layout->length;record->bytes=-1;
    if(phase=='A')record->flags|=ZT30_PAYLOAD_POST;
    if(phase=='A' && !status)record->flags|=ZT30_PAYLOAD_RAW_ZERO;
}
static inline void zt30_payload_copy(struct zt30_payload_record *record,const struct zt30_payload_call *call,const struct zt30_self_reader *reader) {
    memcpy(record->descriptor,call->descriptor,sizeof(record->descriptor));record->descriptor[1]=0;record->flags|=ZT30_PAYLOAD_DESC;
    record->pointer_low=call->descriptor[2];record->pointer_high=call->descriptor[3];
    record->capture_length=record->phase=='B'?call->layout->pre_length:call->layout->length;
    struct zt30_copy_result copy=zt30_self_copy_guarded(reader,&call->guard,record->data,call->pointer,record->capture_length);
    record->bytes=(int32_t)copy.bytes;record->copy_error=copy.error;
    if(copy.bytes==(ssize_t)record->capture_length && !copy.error)record->flags|=ZT30_PAYLOAD_COMPLETE;
    else memset(record->data,0,sizeof(record->data)); /* partial bytes are unavailable */
}
/* Unknown/scalar requests do not initialise a guard or attempt any copy. */
static inline int zt30_payload_unavailable(unsigned device,uint32_t command,unsigned major,unsigned minor,uint32_t id,int error,struct zt30_payload_call *call,struct zt30_payload_record *record) {
    int incoming=errno;memset(call,0,sizeof(*call));
    if(!zt30_ioctl_payload_device_matches(device,major,minor))return 0;
    call->layout=zt30_ioctl_layout_find(device,command);if(!call->layout)return 0;
    call->call_id=id;zt30_payload_record_init(record,call,'B',0);record->copy_error=error;
    errno=incoming;return 1;
}
static inline int zt30_payload_before(const struct zt30_self_reader *reader,unsigned device,uint32_t command,unsigned device_major,unsigned device_minor,uintptr_t argument,uint32_t call_id,struct zt30_payload_call *call,struct zt30_payload_record *record) {
    int incoming=errno;memset(call,0,sizeof(*call));
    if(!zt30_ioctl_payload_device_matches(device,device_major,device_minor))return 0;
    call->layout=zt30_ioctl_layout_find(device,command);if(!call->layout)return 0;
    call->call_id=call_id;zt30_payload_record_init(record,call,'B',0);
    int error=zt30_stack_guard_init(&call->guard,(uintptr_t)call);
    if(error)goto unavailable;
    struct zt30_copy_result copy=zt30_self_copy_guarded(reader,&call->guard,call->descriptor,argument,16);
    if(copy.bytes!=16 || copy.error) {error=copy.error?copy.error:EIO;goto unavailable;}
    if(call->descriptor[0]!=call->layout->length || call->layout->length>ZT30_SELF_COPY_MAX) {error=EINVAL;goto unavailable;}
#if defined(ZT30_PAYLOAD_HOST_FIXTURE) && !defined(__arm__)
    call->pointer=(uintptr_t)call->descriptor[2]|((uintptr_t)call->descriptor[3]<<32);
#else
    error=zt30_arm_payload_pointer(call->descriptor[2],call->descriptor[3],&call->pointer);if(error)goto unavailable;
#endif
    error=zt30_stack_guard_check(&call->guard,call->pointer,call->layout->length);if(error)goto unavailable;
    call->descriptor_valid=1;zt30_payload_copy(record,call,reader);errno=incoming;return 1;
unavailable:
    memset(call->descriptor,0,sizeof(call->descriptor));record->copy_error=error;errno=incoming;return 1;
}
static inline int zt30_payload_after(const struct zt30_self_reader *reader,struct zt30_payload_call *call,int raw_status,struct zt30_payload_record *record) {
    int incoming=errno,emit=call->layout && call->layout->post;
    if(emit) {
        zt30_payload_record_init(record,call,'A',raw_status);
        if(call->descriptor_valid) {
            /* Revalidate the original stack buffer after a slow call. This
             * reloads mapping metadata, never the caller's descriptor. */
            int error=0;
            if(zt30_guard_now()>=call->guard.expires_ns) {
                record->flags|=ZT30_PAYLOAD_REFRESHED;
                error=zt30_stack_guard_init(&call->guard,(uintptr_t)call);
            }
            if(error)record->copy_error=error;
            else zt30_payload_copy(record,call,reader);
        }
        else record->copy_error=EINVAL;
    }
    zt30_stack_guard_clear(&call->guard);call->descriptor_valid=0;errno=incoming;return emit;
}
