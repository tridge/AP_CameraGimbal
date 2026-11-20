/* Process-lifetime diagnostic output on a verified regular tmpfs file.
 * Pin a high descriptor and its inode; refuse writes if the application closes
 * or reuses it. No caller path or external descriptor is accepted here. */
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
struct zt30_trace_output {int fd;dev_t device;ino_t inode;};
static inline int zt30_trace_output_open(struct zt30_trace_output *file,const char *fixed_path) {
    file->fd=-1;
    int low=(int)syscall(SYS_openat,AT_FDCWD,fixed_path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(low<0)return -1;
    int high=fcntl(low,F_DUPFD_CLOEXEC,512);int error=errno;syscall(SYS_close,low);
    if(high<0) {errno=error;return -1;}
    struct stat st;struct statfs fs;
    if(fstat(high,&st) || fstatfs(high,&fs) || !S_ISREG(st.st_mode) || fs.f_type!=TMPFS_MAGIC) {syscall(SYS_close,high);errno=EPROTO;return -1;}
    file->fd=high;file->device=st.st_dev;file->inode=st.st_ino;return high;
}
static inline int zt30_trace_output_matches(const struct zt30_trace_output *file) {
    struct stat st;struct statfs fs;
    if(file->fd<0 || fstat(file->fd,&st) || fstatfs(file->fd,&fs))return 0;
    if(!S_ISREG(st.st_mode) || fs.f_type!=TMPFS_MAGIC || st.st_dev!=file->device || st.st_ino!=file->inode) {errno=ESTALE;return 0;}
    return 1;
}
