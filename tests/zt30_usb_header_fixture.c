/* Synthetic private allocation fixture. All ioctls terminate in fake stub. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
struct header {
 uint8_t type,endpoint;uint16_t pad;int32_t status;uint32_t flags,buffer;
 int32_t length,actual,start,packets,errors;uint32_t signal,context;
};
_Static_assert(sizeof(struct header)==44,"ARM32 ABI fixture");
static int fd,wrong;static const char *mode;static struct header *early,*late;
static uint64_t tick(clockid_t which) {struct timespec t;assert(!clock_gettime(which,&t));return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void calls(struct header *h,unsigned n) {
 for(unsigned i=0;i<n;i++) {
  struct rusage ru0,ru1;uint64_t wall=0,cpu=0;int measure=!i && n==64;
  if(measure) {assert(!getrusage(RUSAGE_THREAD,&ru0));cpu=tick(CLOCK_THREAD_CPUTIME_ID);wall=tick(CLOCK_MONOTONIC);}
  errno=EDOM;assert(ioctl(fd,0x802c550aUL,!strcmp(mode,"invalid")?(void*)0xdeadbeef:h)==123);assert(errno==EDOM);
  if(measure) {wall=tick(CLOCK_MONOTONIC)-wall;cpu=tick(CLOCK_THREAD_CPUTIME_ID)-cpu;assert(!getrusage(RUSAGE_THREAD,&ru1));printf("WINDOW phase=%s wall_ns=%llu cpu_ns=%llu voluntary=%ld involuntary=%ld\n",h==early?"pre":"post",(unsigned long long)wall,(unsigned long long)cpu,ru1.ru_nvcsw-ru0.ru_nvcsw,ru1.ru_nivcsw-ru0.ru_nivcsw);}
 }
}
static void *worker(void *unused) {
 (void)unused;
 /* Non-target ALT must not consume the pre-ALT phase. */
 errno=EDOM;assert(ioctl(wrong,0x80085504UL,(void*)0xdeadbeef)==0 && errno==EDOM);
 if(!strcmp(mode,"reuse")) {
  int tmp=open("replacement",O_CREAT|O_WRONLY|O_EXCL,0600);assert(tmp>=0);assert(dup2(tmp,514)==514);close(tmp);
 }
 if(!strcmp(mode,"fork")) {
  pid_t p=fork();assert(p>=0);if(!p) {calls(early,3);exit(0);}int status;assert(waitpid(p,&status,0)==p && status==0);
 }
 calls(early,64);errno=EAGAIN;assert(ioctl(fd,0x80085504UL,(void*)0xdeadbeef)==-1 && errno==EAGAIN);
 calls(early,4);errno=EDOM;assert(ioctl(fd,0x80085504UL,(void*)0xdeadbeef)==0 && errno==EDOM);
 calls(late,64);errno=EDOM;assert(ioctl(fd,0xf001UL)==123 && errno==EDOM);
 return NULL;
}
int main(int argc,char **argv) {
 mode=argc>1?argv[1]:"normal";setvbuf(stdout,NULL,_IOLBF,0);
 if(getenv("APCG_FIXTURE_PRIORITY")) {assert(!setpriority(PRIO_PROCESS,0,-20));errno=0;assert(getpriority(PRIO_PROCESS,0)==-20 && !errno);}
 /* 55 resident anonymous regions separated by protected gaps. Large resident
  * malloc allocation models vendor page-table generation, never device maps. */
 volatile unsigned char *large=malloc(16U*1024*1024);assert(large);
 for(size_t i=0;i<16U*1024*1024;i+=4096)large[i]=7;
 size_t pagesize=(size_t)sysconf(_SC_PAGESIZE);assert(pagesize==4096);
 void *arena=mmap((void*)0x80000000,110*pagesize,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(arena!=MAP_FAILED);
 for(unsigned i=0;i<55;i++) {void *p=(char*)arena+i*2*pagesize;assert(!mprotect(p,pagesize,PROT_READ|PROT_WRITE));memset(p,3,pagesize);}
 early=calloc(1,sizeof(*early));assert(early);
 late=mmap((void*)0xbff00000,pagesize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(late!=MAP_FAILED);
 *early=(struct header){.type=2,.endpoint=0,.buffer=0xdeadbeef,.length=42,.context=0xdeadbabe};
 *late=(struct header){.type=0,.endpoint=0x81,.buffer=0xbaadf00d,.length=49152,.packets=16,.context=0xdeadbeef};
 fd=open("/dev/null",O_RDWR);wrong=open("/dev/zero",O_RDWR);assert(fd>=0 && wrong>=0);
 /* Record metadata size/VMA count without mapped contents. */
 FILE *sm=fopen("/proc/self/smaps","r");assert(sm);size_t bytes=0,vmas=0;char line[512];
 while(fgets(line,sizeof(line),sm)) {bytes+=strlen(line);unsigned long a,b;if(sscanf(line,"%lx-%lx",&a,&b)==2)vmas++;}fclose(sm);
#if defined(__arm__) && __SIZEOF_POINTER__==4
 assert(vmas>=140 && vmas<=170 && bytes>=75*1024 && bytes<=105*1024);
#endif
 printf("LAYOUT vmas=%zu smaps_bytes=%zu resident_heap=16777216 anonymous_regions=55\n",vmas,bytes);
 if(!strcmp(mode,"main"))worker(NULL);
 else {pthread_t t;pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstacksize(&attr,!strcmp(mode,"small")?32768:131072));assert(!pthread_create(&t,&attr,worker,NULL));
 if(!strcmp(mode,"concurrent")) {pthread_t t2;assert(!pthread_create(&t2,&attr,worker,NULL));assert(!pthread_join(t2,NULL));}
 assert(!pthread_join(t,NULL));pthread_attr_destroy(&attr);}
 if(!strcmp(mode,"reuse")) {off_t n=lseek(514,0,SEEK_END);assert(n==0);assert(write(514,"untouched",9)==9);}
 puts("PASS fake forwarded target_calls=135 nested buffers never accessed");return 0;
}
