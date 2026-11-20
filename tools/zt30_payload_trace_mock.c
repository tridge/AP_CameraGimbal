/* Fake ioctl plus explicit read/write fault injection, never a driver ioctl. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
static long (*real_syscall)(long,...);static const char *mode;static unsigned nested;
__attribute__((constructor)) static void begin(void) {real_syscall=dlsym(RTLD_NEXT,"syscall");mode=getenv("FIXTURE_MODE");if(!mode)mode="normal";}
long mock_syscall(long number,uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t a4,uintptr_t a5,uintptr_t a6) __asm__("syscall");
long mock_syscall(long number,uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t a4,uintptr_t a5,uintptr_t a6) {
 if(!real_syscall) {_exit(90);}
 if(number==SYS_pwrite64 && a3==352) {
  if(!strcmp(mode,"interrupt-write")) {errno=EINTR;return -1;}
  if(!strcmp(mode,"short-write"))a3/=2;
 }
 if(number==SYS_pread64 && a3!=16) {
  if(!strcmp(mode,"fault-read")) {errno=EIO;return -1;}
  if(!strcmp(mode,"short-read")) {long r=real_syscall(number,a1,a2,a3,a4,a5,a6);return r>0?r-1:r;}
 }
 return real_syscall(number,a1,a2,a3,a4,a5,a6);
}
int mock_ioctl(int fd,unsigned long command,uintptr_t argument) __asm__("ioctl");
int mock_ioctl(int fd,unsigned long command,uintptr_t argument) {
 (void)fd;
 if(command==0x40086908U && !strcmp(mode,"nested") && __sync_bool_compare_and_swap(&nested,0,1))raise(SIGUSR2);
 if(command==0xc0086908U && argument!=1) {
  uint32_t *d=(uint32_t*)argument;uintptr_t p=(uintptr_t)d[2];
#if !defined(__arm__)
  p|=(uintptr_t)d[3]<<32;
#endif
  ((uint32_t*)p)[1]=9;
 }
 errno=EBUSY;return command==0xc0086908U?0:123;
}
