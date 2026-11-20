/* Initial payload scope: anonymous private RW stack of the current caller.
 * Mapping validation is cached per thread for at most one second. Callers
 * keep one guard copy per sampled ioctl and clear that copy after forwarding.
 * No heap/image/IQ/device mappings. A snapshot can still race a writer. */
#pragma once
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#define ZT30_SMAPS_LIMIT (256U*1024U)
struct zt30_stack_guard { uintptr_t low,high;pid_t owner,tid;uint64_t expires_ns; };
static inline uint64_t zt30_guard_now(void) {
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec;
}
struct zt30_maps_input { int fd;size_t used,length,total;unsigned retries;char data[4096]; };
static inline int zt30_user_range(uintptr_t address,size_t length) {
#if defined(__arm__) && __SIZEOF_POINTER__==4
    const uintptr_t top=0xc0000000U; /* captured kernel text starts c002c8bc */
#else
    const uintptr_t top=UINTPTR_MAX>>1;
#endif
    return address>=4096 && length && address<top && length<=top-address;
}
/* Returns 1 for a header, 0 for a non-header, -1 for malformed metadata. */
static inline int zt30_maps_header(const char *line,uintptr_t *low,uintptr_t *high,int *safe) {
    unsigned long a,b,offset;unsigned major,minor;unsigned long long inode;char perms[5]={0};int end=0;
    if(sscanf(line,"%lx-%lx %4s %lx %x:%x %llu %n",&a,&b,perms,&offset,&major,&minor,&inode,&end)!=7 || !end)return 0;
    if(a>=b || !zt30_user_range(a,b-a))return -1;
    const char *path=line+end;
    int stack_name=!strcmp(path,"[stack]");
    if(!strncmp(path,"[stack:",7)) {
        const char *c=path+7;while(*c>='0' && *c<='9')c++;
        stack_name=c>path+7 && !strcmp(c,"]");
    }
    *low=a;*high=b;
    *safe=!strcmp(perms,"rw-p") && !offset && !major && !minor && !inode && (!*path || stack_name);
    return 1;
}
static inline int zt30_maps_flags(const char *line) {
    if(strncmp(line,"VmFlags:",8))return -1;
    const char *p=line+8;int rd=0,wr=0;
    while(*p) {
        while(*p==' ' || *p=='\t')p++;
        if(!*p)break;
        const char *start=p;while(*p && *p!=' ' && *p!='\t')p++;
        size_t n=(size_t)(p-start);
        if(n!=2)return 0;
        if(!memcmp(start,"io",2) || !memcmp(start,"pf",2) || !memcmp(start,"mm",2))return 0;
        if(!memcmp(start,"rd",2))rd=1;
        if(!memcmp(start,"wr",2))wr=1;
    }
    return rd && wr;
}
static inline int zt30_maps_line(struct zt30_maps_input *in,char line[512]) {
    size_t n=0;
    for(;;) {
        if(in->used==in->length) {
            if(in->total>=ZT30_SMAPS_LIMIT)return -E2BIG;
            ssize_t count=syscall(SYS_read,in->fd,in->data,sizeof(in->data));
            if(count<0) {
                if(errno==EINTR && in->retries++<8)continue;
                return -errno;
            }
            if(!count)return n?-EPROTO:0;
            in->total+=(size_t)count;in->used=0;in->length=(size_t)count;
            if(in->total>ZT30_SMAPS_LIMIT)return -E2BIG;
        }
        char c=in->data[in->used++];
        if(c=='\n') {line[n]=0;return 1;}
        if(!c || n>=511)return -E2BIG;
        line[n++]=c;
    }
}
static inline int zt30_stack_guard_fd(struct zt30_stack_guard *guard,int fd,uintptr_t anchor) {
    int incoming=errno,error=EPERM,selected=0;char line[512];
    struct zt30_maps_input input={.fd=fd};memset(guard,0,sizeof(*guard));
    if(!zt30_user_range(anchor,1))goto done;
    for(;;) {
        int rc=zt30_maps_line(&input,line);if(rc<=0) {error=rc<0?-rc:EPROTO;break;}
        uintptr_t low=0,high=0;int safe=0;
        int header=zt30_maps_header(line,&low,&high,&safe);
        if(header<0) {error=EPROTO;break;}
        if(header) {
            if(selected) {error=EPROTO;break;} /* missing VmFlags */
            if(anchor>=low && anchor<high) {
                if(!safe)break;
                guard->low=low;guard->high=high;selected=1;
            }
        } else if(selected) {
            int flags=zt30_maps_flags(line);
            if(flags<0)continue;
            if(flags) {guard->owner=getpid();guard->tid=(pid_t)syscall(SYS_gettid);guard->expires_ns=zt30_guard_now()+1000000000U;error=0;}
            break;
        }
    }
done:
    if(error)memset(guard,0,sizeof(*guard));
    errno=incoming;return error;
}
static inline int zt30_stack_guard_init(struct zt30_stack_guard *guard,uintptr_t local_anchor) {
    /* Real pthread extent excludes anonymous VMAs merged alongside the stack.
     * Cache its validated intersection for at most one second per thread. */
    static __thread struct zt30_stack_guard cache;
    int incoming=errno,error=EPERM;uintptr_t internal_anchor=(uintptr_t)&incoming;
    uint64_t now=zt30_guard_now();pid_t pid=getpid(),tid=(pid_t)syscall(SYS_gettid);
    memset(guard,0,sizeof(*guard));
    if(now && cache.owner==pid && cache.tid==tid && now<cache.expires_ns &&
       local_anchor>=cache.low && local_anchor<cache.high && internal_anchor>=cache.low && internal_anchor<cache.high) {
        *guard=cache;error=0;goto done;
    }
    pthread_attr_t attr;void *stack=NULL;size_t size=0;
    error=pthread_getattr_np(pthread_self(),&attr);if(error)goto done;
    error=pthread_attr_getstack(&attr,&stack,&size);pthread_attr_destroy(&attr);if(error)goto done;
    uintptr_t low=(uintptr_t)stack;
    if(!zt30_user_range(low,size) || local_anchor<low || local_anchor-low>=size || internal_anchor<low || internal_anchor-low>=size) {error=EPERM;goto done;}
    int fd=(int)syscall(SYS_openat,AT_FDCWD,"/proc/self/smaps",O_RDONLY|O_NOFOLLOW|O_CLOEXEC,0);
    if(fd<0)error=errno;
    else {error=zt30_stack_guard_fd(guard,fd,local_anchor);syscall(SYS_close,fd);}
    if(!error) {
        if(guard->low<low)guard->low=low;
        if(guard->high>low+size)guard->high=low+size;
        guard->expires_ns=now+1000000000U;cache=*guard;
    }
done:
    if(error)memset(guard,0,sizeof(*guard));
    errno=incoming;return error;
}
static inline void zt30_stack_guard_clear(struct zt30_stack_guard *guard) {memset(guard,0,sizeof(*guard));}
static inline int zt30_stack_guard_check(const struct zt30_stack_guard *guard,uintptr_t address,size_t size) {
    if(guard->owner!=getpid() || guard->tid!=(pid_t)syscall(SYS_gettid))return EPERM;
    uint64_t now=zt30_guard_now();if(!now || now>=guard->expires_ns)return EPERM;
    if(!zt30_user_range(address,size) || (address&3) || address<guard->low || address>=guard->high || size>guard->high-address)return EPERM;
    return 0;
}
