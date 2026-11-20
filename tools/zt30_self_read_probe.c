#define _GNU_SOURCE
#include "zt30_self_read.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#if defined(__arm__)
_Static_assert(SYS_process_vm_readv==376,"pinned ARM EABI self-read syscall");
_Static_assert(sizeof(uintptr_t)==4,"ARM32 pointer ABI");
#endif
static struct zt30_self_reader reader={.fd=-1};
struct guarded { unsigned char left[32],data[ZT30_SELF_COPY_MAX],right[32]; };
static int check_guards(const struct guarded *buffer) {
    for(unsigned i=0;i<32;i++)if(buffer->left[i]!=0xa5 || buffer->right[i]!=0xa5)return 0;
    return 1;
}
static struct zt30_copy_result read_checked(struct guarded *buffer,uintptr_t source,size_t size) {
    memset(buffer,0xa5,sizeof(*buffer));errno=EINTR;
    struct zt30_copy_result result=zt30_self_copy(&reader,buffer->data,source,size);
    if(errno!=EINTR || !check_guards(buffer)) { result.bytes=-2;result.error=EOVERFLOW; }
    return result;
}
static void *race_page;static size_t page_size;static uint32_t go;static int unmap_rc;
static void *unmapper(void *unused) {
    (void)unused;while(!__atomic_load_n(&go,__ATOMIC_ACQUIRE)) { struct timespec t={0,10000};nanosleep(&t,NULL); }
    struct timespec t={0,2000000};nanosleep(&t,NULL);
    unmap_rc=munmap(race_page,page_size);return NULL;
}
static int guarded_stack_case(void) {
    struct zt30_stack_guard guard;uint32_t pattern[8]={1,2,3,4,5,6,7,8},output[8]={0};
    errno=EINTR;
    if(zt30_stack_guard_init(&guard,(uintptr_t)&guard) || errno!=EINTR)return 1;
    struct zt30_copy_result c=zt30_self_copy_guarded(&reader,&guard,output,(uintptr_t)pattern,sizeof(pattern));
    if(c.bytes!=sizeof(pattern) || c.error || errno!=EINTR || memcmp(pattern,output,sizeof(pattern)))return 2;
    uint64_t expiry=guard.expires_ns;guard.expires_ns=1;
    if(!zt30_stack_guard_check(&guard,(uintptr_t)pattern,4))return 6;
    guard.expires_ns=expiry;
    pid_t child=fork();if(child<0)return 7;
    if(!child)_exit(zt30_stack_guard_check(&guard,(uintptr_t)pattern,4)==EPERM?0:1);
    int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))return 8;
    if(!zt30_stack_guard_check(&guard,(uintptr_t)pattern+1,4) || !zt30_stack_guard_check(&guard,UINTPTR_MAX-1,32) || !zt30_stack_guard_check(&guard,(uintptr_t)&reader,4))return 3;
    void *page=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(page==MAP_FAILED)return 4;
    c=zt30_self_copy_guarded(&reader,&guard,output,(uintptr_t)page,32);
    if(munmap(page,4096) || c.bytes!=-1 || c.error!=EPERM)return 5;
    return 0;
}
static void *guarded_thread(void *unused) { (void)unused;return (void*)(uintptr_t)guarded_stack_case(); }
int main(int argc,char **argv) {
    (void)argv;if(argc!=1)return 2;setvbuf(stdout,NULL,_IOLBF,0);
    errno=EINTR;int init_error=zt30_self_reader_init(&reader);
    if(init_error || errno!=EINTR) {printf("INIT error=%d\n",init_error);return 23;}
    struct guarded buffer;unsigned char pattern[ZT30_SELF_COPY_MAX];
    for(unsigned i=0;i<sizeof(pattern);i++)pattern[i]=(unsigned char)i;
    struct zt30_copy_result r=read_checked(&buffer,(uintptr_t)pattern,sizeof(pattern));
    if(r.bytes!=(ssize_t)sizeof(pattern)||r.error || memcmp(buffer.data,pattern,sizeof(pattern)))return 3;
    puts("CASE valid PASS");
    unsigned char byte=0;struct iovec local={&byte,1},remote={pattern,1};errno=0;
    ssize_t vm=syscall(SYS_process_vm_readv,getpid(),&local,1,&remote,1,0);int vm_error=vm<0?errno:0;
    printf("VM_SUPPORT syscall=%d result=%ld error=%d\n",SYS_process_vm_readv,(long)vm,vm_error);
    r=read_checked(&buffer,0,16);if(r.bytes!=-1||(r.error!=EIO && r.error!=EFAULT))return 4;puts("CASE null PASS");
    r=read_checked(&buffer,1,16);if(r.bytes!=-1||(r.error!=EIO && r.error!=EFAULT))return 5;puts("CASE unmapped PASS");
    r=read_checked(&buffer,(uintptr_t)pattern,0);if(r.bytes!=-1||r.error!=EINVAL)return 6;
    r=read_checked(&buffer,(uintptr_t)pattern,ZT30_SELF_COPY_MAX+1);if(r.bytes!=-1||r.error!=EINVAL)return 7;puts("CASE bounds PASS");
    page_size=(size_t)sysconf(_SC_PAGESIZE);if(page_size<4096 || page_size>65536)return 8;
    unsigned char *pages=mmap(NULL,page_size*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(pages==MAP_FAILED)return 9;
    memset(pages,0x42,page_size*2);if(mprotect(pages+page_size,page_size,PROT_NONE))return 10;
    r=read_checked(&buffer,(uintptr_t)(pages+page_size),32);if(r.bytes!=32 || r.error)return 11;
    for(unsigned i=0;i<32;i++)if(buffer.data[i]!=0x42)return 24;
    puts("CASE protected_read PASS");
    if(munmap(pages+page_size,page_size))return 27;
    r=read_checked(&buffer,(uintptr_t)(pages+page_size-32),64);
    if(r.bytes!=32 || r.error)return 12;
    for(unsigned i=0;i<64;i++)if(buffer.data[i]!=(i<32?0x42:0xa5))return 13;
    puts("CASE short_page PASS");
    if(munmap(pages,page_size))return 14;
    /* Reserve surrounding pages; unmap only our middle page, never MAP_FIXED.
     * Reader uses no userspace dereference or allocation during the race. */
    unsigned char *window=mmap(NULL,page_size*10,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(window==MAP_FAILED)return 15;
    race_page=window+page_size*5;if(mprotect(race_page,page_size,PROT_READ|PROT_WRITE))return 16;
    memset(race_page,0x53,page_size);pthread_t thread;if(pthread_create(&thread,NULL,unmapper,NULL))return 17;
    r=read_checked(&buffer,(uintptr_t)race_page,32);if(r.bytes!=32 || r.error)return 22;
    __atomic_store_n(&go,1,__ATOMIC_RELEASE);unsigned valid=0,faults=0;
    struct timespec started;clock_gettime(CLOCK_MONOTONIC,&started);
    for(unsigned i=0;i<250000;i++) {
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        if((now.tv_sec-started.tv_sec)*1000000000LL+now.tv_nsec-started.tv_nsec>1000000000LL)break;
        r=read_checked(&buffer,(uintptr_t)race_page,32);
        if(r.bytes==32 && !r.error) { for(unsigned j=0;j<32;j++)if(buffer.data[j]!=0x53)return 18;valid++; }
        else if(r.bytes==-1 && (r.error==EIO || r.error==EFAULT))faults++;
        else return 19;
        if(faults>=1000)break;
    }
    if(pthread_join(thread,NULL)||unmap_rc || !valid || !faults)return 20;
    if(munmap(window,page_size*5) || munmap(window+page_size*6,page_size*4))return 21;
    printf("CASE concurrent_unmap PASS valid=%u faults=%u\n",valid,faults);
    pid_t child=fork();if(child<0)return 25;
    if(child==0) {struct zt30_copy_result c=zt30_self_copy(&reader,buffer.data,(uintptr_t)pattern,16);_exit(c.bytes==-1 && c.error==EPERM?0:1);}
    int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status))return 26;
    puts("CASE fork_exclusion PASS");
    if(guarded_stack_case())return 28;
    pthread_t guard_thread;void *thread_result=NULL;
    if(pthread_create(&guard_thread,NULL,guarded_thread,NULL) || pthread_join(guard_thread,&thread_result) || thread_result)return 29;
    puts("CASE stack_guard PASS main_and_pthread");
    int old_fd=reader.fd;zt30_self_reader_close(&reader);
    FILE *file=tmpfile();if(!file)return 30;
    if(fileno(file)!=old_fd && dup2(fileno(file),old_fd)!=old_fd)return 31;
    reader.fd=old_fd;
    r=read_checked(&buffer,(uintptr_t)pattern,16);if(r.bytes!=-1 || r.error!=EPERM)return 32;
    if(fileno(file)!=old_fd)close(old_fd);
    fclose(file);reader.fd=-1;
    int other=(int)syscall(SYS_openat,AT_FDCWD,"/proc/self/maps",O_RDONLY|O_CLOEXEC,0);if(other<0)return 33;
    if(other!=old_fd && dup2(other,old_fd)!=old_fd)return 34;
    reader.fd=old_fd;r=read_checked(&buffer,(uintptr_t)pattern,16);
    if(r.bytes!=-1 || r.error!=EPERM)return 35;
    if(other!=old_fd)close(old_fd);
    close(other);reader.fd=-1;puts("CASE fd_reuse PASS tmpfs_and_procfs");
    puts("RESULT PASS method=proc-self-mem cases=10");return 0;
}
