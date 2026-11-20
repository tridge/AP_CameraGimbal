/* Level-0 vendor ioctl observation: identities/scalars only, no argument reads.
 * No MI API, additional device ioctl, or caller path/payload dereference. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#ifdef ZT30_IOCTL_PAYLOAD_TRACE
#include "zt30_ioctl_payload.h"
static void payload_begin(const char *directory);
static void payload_store(const struct zt30_payload_record *record);
static void payload_finish(void);
static int payload_fd=-1;
static struct zt30_self_reader payload_reader={.fd=-1};
static __thread volatile sig_atomic_t payload_busy;
#endif
#ifndef ZT30_IOCTL_VENDOR_EXE
#define ZT30_IOCTL_VENDOR_EXE "/customer/siyi_app_359"
#endif
#ifndef ZT30_IOCTL_SENSOR_DEVICE
#define ZT30_IOCTL_SENSOR_DEVICE "/dev/mi_sensor"
#endif
#ifndef ZT30_IOCTL_FIRST_DEVICE
#define ZT30_IOCTL_FIRST_DEVICE "/dev/mi_sys"
#endif
#ifdef ZT30_USB_TRACE
#ifdef ZT30_IOCTL_PAYLOAD_TRACE
#error USB metadata must not enable payload reads
#endif
#ifdef ZT30_USB_HEADER_TRACE
#ifdef ZT30_USB_SETUP_TRACE
#define TRACE_ENV "APCG_USB_SETUP_TRACE"
#define TRACE_FILE "/usb_setup.bin"
#define COUNTS_FILE "usb_setup-counts.bin"
#define TRACE_MAGIC "ZT30UV00"
#define COUNTS_MAGIC "ZT30UVC2"
#define COUNTS_FAILURE_MAGIC "FAILUVC2"
#define FAILURE_MAGIC "FAILUV00"
#else
#define TRACE_ENV "APCG_USB_HEADER_TRACE"
#define TRACE_FILE "/usb_header.bin"
#define COUNTS_FILE "usb_header-counts.bin"
#define TRACE_MAGIC "ZT30UH00"
#define COUNTS_MAGIC "ZT30UHC1"
#define COUNTS_FAILURE_MAGIC "FAILUHC1"
#define FAILURE_MAGIC "FAILUH00"
#endif
#else
#define TRACE_ENV "APCG_USB_TRACE"
#define TRACE_FILE "/usb.bin"
#define COUNTS_FILE "usb-counts.bin"
#define TRACE_MAGIC "ZT30USB0"
#define COUNTS_MAGIC "ZT30USC1"
#define COUNTS_FAILURE_MAGIC "FAILUSC1"
#define FAILURE_MAGIC "FAILUSB0"
#endif
#else
#define TRACE_ENV "APCG_IOCTL_TRACE"
#define TRACE_FILE "/ioctl.bin"
#define COUNTS_FILE "ioctl-counts.bin"
#define TRACE_MAGIC "ZT30IOT0"
#define COUNTS_MAGIC "ZT30IOC1"
#define COUNTS_FAILURE_MAGIC "FAILIOC1"
#define FAILURE_MAGIC "FAILIOT1"
#endif
// All observers must survive caller closure/reuse of their diagnostic FDs.
#define ZT30_PIN_TRACE_OUTPUT
#define LIMIT (2U*1024U*1024U)
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif
#ifdef ZT30_PIN_TRACE_OUTPUT
#include "zt30_trace_output.h"
static struct zt30_trace_output log_output={.fd=-1},counts_output={.fd=-1};
#endif
struct record {
    uint64_t ns;
    uint32_t call_id,tid,command;
    int32_t fd,result,error;
    uint32_t device_major,device_minor,flags;
    uint8_t device,type;
    uint16_t reserved;
    uint32_t data[4];
};
_Static_assert(sizeof(struct record)==64,"fixed ARM/host record");
#ifdef ZT30_USB_TRACE
#ifndef ZT30_USB_DEVICE
#define ZT30_USB_DEVICE "/dev/bus/usb/002/002"
#endif
static const char *devices[]={ZT30_USB_DEVICE};
#else
static const char *devices[]={ZT30_IOCTL_FIRST_DEVICE,ZT30_IOCTL_SENSOR_DEVICE,"/dev/mi_vif","/dev/mi_isp","/dev/mi_scl","/dev/mi_venc","/dev/mi_poll","/dev/mi_vdisp","/dev/isp_fe"};
#endif
#define DEVICE_COUNT (sizeof(devices)/sizeof(devices[0]))
static dev_t device_ids[DEVICE_COUNT];static unsigned char have_device[DEVICE_COUNT];
static int log_fd=-1,counts_fd=-1;static pid_t owner;
/* Full request values share no budget. Bounded probing reports saturation. */
#define SLOTS 1024U
static uint32_t cursor=8,call_counter,keys[DEVICE_COUNT][SLOTS],counts[DEVICE_COUNT][SLOTS],cap_reported;
static uint32_t closing,untracked,short_writes,zero_counts[DEVICE_COUNT];
struct count_record { uint32_t device,command,total,budget_granted,budget_skipped,reserved[3]; };
_Static_assert(sizeof(struct count_record)==32,"fixed counts record");
#ifdef ZT30_USB_HEADER_TRACE
#ifdef ZT30_USB_SETUP_TRACE
#include "zt30_usb_setup_trace.h"
#else
#include "zt30_usb_header_trace.h"
#endif
#endif
static int (*next_ioctl)(int,unsigned long,...);
static int (*next_open)(const char*,int,...),(*next_open64)(const char*,int,...);
static int (*next_openat)(int,const char*,int,...);
static ssize_t offset_write(const void *data,size_t length,uint32_t offset) {
#ifdef ZT30_PIN_TRACE_OUTPUT
    if(!zt30_trace_output_matches(&log_output))return -1;
#endif
#if defined(__arm__) && __SIZEOF_POINTER__==4
    return syscall(SYS_pwrite64,log_fd,data,length,0,offset,0);
#else
    return syscall(SYS_pwrite64,log_fd,data,length,(off_t)offset);
#endif
}
static void store_record(const struct record *record,uint32_t offset) {
    ssize_t n;
    do { n=offset_write(record,sizeof(*record),offset); } while(n<0 && errno==EINTR);
    if(n!=(ssize_t)sizeof(*record)) { __sync_fetch_and_add(&short_writes,1);offset_write(FAILURE_MAGIC,8,0); }
}
static uint64_t clock_ns(void) { struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec; }
static void emit(uint8_t type,unsigned device,int fd,uint32_t command,int result,int error,uint32_t id,uint32_t flags) {
    struct record record={0};record.ns=clock_ns();record.call_id=id;record.tid=(uint32_t)syscall(SYS_gettid);
    record.command=command;record.fd=fd;record.result=result;record.error=error;record.device=(uint8_t)device;record.type=type;record.flags=flags;
    record.device_major=major(device_ids[device]);record.device_minor=minor(device_ids[device]);
    uint32_t position;
    for(unsigned attempt=0;;attempt++) {
        if(attempt==32) { __sync_fetch_and_add(&short_writes,1);offset_write(FAILURE_MAGIC,8,0);return; }
        position=__atomic_load_n(&cursor,__ATOMIC_RELAXED);
        if(position>LIMIT-2*sizeof(record)) {
            if(__sync_bool_compare_and_swap(&cap_reported,0,1)) { record.type='L';store_record(&record,position); }
            return;
        }
        if(__sync_bool_compare_and_swap(&cursor,position,position+sizeof(record)))break;
    }
    store_record(&record,position);
}
static int identity(int fd) {
    struct stat st;
    if(log_fd<0 || __atomic_load_n(&closing,__ATOMIC_ACQUIRE) || getpid()!=owner || fstat(fd,&st) || !S_ISCHR(st.st_mode))return -1;
    for(unsigned i=0;i<DEVICE_COUNT;i++)if(have_device[i] && st.st_rdev==device_ids[i])return (int)i;
    return -1;
}
static void resolve(void) {
    if(!next_ioctl)next_ioctl=dlsym(RTLD_NEXT,"ioctl");
    if(!next_open)next_open=dlsym(RTLD_NEXT,"open");
    if(!next_open64)next_open64=dlsym(RTLD_NEXT,"open64");
    if(!next_openat)next_openat=dlsym(RTLD_NEXT,"openat");
}
__attribute__((constructor)) static void begin(void) {
    int saved=errno;char exe[256];ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    resolve();if(n<0)goto done;exe[n]=0;
    if(strcmp(exe,ZT30_IOCTL_VENDOR_EXE))goto done;
    const char *path=getenv(TRACE_ENV);const char *prefix="/tmp/apcg-zt30.session.";
    if(!path || strlen(path)!=strlen(prefix)+6+strlen(TRACE_FILE) || strncmp(path,prefix,strlen(prefix)))goto done;
    for(const char *p=path+strlen(prefix);p<path+strlen(prefix)+6;p++)
        if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')))goto done;
    if(strcmp(path+strlen(prefix)+6,TRACE_FILE))goto done;
    char directory[128];memcpy(directory,path,strlen(prefix)+6);directory[strlen(prefix)+6]=0;
    struct statfs fs;struct stat st;
    if(lstat(directory,&st) || !S_ISDIR(st.st_mode) || statfs(directory,&fs) || fs.f_type!=TMPFS_MAGIC)goto done;
    for(unsigned i=0;i<DEVICE_COUNT;i++)if(!stat(devices[i],&st) && S_ISCHR(st.st_mode)) { device_ids[i]=st.st_rdev;have_device[i]=1; }
    #ifdef ZT30_PIN_TRACE_OUTPUT
    log_fd=zt30_trace_output_open(&log_output,path);
#else
    log_fd=(int)syscall(SYS_openat,AT_FDCWD,path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
#endif
    if(log_fd<0)goto done;
    char count_path[160];
    if(snprintf(count_path,sizeof(count_path),"%s/%s",directory,COUNTS_FILE)>=(int)sizeof(count_path)) { close(log_fd);log_fd=-1;goto done; }
    #ifdef ZT30_PIN_TRACE_OUTPUT
    counts_fd=zt30_trace_output_open(&counts_output,count_path);
#else
    counts_fd=(int)syscall(SYS_openat,AT_FDCWD,count_path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
#endif
    if(counts_fd<0) { close(log_fd);log_fd=-1;goto done; }
    owner=getpid();
    #ifdef ZT30_USB_TRACE
    /* No identity means no observation, never an empty successful capture. */
    if(!have_device[0]) { offset_write(FAILURE_MAGIC,8,0);goto done; }
#endif
    if(offset_write(TRACE_MAGIC,8,0)!=8) { close(log_fd);log_fd=-1; }
#ifdef ZT30_USB_HEADER_TRACE
    if(log_fd>=0)usb_header_begin(directory);
#endif
#ifdef ZT30_IOCTL_PAYLOAD_TRACE
    if(log_fd>=0)payload_begin(directory);
#endif
done:
    errno=saved;
}
/* Fixed-register entry aliases preserve libc's optional third/fourth argument
 * ABI without va_arg-reading an argument omitted by a two-argument caller. */
int trace_ioctl(int fd,unsigned long command,uintptr_t argument) __asm__("ioctl");
int trace_ioctl(int fd,unsigned long command,uintptr_t argument) {
    int incoming=errno;resolve();if(!next_ioctl) { errno=ENOSYS;return -1; }
    int device=identity(fd);uint32_t id=0;int sample=0;
#ifdef ZT30_IOCTL_PAYLOAD_TRACE
    struct zt30_payload_call payload_call={0};struct zt30_payload_record payload_record;unsigned owns_payload_busy=0;
#endif
    if(device>=0) {
        uint32_t request=(uint32_t)command;
        if(request) {
            uint32_t start=(request*2654435761U)>>(32-10);
            for(unsigned i=0;i<SLOTS;i++) {
                unsigned slot=(start+i)&(SLOTS-1);uint32_t key=__atomic_load_n(&keys[device][slot],__ATOMIC_ACQUIRE);
                if(!key && __sync_bool_compare_and_swap(&keys[device][slot],0,request))key=request;
                if(!key)key=__atomic_load_n(&keys[device][slot],__ATOMIC_ACQUIRE);
                if(key==request) { sample=__sync_fetch_and_add(&counts[device][slot],1)<16;break; }
                if(i==SLOTS-1)__sync_fetch_and_add(&untracked,1);
            }
        } else sample=__sync_fetch_and_add(&zero_counts[device],1)<16;
    }
#ifdef ZT30_USB_HEADER_TRACE
    struct zt30_usb_header_record header_record={0};
    #ifdef ZT30_USB_SETUP_TRACE
    header_record.fd=fd;header_record.generation=usb_setup_generation(fd);
    #endif
    int header_selected=usb_header_select(device,(uint32_t)command,&header_record);
#endif
    if(sample) {
        id=__sync_add_and_fetch(&call_counter,1);emit('B',(unsigned)device,fd,(uint32_t)command,0,0,id,0);
#ifdef ZT30_IOCTL_PAYLOAD_TRACE
        if(payload_fd>=0) {
            int emit_payload;
            if(payload_busy)emit_payload=zt30_payload_unavailable((unsigned)device,(uint32_t)command,major(device_ids[device]),minor(device_ids[device]),id,EDEADLK,&payload_call,&payload_record);
            else {
                payload_busy=1;owns_payload_busy=1;
                emit_payload=zt30_payload_before(&payload_reader,(unsigned)device,(uint32_t)command,major(device_ids[device]),minor(device_ids[device]),argument,id,&payload_call,&payload_record);
            }
            if(emit_payload)payload_store(&payload_record);
        }
#endif
    }
#ifdef ZT30_USB_HEADER_TRACE
    if(header_selected) {
        if(!id)header_record.call_id=__sync_add_and_fetch(&call_counter,1);
        else header_record.call_id=id;
        usb_header_before(fd,argument,&header_record);
    }
#endif
    errno=incoming;int result=next_ioctl(fd,command,(void*)argument);int error=errno;
#ifdef ZT30_IOCTL_PAYLOAD_TRACE
    if(id && payload_fd>=0 && zt30_payload_after(&payload_reader,&payload_call,result,&payload_record))payload_store(&payload_record);
    if(owns_payload_busy)payload_busy=0;
#endif
#ifdef ZT30_USB_HEADER_TRACE
    usb_header_after(device,(uint32_t)command,header_selected,&header_record,result,error);
#endif
    if(id)emit('A',(unsigned)device,fd,(uint32_t)command,result,result==-1?error:0,id,0);
    errno=error;return result;
}
static int opened(int fd,int flags,int error) {
    int device=identity(fd);
    #ifdef ZT30_USB_SETUP_TRACE
    if(device>=0) {
        uint32_t id=__sync_add_and_fetch(&call_counter,1);usb_setup_opened(fd,id);
        emit('O',(unsigned)device,fd,0,fd,0,id,(uint32_t)flags);
    }
    #else
    if(device>=0)emit('O',(unsigned)device,fd,0,fd,0,__sync_add_and_fetch(&call_counter,1),(uint32_t)flags);
    #endif
    errno=error;return fd;
}
int trace_open(const char *path,int flags,mode_t mode) __asm__("open");
int trace_open(const char *path,int flags,mode_t mode) { int incoming=errno;resolve();if(!next_open) { errno=ENOSYS;return -1; }errno=incoming;int fd=next_open(path,flags,mode);return opened(fd,flags,errno); }
int trace_open64(const char *path,int flags,mode_t mode) __asm__("open64");
int trace_open64(const char *path,int flags,mode_t mode) { int incoming=errno;resolve();if(!next_open64) { errno=ENOSYS;return -1; }errno=incoming;int fd=next_open64(path,flags,mode);return opened(fd,flags,errno); }
int trace_openat(int dir,const char *path,int flags,mode_t mode) __asm__("openat");
int trace_openat(int dir,const char *path,int flags,mode_t mode) { int incoming=errno;resolve();if(!next_openat) { errno=ENOSYS;return -1; }errno=incoming;int fd=next_openat(dir,path,flags,mode);return opened(fd,flags,errno); }

/* An independent bounded exit snapshot retains totals even after the event cap.
 * It is absent on SIGKILL/crash. Freeze entry accounting before snapshotting;
 * missing returns/in-flight races make strict capture validation fail closed. */
__attribute__((destructor)) static void finish(void) {
    int saved=errno;
    if(counts_fd<0 || getpid()!=owner)goto done;
    __atomic_store_n(&closing,1,__ATOMIC_RELEASE);
#ifdef ZT30_USB_HEADER_TRACE
    usb_header_finish();
#endif
#ifdef ZT30_IOCTL_PAYLOAD_TRACE
    payload_finish();
#endif
    uint32_t header[8]={0};memcpy(header,COUNTS_MAGIC,8);
    uint64_t ns=clock_ns();header[2]=(uint32_t)ns;header[3]=(uint32_t)(ns>>32);
    header[4]=__atomic_load_n(&untracked,__ATOMIC_RELAXED);
    header[5]=__atomic_load_n(&short_writes,__ATOMIC_RELAXED);
    header[6]=__atomic_load_n(&cap_reported,__ATOMIC_RELAXED);
    #ifdef ZT30_PIN_TRACE_OUTPUT
    if(!zt30_trace_output_matches(&counts_output))goto done;
#endif
    if(syscall(SYS_write,counts_fd,header,sizeof(header))!=(ssize_t)sizeof(header))goto done;
    for(unsigned device=0;device<DEVICE_COUNT;device++)for(unsigned slot=0;slot<=SLOTS;slot++) {
        uint32_t key=slot==SLOTS?0:__atomic_load_n(&keys[device][slot],__ATOMIC_ACQUIRE);
        if(!key && slot!=SLOTS)continue;
        uint32_t total=slot==SLOTS?__atomic_load_n(&zero_counts[device],__ATOMIC_RELAXED):__atomic_load_n(&counts[device][slot],__ATOMIC_RELAXED);
        if(!total)continue;
        struct count_record record={0};record.device=device;record.command=key;record.total=total;
        record.budget_granted=total<16?total:16;record.budget_skipped=total-record.budget_granted;
        #ifdef ZT30_PIN_TRACE_OUTPUT
        if(!zt30_trace_output_matches(&counts_output))goto done;
#endif
        if(syscall(SYS_write,counts_fd,&record,sizeof(record))!=(ssize_t)sizeof(record)) {
            #ifdef ZT30_PIN_TRACE_OUTPUT
            if(zt30_trace_output_matches(&counts_output))
#endif
            syscall(SYS_pwrite64,counts_fd,COUNTS_FAILURE_MAGIC,8,
#if defined(__arm__) && __SIZEOF_POINTER__==4
                0,0,0
#else
                (off_t)0
#endif
            );break;
        }
    }
    #ifdef ZT30_PIN_TRACE_OUTPUT
    if(zt30_trace_output_matches(&counts_output))close(counts_fd);
#else
    close(counts_fd);
#endif
    counts_fd=-1;
done:errno=saved;
}
