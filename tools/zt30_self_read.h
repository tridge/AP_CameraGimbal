/* Fixed own-process metadata reader. Never dereference a caller pointer.
 * /proc/self/mem can read PROT_NONE pages; callers whitelist owned SDK buffers. */
#pragma once
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "zt30_stack_guard.h"
#define ZT30_SELF_COPY_MAX 256U
struct zt30_self_reader { int fd;pid_t owner;dev_t device;ino_t inode; };
struct zt30_copy_result { ssize_t bytes;int error; };
static int zt30_self_reader_init(struct zt30_self_reader *reader) {
    int incoming=errno,error=0;struct stat st;struct statfs fs;
    reader->owner=getpid();reader->fd=(int)syscall(SYS_openat,AT_FDCWD,"/proc/self/mem",O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0);
    if(reader->fd<0)error=errno;
    else if(fstat(reader->fd,&st) || fstatfs(reader->fd,&fs))error=errno;
    else if(!S_ISREG(st.st_mode) || fs.f_type!=0x9fa0)error=EPROTO;
    if(!error) {reader->device=st.st_dev;reader->inode=st.st_ino;}
    if(error && reader->fd>=0) {syscall(SYS_close,reader->fd);reader->fd=-1;}
    errno=incoming;return error;
}
static struct zt30_copy_result zt30_self_copy(const struct zt30_self_reader *reader,void *destination,uintptr_t source,size_t size) {
    int incoming=errno;struct zt30_copy_result result={-1,EINVAL};
    if(!size || size>ZT30_SELF_COPY_MAX || !destination || reader->fd<0)goto done;
    if(getpid()!=reader->owner) {result.error=EPERM;goto done;}
    /* libc stat functions are not interposed by the observer; preserve ABI. */
    struct stat st;struct statfs fs;
    if(fstat(reader->fd,&st) || fstatfs(reader->fd,&fs)) {result.error=errno;goto done;}
    if(!S_ISREG(st.st_mode) || fs.f_type!=0x9fa0 || st.st_dev!=reader->device || st.st_ino!=reader->inode) {result.error=EPERM;goto done;}
#if defined(__arm__) && __SIZEOF_POINTER__==4
    /* ARM EABI pairs the64-bit offset on an even register: padding before low.
     * Virtual addresses above2GiB are zero-extended, not signed off_t32. */
    result.bytes=syscall(SYS_pread64,reader->fd,destination,size,0,(uint32_t)source,0);
#else
    result.bytes=syscall(SYS_pread64,reader->fd,destination,size,(off_t)source);
#endif
    result.error=result.bytes<0?errno:0;
    if(result.bytes>(ssize_t)size) {result.bytes=-1;result.error=EOVERFLOW;}
done:
    errno=incoming;return result;
}
static void zt30_self_reader_close(struct zt30_self_reader *reader) {
    int incoming=errno;if(reader->fd>=0)syscall(SYS_close,reader->fd);reader->fd=-1;errno=incoming;
}

/* Only this entry may be used by the payload observer. The unguarded entry
 * above exists for the no-SDK memory-method fixture, including PROT_NONE. */
static inline struct zt30_copy_result zt30_self_copy_guarded(const struct zt30_self_reader *reader,const struct zt30_stack_guard *guard,void *destination,uintptr_t source,size_t size) {
    int incoming=errno;int error=zt30_stack_guard_check(guard,source,size);errno=incoming;
    if(error)return (struct zt30_copy_result){-1,error};
    return zt30_self_copy(reader,destination,source,size);
}
