/* RAM-only, read-only sequenced kernel logger. Never loads an MI SDK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#ifndef ZT30_KMSG_DEVICE
#define ZT30_KMSG_DEVICE "/dev/kmsg"
#endif
#define CAP (8U*1024U*1024U)
#define TMPFS_MAGIC 0x01021994
static volatile sig_atomic_t stopped;
static void stop(int sig) { (void)sig;stopped=1; }
static uint64_t now_ns(void) { struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec; }
static uint64_t boot_ns(void) { struct timespec t;if(clock_gettime(CLOCK_BOOTTIME,&t))return 0;return (uint64_t)t.tv_sec*1000000000U+(uint64_t)t.tv_nsec; }
static int write_all(int fd,const void *data,size_t size) {
    const unsigned char *p=data;
    while(size) { ssize_t n=write(fd,p,size);if(n<0 && errno==EINTR)continue;if(n<=0)return -1;p+=n;size-=(size_t)n; }
    return 0;
}
int main(int argc,char **argv) {
    char path[256],*end;struct statfs fs;struct stat st;
    if(argc!=3 || strlen(argv[1])!=strlen("/tmp/apcg-zt30.session.")+6 ||
       strncmp(argv[1],"/tmp/apcg-zt30.session.",strlen("/tmp/apcg-zt30.session.")))return 2;
    for(const char *p=argv[1]+strlen("/tmp/apcg-zt30.session.");*p;p++)
        if(!((*p>='A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')))return 2;
    errno=0;long seconds=strtol(argv[2],&end,10);
    if(errno || *end || seconds<1 || seconds>300)return 2;
    if(lstat(argv[1],&st) || !S_ISDIR(st.st_mode) || statfs(argv[1],&fs) || fs.f_type!=TMPFS_MAGIC)return 2;
    int input=open(ZT30_KMSG_DEVICE,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(input<0) { perror("kmsg open");return 3; }
    if(fstat(input,&st) || !S_ISCHR(st.st_mode) || major(st.st_rdev)!=1 || minor(st.st_rdev)!=11) {
        fputs("unexpected kmsg device\n",stderr);close(input);return 3;
    }
    /* SEEK_END chooses new records without clearing the global log or changing
     * another reader's cursor. A fresh cursor is essential for gap attribution. */
    if(lseek(input,0,SEEK_END)<0) { perror("kmsg seek end");close(input);return 3; }
    if(snprintf(path,sizeof(path),"%s/kmsg.bin",argv[1])>=(int)sizeof(path)) { close(input);return 2; }
    int output=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(output<0) { perror("kmsg output");close(input);return 3; }
    struct sigaction action={0};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,NULL);sigaction(SIGINT,&action,NULL);sigaction(SIGHUP,&action,NULL);
    uint64_t start=now_ns(),start_boot=boot_ns(),drain_deadline=0,last_sequence=0,last_printk_us=0,last_read_boot=0;uint32_t bytes=8,records=0,gaps=0;int rc=0,caught_up=0;
    if(!start || !start_boot || write_all(output,"ZT30KMG1",8)) { close(input);close(output);return 4; }
    printf("READY pid=%ld start_ns=%llu boottime_ns=%llu\n",(long)getpid(),(unsigned long long)start,(unsigned long long)start_boot);fflush(stdout);
    while(!rc) {
        uint64_t now=now_ns();
        if((stopped || now-start>=(uint64_t)seconds*1000000000U) && !drain_deadline)drain_deadline=now+500000000U;
        if(drain_deadline && now>=drain_deadline) { rc=8;break; }
        char data[16385];ssize_t n=read(input,data,sizeof(data)-1);
        if(n<0) {
            if(errno==EPIPE) { gaps++;continue; }
            if(errno==EINTR)continue;
            if(errno==EAGAIN) { if(drain_deadline) { caught_up=1;break; }struct timespec delay={0,100000000};nanosleep(&delay,NULL);continue; }
            perror("kmsg read");rc=5;break;
        }
        if(n==0) { fputs("unexpected kmsg EOF\n",stderr);rc=5;break; }
        data[n]=0;uint32_t length=(uint32_t)n;
        if(bytes>CAP-4-length) { fputs("kernel log cap reached\n",stderr);rc=6;break; }
        if(write_all(output,&length,4) || write_all(output,data,(size_t)n)) { rc=4;break; }
        bytes+=4+length;records++;
        /* The host decoder remains authoritative; this scalar marks the cursor
         * reached by the drain, not a replacement for its sequence audit. */
        unsigned long long sequence=0,printk_us=0;last_read_boot=boot_ns();if(sscanf(data,"%*u,%llu,%llu,",&sequence,&printk_us)==2) { last_sequence=(uint64_t)sequence;last_printk_us=(uint64_t)printk_us; }
    }
    close(input);if(close(output) && !rc)rc=4;
    printf("FINISH rc=%d records=%u bytes=%u read_gaps=%u stopped=%d caught_up=%d last_sequence=%llu end_boottime_ns=%llu last_read_boottime_ns=%llu last_printk_us=%llu\n",rc,records,bytes,gaps,(int)stopped,caught_up,(unsigned long long)last_sequence,(unsigned long long)boot_ns(),(unsigned long long)last_read_boot,(unsigned long long)last_printk_us);fflush(stdout);
    return rc?rc:(gaps?7:0);
}
