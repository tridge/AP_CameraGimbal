/* Own control bytes and fake-next-ioctl; never accesses a USB peripheral. */
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
#include <time.h>
#include <unistd.h>
struct header {uint8_t type,endpoint;uint16_t pad;int32_t status;uint32_t flags,buffer;int32_t length,actual,start,packets,errors;uint32_t signal,context;};
_Static_assert(sizeof(struct header)==44,"ARM32 header");
static int fd,wrong;static const char *mode;static struct header *h;static uint8_t *control;
static uint64_t tick(clockid_t which) {struct timespec t;assert(!clock_gettime(which,&t));return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void submit(unsigned selector,unsigned offset) {
 uint8_t *p=control+offset;memset(p,0,42);p[0]=!strcmp(mode,"in")?0xa1:0x21;p[1]=!strcmp(mode,"in")?0x81:1;p[3]=(uint8_t)selector;p[4]=1;p[6]=34;
 p[8+2]=1;p[8+3]=2;p[8+4]=0x2a;p[8+5]=0x2c;p[8+6]=0x0a; /* 666666 interval */
 h->buffer=(uint32_t)(uintptr_t)p;h->length=42;
 if(!strcmp(mode,"invalid"))h->buffer=0xdeadbeef;
 uint64_t c0=tick(CLOCK_THREAD_CPUTIME_ID),t0=tick(CLOCK_MONOTONIC);errno=EDOM;assert(ioctl(fd,0x802c550aUL,h)==123 && errno==EDOM);
 printf("WINDOW selector=%u wall_ns=%llu cpu_ns=%llu\n",selector,(unsigned long long)(tick(CLOCK_MONOTONIC)-t0),(unsigned long long)(tick(CLOCK_THREAD_CPUTIME_ID)-c0));
}
static void *worker(void *ignored) {
 (void)ignored;
 errno=EDOM;assert(ioctl(wrong,0x8004550fUL,(void*)0xdeadbeef)==0 && errno==EDOM);
 /* Before a target claim, nested arguments remain opaque. */
 errno=EDOM;assert(ioctl(fd,0x802c550aUL,(void*)0xdeadbeef)==123 && errno==EDOM);
 errno=EAGAIN;assert(ioctl(fd,0x8004550fUL,(void*)0xdeadbeef)==-1 && errno==EAGAIN);
 errno=EDOM;assert(ioctl(fd,0x802c550aUL,(void*)0xdeadbeef)==123 && errno==EDOM);
 errno=EDOM;assert(ioctl(fd,0x8004550fUL,(void*)0xdeadbeef)==0 && errno==EDOM);
 if(!strcmp(mode,"reopen")) {
  close(fd);fd=open("/dev/null",O_RDWR);assert(fd>=0);
  errno=EDOM;assert(ioctl(fd,0x802c550aUL,(void*)0xdeadbeef)==123 && errno==EDOM);
  errno=EDOM;assert(ioctl(fd,0x8004550fUL,(void*)0xdeadbeef)==0 && errno==EDOM);
 }
 submit(1,0);submit(2,3);
 if(!strcmp(mode,"cap"))for(unsigned i=0;i<80;i++) {errno=EDOM;assert(ioctl(fd,0x802c550aUL,h)==123 && errno==EDOM);}
 errno=EDOM;assert(ioctl(fd,0x80085504UL,(void*)0xdeadbeef)==0 && errno==EDOM);
 /* After ALT, never inspect an argument again, even an invalid one. */
 for(unsigned i=0;i<20;i++) {errno=EDOM;assert(ioctl(fd,0x802c550aUL,(void*)0xdeadbeef)==123 && errno==EDOM);}
 return NULL;
}
int main(int argc,char **argv) {
 mode=argc>1?argv[1]:"normal";setvbuf(stdout,NULL,_IOLBF,0);
 if(getenv("APCG_FIXTURE_PRIORITY")) {assert(!setpriority(PRIO_PROCESS,0,-20));errno=0;assert(getpriority(PRIO_PROCESS,0)==-20 && !errno);}
 volatile uint8_t *large=malloc(16U*1024*1024);assert(large);for(size_t i=0;i<16U*1024*1024;i+=4096)large[i]=7;
 void *arena=mmap((void*)0x80000000,110*4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(arena!=MAP_FAILED);
 for(unsigned i=0;i<55;i++) {void *p=(char*)arena+i*8192;assert(!mprotect(p,4096,PROT_READ|PROT_WRITE));*(volatile uint8_t*)p=3;}
 h=calloc(1,sizeof(*h));assert(h);h->type=2;h->context=0xdeadbabe;
 control=mmap((void*)0xbff00000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(control!=MAP_FAILED && (uintptr_t)control<UINT32_MAX-4096);
 fd=open("/dev/null",O_RDWR);wrong=open("/dev/zero",O_RDWR);assert(fd>=0 && wrong>=0);
 FILE *sm=fopen("/proc/self/smaps","r");assert(sm);size_t bytes=0,vmas=0;char line[512];
 while(fgets(line,sizeof(line),sm)) {bytes+=strlen(line);unsigned long a,b;if(sscanf(line,"%lx-%lx",&a,&b)==2)vmas++;}fclose(sm);
#if defined(__arm__) && __SIZEOF_POINTER__==4
 assert(vmas>=140 && vmas<=170 && bytes>=75*1024 && bytes<=105*1024);
#endif
 printf("LAYOUT vmas=%zu smaps_bytes=%zu resident_heap=16777216 anonymous_regions=55\n",vmas,bytes);
 if(!strcmp(mode,"main"))worker(NULL);
 else {pthread_t t;pthread_attr_t attr;assert(!pthread_attr_init(&attr));assert(!pthread_attr_setstacksize(&attr,!strcmp(mode,"small")?32768:131072));assert(!pthread_create(&t,&attr,worker,NULL));assert(!pthread_join(t,NULL));pthread_attr_destroy(&attr);}
 puts("PASS setup private forwarded only no USB/SDK");return 0;
}
