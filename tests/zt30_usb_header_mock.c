#define _GNU_SOURCE
#include <stdint.h>
#include <errno.h>
int fake_ioctl(int fd,unsigned long request,uintptr_t argument) __asm__("ioctl");
int fake_ioctl(int fd,unsigned long request,uintptr_t argument) {
 (void)fd;(void)argument;
 if(request==0x80085504UL)return errno==EAGAIN?-1:0;
 return 123;
}
