/* Bounded read-only ZT30 UART transport probe. No SDK loading or actuation.
 * Production accepts no options and always uses ttyS1 at230400. */
#include "camera_app/siyi.h"
#include "camera_app/private_uart.h"
#include <errno.h>
#include <fcntl.h>
#include <initializer_list>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifndef ZT30_CONTROL_DEVICE
#define ZT30_CONTROL_DEVICE "/dev/ttyS1"
#endif
static volatile sig_atomic_t stopping;
static void stop(int) { stopping=1; }
static uint64_t ms() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000; }
struct Probe {
    int fd=-1;
    struct termios previous{};
    bool configured=false;
    uint16_t sequence=0;
    uint8_t buffer[4096]{};
    size_t used=0;
    unsigned records=0;
    ~Probe() { if(fd>=0) { if(configured)tcsetattr(fd,TCSANOW,&previous); close(fd); } }
    bool log(const char *direction,const uint8_t *data,size_t n) {
        if(++records>1000)return false;
        printf("%s %llu ",direction,(unsigned long long)ms());
        for(size_t i=0;i<n;i++)printf("%02x",data[i]);
        putchar('\n');return !ferror(stdout);
    }
    bool send(uint8_t command,const uint8_t *payload,size_t length,uint8_t flags) {
        uint8_t frame[268]={0xaa,flags,2,(uint8_t)length,0};
        frame[4]=ca_crc8_maxim(frame,4);
        frame[5]=(uint8_t)sequence;frame[6]=(uint8_t)(sequence++>>8);
        frame[7]=0x2c;frame[8]=0x2e;frame[9]=0x6b;frame[10]=command;
        if(length)memcpy(frame+11,payload,length);
        uint16_t crc=ca_crc16(frame,11+length);frame[11+length]=crc;frame[12+length]=crc>>8;
        size_t sent=0,n=13+length;uint64_t deadline=ms()+200;
        while(sent<n && !stopping && ms()<deadline) {
            ssize_t count=write(fd,frame+sent,n-sent);
            if(count>0)sent+=(size_t)count;
            else if(count<0 && errno!=EAGAIN && errno!=EINTR)return false;
            else { struct pollfd p={fd,POLLOUT,0};poll(&p,1,10); }
        }
        return sent==n && log("TX",frame,n);
    }
    bool receive(uint8_t command,int sdk,uint8_t *answer,size_t &answer_size,uint64_t deadline) {
        while(!stopping && ms()<deadline) {
            size_t offset=0;
            while(used-offset>=5) {
                const uint8_t *f=buffer+offset;
                if(f[0]!=0xaa || f[2]!=2 || ca_crc8_maxim(f,4)!=f[4]) { offset++;continue; }
                size_t n=13U+f[3];if(used-offset<n)break;
                uint16_t crc=f[n-2]|uint16_t(f[n-1])<<8;
                if(ca_crc16(f,n-2)!=crc) { offset++;continue; }
                if(!log("RX",f,n))return false;
                bool match=f[7]==0x2e && f[8]==0x2c && f[9]==0x6b && f[10]==command && f[1]==0x0a;
                const uint8_t *payload=f+11;size_t length=f[3];
                if(match && sdk>=0) {
                    ca_siyi_packet packet{};size_t consumed=0;
                    match=ca_siyi_parse_one(payload,length,&packet,&consumed)==1 && consumed==length &&
                        (packet.control&2) && packet.opcode==sdk;
                    if(match) { payload=packet.payload;length=packet.payload_length; }
                }
                if(match) {
                    if(length>answer_size)return false;
                    memcpy(answer,payload,length);answer_size=length;
                    offset+=n;memmove(buffer,buffer+offset,used-offset);used-=offset;
                    return true;
                }
                offset+=n;
            }
            if(offset) { memmove(buffer,buffer+offset,used-offset);used-=offset; }
            if(used==sizeof(buffer))return false;
            struct pollfd p={fd,POLLIN,0};int ready=poll(&p,1,20);
            if(ready<0 && errno!=EINTR)return false;
            if(ready>0 && (p.revents&(POLLERR|POLLHUP|POLLNVAL)))return false;
            if(ready>0 && (p.revents&POLLIN)) {
                ssize_t n=read(fd,buffer+used,sizeof(buffer)-used);
                if(n>0)used+=(size_t)n;
                else if(n<0 && errno!=EAGAIN && errno!=EINTR)return false;
            }
        }
        return false;
    }
    bool query(uint8_t op,bool public_sdk) {
        uint8_t packet[16],answer[255];size_t payload_length=0;
        if(public_sdk) {
            payload_length=ca_siyi_build(packet,sizeof(packet),1,sequence,op,nullptr,0);
            if(payload_length!=CA_SIYI_MIN_PACKET)return false;
        }
        for(unsigned attempt=0;attempt<3 && !stopping;attempt++) {
            size_t length=sizeof(answer);
            if(!send(public_sdk?0x16:op,public_sdk?packet:nullptr,payload_length,public_sdk?8:9))return false;
            if(!receive(public_sdk?0x16:op,public_sdk?op:-1,answer,length,ms()+600))continue;
            size_t expected=0;
            switch(op) { case 1:expected=12;break;case 2:expected=12;break;case 0x0d:expected=12;break;case 0x18:expected=2;break;case 0x31:expected=1;break;case 0x15:case 0x17:expected=1;break;default:return false; }
            if(length!=expected)return false;
            printf("ANSWER %s %02x ",public_sdk?"SDK":"LINK",op);
            for(size_t i=0;i<length;i++)printf("%02x",answer[i]);
            putchar('\n');
            return !ferror(stdout);
        }
        return false;
    }
};
int main(int argc,char **)
{
    if(argc!=1)return 2;
    struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    sigaction(SIGTERM,&action,nullptr);sigaction(SIGINT,&action,nullptr);sigaction(SIGHUP,&action,nullptr);
    Probe probe;
    probe.fd=open(ZT30_CONTROL_DEVICE,O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
    if(probe.fd<0 || flock(probe.fd,LOCK_EX|LOCK_NB)<0 || tcgetattr(probe.fd,&probe.previous)<0) { perror("UART open/lock/termios");return 3; }
    printf("TERMIOS iflag=%lx oflag=%lx cflag=%lx lflag=%lx ispeed=%lu ospeed=%lu vmin=%u vtime=%u\n",
        (unsigned long)probe.previous.c_iflag,(unsigned long)probe.previous.c_oflag,
        (unsigned long)probe.previous.c_cflag,(unsigned long)probe.previous.c_lflag,
        (unsigned long)cfgetispeed(&probe.previous),(unsigned long)cfgetospeed(&probe.previous),
        probe.previous.c_cc[VMIN],probe.previous.c_cc[VTIME]);
    struct termios settings=probe.previous;cfmakeraw(&settings);
    cfsetispeed(&settings,B230400);cfsetospeed(&settings,B230400);
    settings.c_cflag|=CLOCAL|CREAD;settings.c_cflag&=~(CRTSCTS|CSTOPB|PARENB);
    if(tcsetattr(probe.fd,TCSANOW,&settings)<0)return 3;
    probe.configured=true;
    for(uint8_t op:{0x15,0x17})if(!probe.query(op,false))return 4;
    for(uint8_t op:{0x01,0x02,0x0d,0x18,0x31})if(!probe.query(op,true))return 4;
    uint64_t start=ms();unsigned samples=0;
    for(unsigned i=0;i<20 && !stopping;i++) {
        if(!probe.query(0x0d,true))return 4;
        samples++;
        uint64_t next=start+(i+1)*100;
        while(!stopping && ms()<next) { struct timespec delay={0,1000000};nanosleep(&delay,nullptr); }
    }
    if(stopping)return 5;
    printf("RESULT PASS attitude_samples=%u\n",samples);
    return ferror(stdout)?6:0;
}
