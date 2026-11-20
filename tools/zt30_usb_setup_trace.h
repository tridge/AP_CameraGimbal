/* Claimed-handle UVC startup metadata; fixed scan and time limits. */
#pragma once
#include "zt30_usb_control_snapshot.h"
#define ZT30_USB_SUBMIT 0x802c550aU
#define ZT30_USB_ALT 0x80085504U
#define ZT30_USB_CLAIM 0x8004550fU
#define ZT30_SETUP_BUDGET_NS 100000000U
struct zt30_usb_header_record {
    uint64_t before_ns,after_ns,inspect_ns,argument,stack_low,stack_high;
    uint32_t call_id,tid,command;int32_t fd,result,error;
    uint32_t phase,copy_error,headroom;
    struct zt30_usb_urb_header header;
    uint32_t generation;
    struct zt30_usb_control_copy control;
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct zt30_usb_header_record)==192,"fixed UVC record ABI");
static struct zt30_trace_output usb_header_output={.fd=-1};
static struct zt30_self_reader usb_header_reader={.fd=-1};
static uint32_t usb_setup_generations[1024],usb_setup_claims[1024];
static uint32_t usb_header_alt,usb_header_slots[2],usb_header_written,usb_header_inflight,usb_header_failures;
static uint32_t usb_setup_used,usb_setup_bad_time,usb_setup_alt_fd=UINT32_MAX,usb_setup_alt_gen;
static volatile sig_atomic_t usb_header_busy;
static uint32_t usb_setup_generation(int fd) {
    return fd>=0 && fd<1024?__atomic_load_n(&usb_setup_generations[fd],__ATOMIC_ACQUIRE):0;
}
static void usb_setup_opened(int fd,uint32_t generation) {
    if(fd<0 || fd>=1024)return;
    __atomic_store_n(&usb_setup_claims[fd],0,__ATOMIC_RELEASE);
    __atomic_store_n(&usb_setup_generations[fd],generation,__ATOMIC_RELEASE);
}
static ssize_t usb_header_write(const void *data,size_t size,uint32_t offset) {
    if(!zt30_trace_output_matches(&usb_header_output))return -1;
#if defined(__arm__) && __SIZEOF_POINTER__==4
    return syscall(SYS_pwrite64,usb_header_output.fd,data,size,0,offset,0);
#else
    return syscall(SYS_pwrite64,usb_header_output.fd,data,size,(off_t)offset);
#endif
}
static void usb_header_begin(const char *directory) {
    char path[160];if(snprintf(path,sizeof(path),"%s/usb-setup.bin",directory)>=(int)sizeof(path))return;
    if(zt30_trace_output_open(&usb_header_output,path)<0)return;
    if(usb_header_write("ZT30UVS1",8,0)!=8 || zt30_usb_reader_init(&usb_header_reader)) {
        usb_header_write("FAILUVS1",8,0);__sync_fetch_and_add(&usb_header_failures,1);
    }
}
static int usb_setup_slot(unsigned phase,unsigned limit) {
    uint32_t old=__atomic_load_n(&usb_header_slots[phase],__ATOMIC_RELAXED);
    for(unsigned i=0;i<16;i++) {
        if(old>=limit)return 0;
        if(__sync_bool_compare_and_swap(&usb_header_slots[phase],old,old+1))return 1;
        old=__atomic_load_n(&usb_header_slots[phase],__ATOMIC_RELAXED);
    }
    return 0;
}
static int usb_header_select(int device,uint32_t command,struct zt30_usb_header_record *r) {
    int fd=r->fd;
    if(device<0 || command!=ZT30_USB_SUBMIT || usb_header_output.fd<0 || fd<0 || fd>=1024 ||
       !r->generation || __atomic_load_n(&usb_setup_claims[fd],__ATOMIC_ACQUIRE)!=r->generation ||
       __atomic_load_n(&usb_header_alt,__ATOMIC_ACQUIRE) ||
       __atomic_load_n(&usb_setup_used,__ATOMIC_ACQUIRE)>=ZT30_SETUP_BUDGET_NS ||
       __atomic_load_n(&usb_header_slots[0],__ATOMIC_RELAXED)>=64)return 0;
    if(!__sync_bool_compare_and_swap(&usb_header_busy,0,1)) {
        if(!usb_setup_slot(1,16))return 0;
        r->phase=1;r->copy_error=EDEADLK;
    } else {
        if(!usb_setup_slot(0,64)) {__sync_lock_release(&usb_header_busy);return 0;}
        r->phase=0;
    }
    __sync_fetch_and_add(&usb_header_inflight,1);return 1;
}
__attribute__((noinline)) static int usb_header_stack(struct zt30_usb_header_record *r) {
    uintptr_t anchor=(uintptr_t)&r;pthread_attr_t attr;void *base=NULL;size_t size=0,guard=0;
    if((pid_t)syscall(SYS_gettid)==getpid())return EOPNOTSUPP;
    int error=pthread_getattr_np(pthread_self(),&attr);if(error)return error;
    error=pthread_attr_getstack(&attr,&base,&size);
    if(!error)error=pthread_attr_getguardsize(&attr,&guard);
    pthread_attr_destroy(&attr);if(error)return error;
    uintptr_t low=(uintptr_t)base;
    if(!zt30_user_range(low,size) || guard>=size || anchor<low+guard || anchor>=low+size)return EPERM;
    r->stack_low=low+guard;r->stack_high=low+size;uintptr_t room=anchor-(low+guard);
    r->headroom=room>UINT32_MAX?UINT32_MAX:(uint32_t)room;return room<32768?E2BIG:0;
}
__attribute__((noinline)) static int usb_header_heavy(uintptr_t argument,struct zt30_usb_header_record *r,uint64_t deadline) {
    struct zt30_usb_memory_guard guard;
    if(!zt30_usb_reader_matches(&usb_header_reader))return EPERM;
    int error=zt30_usb_guard_init(&guard,argument,44,deadline);if(error)return error;
    error=zt30_usb_guard_check(&guard,argument,44);if(error)return error;
    if(__atomic_load_n(&usb_header_alt,__ATOMIC_ACQUIRE))return ECANCELED;
    struct zt30_copy_result copy=zt30_self_copy(&usb_header_reader,&r->header,argument,44);
    if(copy.error || copy.bytes!=44)return copy.error?copy.error:EIO;
    if(zt30_guard_now()>=deadline)return ETIMEDOUT;
    if(__atomic_load_n(&usb_header_alt,__ATOMIC_ACQUIRE))return ECANCELED;
    zt30_usb_control_snapshot(&usb_header_reader,&r->header,deadline,&r->control);
    return 0;
}
static void usb_header_before(int fd,uintptr_t argument,struct zt30_usb_header_record *r) {
    int incoming=errno;r->fd=fd;r->command=ZT30_USB_SUBMIT;r->argument=argument;
    r->tid=(uint32_t)syscall(SYS_gettid);r->before_ns=zt30_guard_now();
    if(r->phase==0) {
        uint32_t used=__atomic_load_n(&usb_setup_used,__ATOMIC_ACQUIRE);
        uint64_t remaining=used<ZT30_SETUP_BUDGET_NS?ZT30_SETUP_BUDGET_NS-used:0;
        uint64_t allowance=remaining<ZT30_USB_COPY_DEADLINE_NS?remaining:ZT30_USB_COPY_DEADLINE_NS;
        r->copy_error=(uint32_t)usb_header_stack(r);
        if(!r->copy_error)r->copy_error=(uint32_t)usb_header_heavy(argument,r,r->before_ns+allowance);
        if(usb_setup_generation(fd)!=r->generation)r->copy_error=ESTALE;
        r->inspect_ns=zt30_guard_now()-r->before_ns;
        if(r->inspect_ns>allowance || r->control.error==ETIMEDOUT) {
            r->copy_error=ETIMEDOUT;__atomic_store_n(&usb_setup_bad_time,1,__ATOMIC_RELEASE);
        }
        uint64_t total=(uint64_t)used+r->inspect_ns;
        __atomic_store_n(&usb_setup_used,total>UINT32_MAX?UINT32_MAX:(uint32_t)total,__ATOMIC_RELEASE);
        __sync_lock_release(&usb_header_busy);
    } else r->inspect_ns=0; /* Busy means no inspection; record its call window. */
    if(r->copy_error) {memset(&r->header,0,sizeof(r->header));memset(&r->control,0,sizeof(r->control));}
    errno=incoming;
}
static void usb_header_after(int device,uint32_t command,int selected,struct zt30_usb_header_record *r,int result,int error) {
    if(device>=0 && result==0) {
        int fd=r->fd;uint32_t generation=usb_setup_generation(fd);
        if(command==ZT30_USB_CLAIM && generation)__atomic_store_n(&usb_setup_claims[fd],generation,__ATOMIC_RELEASE);
        if(command==ZT30_USB_ALT && __sync_bool_compare_and_swap(&usb_header_alt,0,2)) {
            usb_setup_alt_fd=(uint32_t)fd;usb_setup_alt_gen=generation;
            __atomic_store_n(&usb_header_alt,1,__ATOMIC_RELEASE);
        }
    }
    if(!selected)return;
    r->after_ns=zt30_guard_now();r->result=result;r->error=result==-1?error:0;
    unsigned n=__sync_fetch_and_add(&usb_header_written,1);
    if(n>=80 || usb_header_write(r,sizeof(*r),8+n*sizeof(*r))!=(ssize_t)sizeof(*r)) {
        __sync_fetch_and_add(&usb_header_failures,1);usb_header_write("FAILUVS1",8,0);
    }
    __sync_fetch_and_sub(&usb_header_inflight,1);
}
static void usb_header_finish(void) {
    uint32_t footer[16]={0};memcpy(footer,"ENDUVS1!",8);
    footer[2]=__atomic_load_n(&usb_header_slots[0],__ATOMIC_ACQUIRE);
    footer[3]=__atomic_load_n(&usb_header_slots[1],__ATOMIC_ACQUIRE);
    footer[4]=__atomic_load_n(&usb_header_written,__ATOMIC_ACQUIRE);
    footer[5]=__atomic_load_n(&usb_header_inflight,__ATOMIC_ACQUIRE);
    footer[6]=__atomic_load_n(&usb_header_failures,__ATOMIC_ACQUIRE);
    footer[7]=__atomic_load_n(&usb_header_alt,__ATOMIC_ACQUIRE);
    footer[8]=usb_setup_alt_fd;footer[9]=usb_setup_alt_gen;
    footer[10]=__atomic_load_n(&usb_setup_used,__ATOMIC_ACQUIRE);
    footer[11]=__atomic_load_n(&usb_setup_bad_time,__ATOMIC_ACQUIRE);
    if(footer[4]>80 || usb_header_write(footer,sizeof(footer),8+footer[4]*192)!=(ssize_t)sizeof(footer))return;
    zt30_usb_reader_close(&usb_header_reader);
    if(zt30_trace_output_matches(&usb_header_output))syscall(SYS_close,usb_header_output.fd);
    usb_header_output.fd=-1;
}
