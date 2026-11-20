/* Private startup diagnostic. Exactly one submit per pre/post ALT phase. */
#pragma once
#include "zt30_usb_urb_snapshot.h"
#define ZT30_USB_SUBMIT 0x802c550aU
#define ZT30_USB_ALT 0x80085504U
struct zt30_usb_header_record {
    uint64_t before_ns,after_ns,inspect_ns,argument,stack_low,stack_high;
    uint32_t call_id,tid,command;
    int32_t fd,result,error;
    uint32_t phase,copy_error,headroom;
    struct zt30_usb_urb_header header;
};
_Static_assert(sizeof(struct zt30_usb_header_record)==128,"fixed header metadata ABI");
static struct zt30_trace_output usb_header_output={.fd=-1};
static struct zt30_self_reader usb_header_reader={.fd=-1};
static uint32_t usb_header_alt,usb_header_slots[2],usb_header_written,usb_header_inflight,usb_header_failures;
static volatile sig_atomic_t usb_header_busy;
static ssize_t usb_header_write(const void *data,size_t size,uint32_t offset) {
    if(!zt30_trace_output_matches(&usb_header_output))return -1;
#if defined(__arm__) && __SIZEOF_POINTER__==4
    return syscall(SYS_pwrite64,usb_header_output.fd,data,size,0,offset,0);
#else
    return syscall(SYS_pwrite64,usb_header_output.fd,data,size,(off_t)offset);
#endif
}
static void usb_header_begin(const char *directory) {
    char path[160];
    if(snprintf(path,sizeof(path),"%s/usb-headers.bin",directory)>=(int)sizeof(path))return;
    if(zt30_trace_output_open(&usb_header_output,path)<0)return;
    if(usb_header_write("ZT30UHD1",8,0)!=8 || zt30_usb_reader_init(&usb_header_reader)) {
        usb_header_write("FAILUHD1",8,0);__sync_fetch_and_add(&usb_header_failures,1);
    }
}
static int usb_header_select(int device,uint32_t command,struct zt30_usb_header_record *r) {
    if(device<0 || command!=ZT30_USB_SUBMIT || usb_header_output.fd<0)return 0;
    unsigned phase=__atomic_load_n(&usb_header_alt,__ATOMIC_ACQUIRE)?1:0;
    if(!__sync_bool_compare_and_swap(&usb_header_slots[phase],0,1))return 0;
    __sync_fetch_and_add(&usb_header_inflight,1);r->phase=phase;return 1;
}
/* This small frame returns before the heavy parser worker is called. Initial
 * thread rejected before pthread_getattr_np's maps/stdio/malloc path. */
__attribute__((noinline)) static int usb_header_stack(struct zt30_usb_header_record *r) {
    uintptr_t anchor=(uintptr_t)&r;pthread_attr_t attr;void *base=NULL;size_t size=0,guard=0;
    if((pid_t)syscall(SYS_gettid)==getpid())return EOPNOTSUPP;
    int error=pthread_getattr_np(pthread_self(),&attr);if(error)return error;
    error=pthread_attr_getstack(&attr,&base,&size);
    if(!error)error=pthread_attr_getguardsize(&attr,&guard);
    pthread_attr_destroy(&attr);if(error)return error;
    uintptr_t low=(uintptr_t)base;
    if(!zt30_user_range(low,size) || guard>=size || anchor<low+guard || anchor>=low+size)return EPERM;
    r->stack_low=low+guard;r->stack_high=low+size;
    uintptr_t room=anchor-(low+guard);r->headroom=room>UINT32_MAX?UINT32_MAX:(uint32_t)room;
    return room<32768?E2BIG:0;
}
__attribute__((noinline)) static struct zt30_copy_result usb_header_heavy(uintptr_t argument,struct zt30_usb_urb_header *out) {
    return zt30_usb_urb_snapshot(&usb_header_reader,argument,out);
}
static void usb_header_before(int fd,uintptr_t argument,struct zt30_usb_header_record *r) {
    int incoming=errno;r->fd=fd;r->command=ZT30_USB_SUBMIT;r->argument=argument;
    r->tid=(uint32_t)syscall(SYS_gettid);r->before_ns=zt30_guard_now();
    if(!__sync_bool_compare_and_swap(&usb_header_busy,0,1))r->copy_error=EDEADLK;
    else {
        r->copy_error=(uint32_t)usb_header_stack(r);
        if(!r->copy_error) {
            struct zt30_copy_result copy=usb_header_heavy(argument,&r->header);
            if(copy.bytes!=44 || copy.error)r->copy_error=copy.error?copy.error:EIO;
        }
        __sync_lock_release(&usb_header_busy);
    }
    r->inspect_ns=zt30_guard_now()-r->before_ns;
    if(r->inspect_ns>ZT30_USB_COPY_DEADLINE_NS)r->copy_error=ETIMEDOUT;
    if(r->copy_error)memset(&r->header,0,sizeof(r->header));
    errno=incoming;
}
static void usb_header_after(int device,uint32_t command,int selected,struct zt30_usb_header_record *r,int result,int error) {
    if(device>=0 && command==ZT30_USB_ALT && result==0)__atomic_store_n(&usb_header_alt,1,__ATOMIC_RELEASE);
    if(!selected)return;
    r->after_ns=zt30_guard_now();r->result=result;r->error=result==-1?error:0;
    unsigned n=__sync_fetch_and_add(&usb_header_written,1);
    if(n>=2 || usb_header_write(r,sizeof(*r),8+n*sizeof(*r))!=(ssize_t)sizeof(*r)) {
        __sync_fetch_and_add(&usb_header_failures,1);usb_header_write("FAILUHD1",8,0);
    }
    __sync_fetch_and_sub(&usb_header_inflight,1);
}
static void usb_header_finish(void) {
    uint32_t footer[8]={0};memcpy(footer,"ENDUHD1!",8);
    footer[2]=__atomic_load_n(&usb_header_slots[0],__ATOMIC_ACQUIRE);
    footer[3]=__atomic_load_n(&usb_header_slots[1],__ATOMIC_ACQUIRE);
    footer[4]=__atomic_load_n(&usb_header_written,__ATOMIC_ACQUIRE);
    footer[5]=__atomic_load_n(&usb_header_inflight,__ATOMIC_ACQUIRE);
    footer[6]=__atomic_load_n(&usb_header_failures,__ATOMIC_ACQUIRE);
    if(footer[4]>2 || usb_header_write(footer,sizeof(footer),8+footer[4]*128)!=(ssize_t)sizeof(footer))return;
    zt30_usb_reader_close(&usb_header_reader);
    if(zt30_trace_output_matches(&usb_header_output))syscall(SYS_close,usb_header_output.fd);
    usb_header_output.fd=-1;
}
