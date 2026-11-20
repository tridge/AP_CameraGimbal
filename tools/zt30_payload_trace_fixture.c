/* Disposable no-SDK process. All ioctl calls terminate in the test mock. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int sysfd,snrfd;static volatile sig_atomic_t signal_bad;static const char *mode;
static void descriptor(uint32_t desc[4],void *data,unsigned length) {
 uintptr_t p=(uintptr_t)data;desc[0]=length;desc[1]=0xabcdef01;desc[2]=(uint32_t)p;
#if defined(__arm__)
 desc[3]=desc[2]&0x80000000U?UINT32_MAX:0;
#else
 desc[3]=(uint32_t)(p>>32);
#endif
}
static int checked(int fd,unsigned long command,void *argument,int expected) {
 errno=EINTR;int rc=ioctl(fd,command,argument);
 return rc==expected && errno==EBUSY?0:1;
}
static void handler(int sig) {
 int saved=errno;uint32_t p[12]={17,91},d[4];descriptor(d,p,sig==SIGUSR2?48:8);
 if(checked(sysfd,sig==SIGUSR2?0x40306903U:0x6966U,d,123))signal_bad=1;
 errno=saved;
}
static void *worker(void *argument) {
 unsigned i=(unsigned)(uintptr_t)argument;static const unsigned commands[]={0x40086908U,0x40086909U,0x40186915U,0x40306902U};
 static const unsigned lengths[]={8,8,24,48};uint32_t p[24],d[4];
 for(unsigned j=0;j<24;j++)p[j]=0x12340000U+j;
 descriptor(d,p,lengths[i]);
 for(unsigned j=0;j<300;j++)if(checked(sysfd,commands[i],d,123))return (void*)1;
 return NULL;
}
int main(int argc,char **argv) {
 (void)argv;if(argc!=1)return 2;struct timespec all_start,all_end;clock_gettime(CLOCK_MONOTONIC,&all_start);mode=getenv("FIXTURE_MODE");if(!mode)mode="normal";
 sysfd=open("/dev/null",O_RDWR);snrfd=open("/dev/zero",O_RDWR);if(sysfd<0||snrfd<0)return 3;
 int a=open64("/dev/null",O_RDONLY),b=openat(AT_FDCWD,"/dev/null",O_RDONLY);if(a<0||b<0)return 4;close(a);close(b);
 struct sigaction sa={0};sa.sa_handler=handler;sigemptyset(&sa.sa_mask);if(sigaction(SIGUSR1,&sa,NULL)||sigaction(SIGUSR2,&sa,NULL))return 5;
 struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
 for(unsigned i=0;i<10000;i++)if(checked(sysfd,0x6988U,(void*)(uintptr_t)0xdeadbeefU,123))return 6;
 clock_gettime(CLOCK_MONOTONIC,&end);
 uint64_t bench=(uint64_t)(end.tv_sec-start.tv_sec)*1000000000ULL+(uint64_t)end.tv_nsec-start.tv_nsec;
 if(strstr(mode,"reuse")) {
  const char *path=getenv("FIXTURE_SACRIFICIAL_FILE");
  if(!path || strncmp(path,"/tmp/apcg-zt30.session.",strlen("/tmp/apcg-zt30.session.")) || !strstr(path,"/unrelated.bin"))return 15;
  int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);if(fd<0)return 16;
  if(write(fd,"unrelated contents",18)!=18)return 17;
  int target=!strcmp(mode,"metadata-reuse")?512:!strcmp(mode,"counts-reuse")?513:!strcmp(mode,"writer-reuse")?514:515;
  if(dup2(fd,target)!=target)return 18;
  close(fd);
 }
 uint32_t params[2]={1,6},desc[4];descriptor(desc,params,8);
 for(unsigned i=0;i<40;i++) {
  if(checked(snrfd,0xc0086908U,!strcmp(mode,"badptr")?(void*)1:desc,0))return 7;
  if(strcmp(mode,"badptr") && params[1]!=9)return 8;
 }
 pthread_t threads[4];for(unsigned i=0;i<4;i++)if(pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i))return 9;
 for(unsigned i=0;i<50;i++)raise(SIGUSR1);
 for(unsigned i=0;i<4;i++) {void *value;if(pthread_join(threads[i],&value)||value)return 10;}
 if(signal_bad)return 11;
 errno=EINTR;if(ioctl(sysfd,0x69ffU)!=123||errno!=EBUSY)return 12;
 if(!strcmp(mode,"abrupt"))_exit(0);
 pid_t child=fork();if(child<0)return 13;
 if(!child)_exit(checked(sysfd,0x40086908U,desc,123));
 int status;if(waitpid(child,&status,0)!=child || !WIFEXITED(status)||WEXITSTATUS(status))return 14;
 clock_gettime(CLOCK_MONOTONIC,&all_end);uint64_t total=(uint64_t)(all_end.tv_sec-all_start.tv_sec)*1000000000ULL+(uint64_t)all_end.tv_nsec-all_start.tv_nsec;
 printf("PASS child_pid=%ld pid=%ld bench_ns=%llu total_ns=%llu\n",(long)child,(long)getpid(),(unsigned long long)bench,(unsigned long long)total);
 close(sysfd);close(snrfd);return 0;
}
