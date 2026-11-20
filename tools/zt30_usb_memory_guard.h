/* Independent USB diagnostic guard. Does not expand MI stack-read permission.
 * Fresh anonymous private RW smaps validation; no device/PFN or nested reads.
 * Validation and pread are separate observations, not an atomic memory view. */
#pragma once
#include "zt30_self_read.h"
#include <signal.h>
#define ZT30_USB_COPY_DEADLINE_NS 50000000ULL
struct zt30_usb_proc_file { int fd;pid_t owner;dev_t device;ino_t inode; };
static inline int zt30_usb_proc_identity(const struct zt30_usb_proc_file *f) {
    struct stat st;struct statfs fs;
    return f->fd>=0 && f->owner==getpid() && !fstat(f->fd,&st) && !fstatfs(f->fd,&fs) &&
        S_ISREG(st.st_mode) && fs.f_type==0x9fa0 && st.st_dev==f->device && st.st_ino==f->inode;
}
static inline int zt30_usb_proc_matches(const struct zt30_usb_proc_file *f) {
    return f->fd>=512 && zt30_usb_proc_identity(f);
}
static inline int zt30_usb_proc_open(struct zt30_usb_proc_file *f,int memory) {
    int incoming=errno,error=0;struct stat st,expected;struct statfs fs;
    *f=(struct zt30_usb_proc_file){.fd=-1,.owner=getpid()};
    const char *path=memory?"/proc/self/mem":"/proc/self/smaps";
    int low=(int)syscall(SYS_openat,AT_FDCWD,path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0);
    if(low<0) {error=errno;goto done;}
    /* If initial validation cannot establish identity, never close a possibly
     * reused low fd. An unavailable diagnostic may leak it until process exit. */
    if(fstat(low,&st) || fstatfs(low,&fs)) {error=errno;goto done;}
    if(!S_ISREG(st.st_mode) || fs.f_type!=0x9fa0 || stat(path,&expected) ||
       st.st_dev!=expected.st_dev || st.st_ino!=expected.st_ino) {error=EPROTO;goto done;}
    f->device=st.st_dev;f->inode=st.st_ino;
    f->fd=fcntl(low,F_DUPFD_CLOEXEC,512);error=f->fd<0?errno:0;
    struct zt30_usb_proc_file initial={.fd=low,.owner=f->owner,.device=f->device,.inode=f->inode};
    if(zt30_usb_proc_identity(&initial))syscall(SYS_close,low);
    if(!error && !zt30_usb_proc_matches(f))error=EPERM;
done:errno=incoming;return error;
}
static inline int zt30_usb_smaps_open(struct zt30_usb_proc_file *f) {return zt30_usb_proc_open(f,0);}
static inline void zt30_usb_smaps_close(struct zt30_usb_proc_file *f) {
    int incoming=errno;if(zt30_usb_proc_matches(f))syscall(SYS_close,f->fd);f->fd=-1;errno=incoming;
}
struct zt30_usb_memory_guard {uintptr_t low,high;pid_t owner,tid;uint64_t deadline;size_t consumed;};
/* Streaming parser also accepts a disposable fixture callback. Only init()
 * supplies mapping permission to the production snapshot entry below. */
typedef ssize_t (*zt30_usb_metadata_read)(void *,void *,size_t);
struct zt30_usb_metadata_input {
    zt30_usb_metadata_read read;void *context;size_t used,length,total;unsigned retries;
    uint64_t deadline;char data[4096];
};
static inline int zt30_usb_metadata_line(struct zt30_usb_metadata_input *in,char line[512]) {
    size_t n=0;
    for(;;) {
        if(in->used==in->length) {
            if(zt30_guard_now()>=in->deadline)return -ETIMEDOUT;
            if(in->total>=ZT30_SMAPS_LIMIT)return -E2BIG;
            ssize_t count=in->read(in->context,in->data,sizeof(in->data));
            if(count<0) {if(errno==EINTR && in->retries++<8)continue;return -errno;}
            if(!count)return n?-EPROTO:0;
            if((size_t)count>sizeof(in->data))return -EPROTO;
            in->total+=(size_t)count;in->used=0;in->length=(size_t)count;
            if(in->total>ZT30_SMAPS_LIMIT)return -E2BIG;
        }
        char c=in->data[in->used++];
        if(c=='\n') {line[n]=0;return 1;}
        if(!c || n>=511)return -E2BIG;
        line[n++]=c;
    }
}
static inline int zt30_usb_mapping_header(const char *line,uintptr_t *low,uintptr_t *high,int *safe) {
    /* Most smaps lines are counters, not VMA headers. Avoid sscanf/strtol
     * on thousands of metadata lines on the camera's older ARM libc. */
    const char *p=line;size_t digits=0;
    while((*p>='0' && *p<='9') || (*p>='a' && *p<='f')) {p++;if(++digits>2*sizeof(uintptr_t))return 0;}
    if(!digits || *p!='-')return 0;
    int rc=zt30_maps_header(line,low,high,safe);
    if(rc==1 && !*safe) {
        unsigned long a,b,offset;unsigned major,minor;unsigned long long inode;char perms[5]={0};int end=0;
        if(sscanf(line,"%lx-%lx %4s %lx %x:%x %llu %n",&a,&b,perms,&offset,&major,&minor,&inode,&end)==7 && end)
            *safe=!strcmp(perms,"rw-p") && !offset && !major && !minor && !inode && !strcmp(line+end,"[heap]");
    }
    return rc;
}
static inline int zt30_usb_guard_parse(struct zt30_usb_memory_guard *g,zt30_usb_metadata_read read,
                                     void *context,uintptr_t source,size_t size,uint64_t deadline) {
    int incoming=errno,error=EPERM,selected=0;char line[512];memset(g,0,sizeof(*g));
    struct zt30_usb_metadata_input in={.read=read,.context=context,.deadline=deadline};
    if(!zt30_user_range(source,size) || (source&3) || size>ZT30_SELF_COPY_MAX) {error=EINVAL;goto done;}
    for(;;) {
        int rc=zt30_usb_metadata_line(&in,line);if(rc<=0) {error=rc<0?-rc:EPROTO;break;}
        uintptr_t low=0,high=0;int safe=0;int header=zt30_usb_mapping_header(line,&low,&high,&safe);
        if(header<0) {error=EPROTO;break;}
        if(header) {
            if(selected) {error=EPROTO;break;}
            if(source>=low && source<high) {
                if(!safe || size>high-source)break;
                g->low=low;g->high=high;selected=1;
            }
        } else if(selected) {
            int flags=zt30_maps_flags(line);if(flags<0)continue;
            if(flags && zt30_guard_now()>=deadline)error=ETIMEDOUT;
            else if(flags) {
                g->owner=getpid();g->tid=(pid_t)syscall(SYS_gettid);g->deadline=deadline;error=0;
            }
            break;
        }
    }
done:
    if(error)memset(g,0,sizeof(*g));
    g->consumed=in.total;errno=incoming;return error;
}
static inline ssize_t zt30_usb_smaps_read(void *context,void *data,size_t size) {
    struct zt30_usb_proc_file *f=context;
    if(!zt30_usb_proc_matches(f)) {errno=EPERM;return -1;}
    return syscall(SYS_read,f->fd,data,size);
}
static inline int zt30_usb_guard_init(struct zt30_usb_memory_guard *g,uintptr_t source,size_t size,uint64_t deadline) {
    int incoming=errno;struct zt30_usb_proc_file f;int error=zt30_usb_smaps_open(&f);
    if(!error)error=zt30_usb_guard_parse(g,zt30_usb_smaps_read,&f,source,size,deadline);
    else memset(g,0,sizeof(*g));
    zt30_usb_smaps_close(&f);errno=incoming;return error;
}
static inline int zt30_usb_guard_check(const struct zt30_usb_memory_guard *g,uintptr_t source,size_t size) {
    if(g->owner!=getpid() || g->tid!=(pid_t)syscall(SYS_gettid))return EPERM;
    if(zt30_guard_now()>=g->deadline)return ETIMEDOUT;
    if(!zt30_user_range(source,size) || size>ZT30_SELF_COPY_MAX || (source&3) || source<g->low || source>=g->high || size>g->high-source)return EPERM;
    return 0;
}
static inline int zt30_usb_reader_matches(const struct zt30_self_reader *r) {
    struct zt30_usb_proc_file f={.fd=r->fd,.owner=r->owner,.device=r->device,.inode=r->inode};
    return zt30_usb_proc_matches(&f);
}
static inline int zt30_usb_reader_init(struct zt30_self_reader *r) {
    /* Reuse the qualified flat copier, not its legacy low-fd initializer. */
    (void)&zt30_self_reader_init;
    struct zt30_usb_proc_file f;int error=zt30_usb_proc_open(&f,1);
    r->fd=f.fd;r->owner=f.owner;r->device=f.device;r->inode=f.inode;return error;
}
static inline void zt30_usb_reader_close(struct zt30_self_reader *r) {
    int incoming=errno;if(zt30_usb_reader_matches(r))zt30_self_reader_close(r);else r->fd=-1;errno=incoming;
}
