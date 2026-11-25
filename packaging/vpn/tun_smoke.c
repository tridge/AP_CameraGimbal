#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
static void die(const char *s) { perror(s);exit(1); }
static void check(int ok,const char *s) { if(!ok) { fprintf(stderr,"FAIL: %s\n",s);exit(1); } }
static uint16_t sum(const unsigned char *p,size_t n) {
 uint32_t s=0;for(;n>1;p+=2,n-=2)s+=((unsigned)p[0]<<8)|p[1];if(n)s+=(unsigned)p[0]<<8;
 while(s>>16)s=(s&65535)+(s>>16);return (uint16_t)~s;
}
static void put16(unsigned char *p,unsigned v) { p[0]=v>>8;p[1]=v; }
static void addr_ioctl(int s,struct ifreq *r,unsigned op,const char *ip) {
 struct sockaddr_in *a=(struct sockaddr_in*)&r->ifr_addr;memset(a,0,sizeof(*a));a->sin_family=AF_INET;
 check(inet_pton(AF_INET,ip,&a->sin_addr)==1,"inet_pton");if(ioctl(s,op,r))die("address ioctl");
}
static ssize_t receive(int fd,unsigned char *p,size_t n) {
 struct pollfd pollfd={fd,POLLIN,0};int rc=poll(&pollfd,1,2000);
 if(rc<=0) { if(!rc)errno=ETIMEDOUT;die("poll tun reply"); }
 ssize_t got=read(fd,p,n);if(got<0)die("read tun reply");return got;
}
static void exercise(const char *node,int tap) {
 const unsigned char peer[6]={2,0,0,0,0,2};unsigned char mac[6]={};
 uint32_t local,remote;const char *local_ip=tap?"198.19.0.1":"198.18.0.1";
 const char *remote_ip=tap?"198.19.0.2":"198.18.0.2";
 inet_pton(AF_INET,local_ip,&local);inet_pton(AF_INET,remote_ip,&remote);
 printf("BEGIN %s\n",tap?"TAP":"TUN");fflush(stdout);
 int fd=open(node,O_RDWR|O_NONBLOCK);if(fd<0)die("open tun");unsigned features=0;
 if(ioctl(fd,TUNGETFEATURES,&features))die("TUNGETFEATURES");
 struct ifreq r={};snprintf(r.ifr_name,IFNAMSIZ,"apcgtest%d",tap);r.ifr_flags=(tap?IFF_TAP:IFF_TUN)|IFF_NO_PI|IFF_MULTI_QUEUE;
 if(ioctl(fd,TUNSETIFF,&r))die("TUNSETIFF");printf("CREATED %s\n",r.ifr_name);fflush(stdout);

 struct ifreq observed={};if(ioctl(fd,TUNGETIFF,&observed))die("TUNGETIFF");
 check(!strcmp(observed.ifr_name,r.ifr_name) && (observed.ifr_flags&IFF_MULTI_QUEUE),"get interface flags");
 if(ioctl(fd,TUNSETOWNER,1000) || ioctl(fd,TUNSETGROUP,0))die("set owner/group");
 for(unsigned uid=1001;uid>=1000;uid--) {
  pid_t child=fork();if(child<0)die("fork");
  if(!child) {
   int extra=open(node,O_RDWR|O_NONBLOCK);if(extra<0)die("open second queue");
   if(setresuid(uid,uid,uid))die("drop uid");
   struct ifreq q={};strcpy(q.ifr_name,r.ifr_name);q.ifr_flags=(tap?IFF_TAP:IFF_TUN)|IFF_NO_PI|IFF_MULTI_QUEUE;
   int rc=ioctl(extra,TUNSETIFF,&q);
   if(uid==1001)check(rc<0 && errno==EPERM,"wrong owner denied");
   else check(rc==0,"matching owner attaches");
   close(extra);_exit(0);
  }
  int status=0;check(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status),"owner child");
 }
 signal(SIGIO,SIG_IGN);int fl=fcntl(fd,F_GETFL);if(fl<0 || fcntl(fd,F_SETFL,fl|O_ASYNC))die("enable async");
 check(fcntl(fd,F_GETOWN)==getpid(),"async notification owner");
 if(fcntl(fd,F_SETFL,fl))die("disable async");
 printf("PASS owner permissions, multi-queue attach/close, TUNGETIFF and fasync\n");fflush(stdout);
 int s=socket(AF_INET,SOCK_DGRAM,0);if(s<0)die("socket");
 addr_ioctl(s,&r,SIOCSIFADDR,local_ip);addr_ioctl(s,&r,SIOCSIFNETMASK,"255.255.255.252");
 r.ifr_mtu=2800;if(ioctl(s,SIOCSIFMTU,&r))die("SIOCSIFMTU");
 if(tap) { if(ioctl(s,SIOCGIFHWADDR,&r))die("SIOCGIFHWADDR");memcpy(mac,r.ifr_hwaddr.sa_data,6); }
 if(ioctl(s,SIOCGIFFLAGS,&r))die("SIOCGIFFLAGS");r.ifr_flags|=IFF_UP;
 if(ioctl(s,SIOCSIFFLAGS,&r))die("SIOCSIFFLAGS");close(s);
 unsigned char packet[4096]={},reply[4096];
 if(tap) {
  memset(packet,255,6);memcpy(packet+6,peer,6);put16(packet+12,0x806);
  put16(packet+14,1);put16(packet+16,0x800);packet[18]=6;packet[19]=4;put16(packet+20,1);
  memcpy(packet+22,peer,6);memcpy(packet+28,&remote,4);memcpy(packet+38,&local,4);
  check(write(fd,packet,42)==42,"write ARP");ssize_t n=receive(fd,reply,sizeof(reply));
  check(n>=42 && reply[12]==8 && reply[13]==6 && reply[20]==0 && reply[21]==2,"ARP reply");
  check(!memcmp(reply,peer,6) && !memcmp(reply+28,&local,4) && !memcmp(reply+38,&remote,4),"ARP addressing");
 }
 const unsigned lengths[]={32,1400,2700};
 for(unsigned size=0;size<3;size++)for(unsigned seq=0;seq<100;seq++) {
  const unsigned payload=lengths[size],off=tap?14:0,bytes=off+28+payload;
  memset(packet,0,bytes);
  if(tap) { memcpy(packet,mac,6);memcpy(packet+6,peer,6);put16(packet+12,0x800); }
  unsigned char *ip=packet+off,*icmp=ip+20;ip[0]=0x45;put16(ip+2,28+payload);put16(ip+4,seq);
  ip[8]=64;ip[9]=1;memcpy(ip+12,&remote,4);memcpy(ip+16,&local,4);put16(ip+10,sum(ip,20));
  icmp[0]=8;put16(icmp+4,0x6b03);put16(icmp+6,seq);
  for(unsigned i=0;i<payload;i++)icmp[8+i]=(unsigned char)(i+seq);
  put16(icmp+2,sum(icmp,payload+8));check(write(fd,packet,bytes)==(ssize_t)bytes,"write ICMP request");
  ssize_t n=receive(fd,reply,sizeof(reply));check(n==(ssize_t)bytes,"ICMP response size");
  const unsigned char *ri=reply+off,*rc=ri+20;
  check(ri[0]==0x45 && ri[9]==1 && !memcmp(ri+12,&local,4) && !memcmp(ri+16,&remote,4),"IPv4 reply");
  check(rc[0]==0 && rc[4]==0x6b && rc[5]==3 && rc[6]==0 && rc[7]==seq,"ICMP reply");
  check(!memcmp(rc+8,icmp+8,payload) && sum(ri,20)==0 && sum(rc,payload+8)==0,"payload/checksum");
 }
 printf("PASS %s: %s features=0x%x, 300 IPv4/ICMP round trips, payloads 32/1400/2700 bytes%s\n",
  tap?"TAP":"TUN",r.ifr_name,features,tap?", ARP round trip":"");fflush(stdout);close(fd);
}
int main(int argc,char **argv) {
 check(argc==2,"device argument");if(unshare(CLONE_NEWNET))die("unshare CLONE_NEWNET");
 /* This probe exercises IPv4; suppress unrelated IPv6 router solicitations
  * only inside the newly created, private network namespace. */
 FILE *ipv6=fopen("/proc/sys/net/ipv6/conf/default/disable_ipv6","w");
 if(ipv6) { check(fputs("1\n",ipv6)>=0 && fclose(ipv6)==0,"disable probe IPv6"); }
 exercise(argv[1],0);exercise(argv[1],1);puts("PASS private network namespace; interfaces removed on exit");return 0;
}
