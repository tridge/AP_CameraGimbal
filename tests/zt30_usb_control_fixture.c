/* Independent private control-copy fixture, no vendor preload/device access. */
#define _GNU_SOURCE
#include "zt30_usb_control_snapshot.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
static struct zt30_self_reader reader;
static void setup(uint8_t *b,unsigned length,unsigned selector) {
 memset(b,0,42);b[0]=0x21;b[1]=1;b[3]=(uint8_t)selector;b[4]=1;b[6]=(uint8_t)length;
 for(unsigned i=0;i<length;i++)b[8+i]=(uint8_t)(i+11);
}
static uint64_t cpu_now(void) {struct timespec t;assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;}
static void run(struct zt30_usb_urb_header *header,struct zt30_usb_control_copy *out) {
 uint64_t c0=cpu_now(),start=zt30_guard_now();errno=EDOM;zt30_usb_control_snapshot(&reader,header,start+50000000ULL,out);assert(errno==EDOM);
 uint64_t elapsed=zt30_guard_now()-start,cost=cpu_now()-c0;
 if(out->data_length)printf("CONTROL_COPY length=%u wall_ns=%llu cpu_ns=%llu\n",out->data_length,(unsigned long long)elapsed,(unsigned long long)cost);
}
int main(void) {
 setvbuf(stdout,NULL,_IOLBF,0);assert(!zt30_usb_reader_init(&reader));
 uint8_t *b=mmap((void*)0x90000000,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(b!=MAP_FAILED && (uintptr_t)b<=UINT32_MAX-8192);
 struct zt30_usb_urb_header h={.type=2,.endpoint=0,.buffer=(uint32_t)(uintptr_t)b,.buffer_length=42};struct zt30_usb_control_copy out;
 for(unsigned offset=0;offset<4;offset++)for(unsigned length=26;length<=34;length+=8)for(unsigned selector=1;selector<=2;selector++) {
  setup(b+offset,length,selector);h.buffer=(uint32_t)(uintptr_t)(b+offset);h.buffer_length=8+(int)length;run(&h,&out);
  assert(!out.error && out.setup_valid && out.data_length==length && !memcmp(out.data,b+offset+8,length));
 }
 puts("ALIGNMENT PASS offsets0..3 lengths26/34 probe/commit");
 h.buffer=(uint32_t)(uintptr_t)b;h.buffer_length=42;
 for(unsigned field=0;field<8;field++) {
  setup(b,34,1);
  switch(field) {case 0:b[0]=0xa1;b[1]=0x81;break;case 1:b[0]=0x41;break;case 2:b[1]=2;break;case 3:b[3]=3;break;case 4:b[4]=2;break;case 5:b[6]=26;break;case 6:b[6]=255;break;case 7:b[2]=1;break;}
  run(&h,&out);assert(!out.error && out.setup_valid && !out.data_length);for(unsigned i=0;i<34;i++)assert(!out.data[i]);
 }
 puts("ALLOWLIST PASS no IN/extension/mismatch data");
 for(unsigned field=0;field<5;field++) {
  struct zt30_usb_urb_header x=h;
  switch(field) {case 0:x.type=0;break;case 1:x.endpoint=0x81;break;case 2:x.buffer_length=263;break;case 3:x.buffer_length=7;break;case 4:x.packets=1;break;}
  x.buffer=0xdeadbeef;run(&x,&out);assert(!out.error && !out.setup_valid && !out.data_length);
 }
 puts("PREFILTER PASS opaque invalid nested pointers untouched");
 setup(b,34,2);assert(!mprotect(b,4096,PROT_NONE));run(&h,&out);assert(out.error && !out.setup_valid && !out.data_length);assert(!mprotect(b,4096,PROT_READ|PROT_WRITE));
 h.buffer=(uint32_t)(uintptr_t)(b+4096-16);assert(!mprotect(b+4096,4096,PROT_NONE));run(&h,&out);assert(out.error && !out.setup_valid);assert(!mprotect(b+4096,4096,PROT_READ|PROT_WRITE));
 h.buffer=0xdeadbeef;run(&h,&out);assert(out.error && !out.setup_valid);
 uint8_t *shared=mmap((void*)0x91000000,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);assert(shared!=MAP_FAILED && (uintptr_t)shared<=UINT32_MAX);h.buffer=(uint32_t)(uintptr_t)shared;run(&h,&out);assert(out.error && !out.setup_valid);
 char name[]="/tmp/zt30-control-fixture.XXXXXX";int fd=mkstemp(name);assert(fd>=0);assert(!unlink(name));assert(!ftruncate(fd,4096));
 uint8_t *file=mmap((void*)0x92000000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);assert(file!=MAP_FAILED && (uintptr_t)file<=UINT32_MAX);close(fd);
 h.buffer=(uint32_t)(uintptr_t)file;run(&h,&out);assert(out.error && !out.setup_valid);assert(!munmap(file,4096));
 puts("GUARDS PASS PROT_NONE boundary invalid shared file");
 h.buffer=(uint32_t)(uintptr_t)b;setup(b,34,1);errno=EDOM;zt30_usb_control_snapshot(&reader,&h,zt30_guard_now()-1,&out);assert(out.error==ETIMEDOUT && errno==EDOM && !out.setup_valid);
 zt30_usb_control_busy=1;run(&h,&out);assert(out.error==EDEADLK);zt30_usb_control_busy=0;
 pid_t child=fork();assert(child>=0);if(!child) {run(&h,&out);assert(out.error && !out.setup_valid);_exit(0);}int status;assert(waitpid(child,&status,0)==child && status==0);
 puts("LIFETIME PASS deadline reentry fork errno");
 int replacement=open("/dev/null",O_RDONLY);assert(replacement>=0);assert(dup2(replacement,reader.fd)==reader.fd);close(replacement);
 run(&h,&out);assert(out.error && !out.setup_valid && !out.data_length);int oldfd=reader.fd;zt30_usb_reader_close(&reader);assert(fcntl(oldfd,F_GETFD)>=0);close(oldfd);puts("READER_REUSE PASS pinned descriptor refused");assert(!munmap(b,8192));assert(!munmap(shared,4096));puts("PASS private UVC OUT metadata only no USB/SDK");return 0;
}
