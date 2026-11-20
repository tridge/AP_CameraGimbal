/* Bounded warm-state thermal capture. No SDK, XU, reset, driver detach or
 * persistent paths. Standalone RAM diagnostic, not native backend activation. */
#include "../camera_app/src/backends/zt30/usb_iso.h"
#include "../camera_app/src/backends/zt30/uvc_frame.h"
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
using namespace ca_zt30;
static volatile sig_atomic_t stopping;
static void stop(int) { stopping=1; }
static uint64_t now(void*) {
    timespec t{};if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec);
}
static void pause_ms(void*,unsigned n) {
    timespec t{time_t(n/1000),long(n%1000)*1000000};
    while(nanosleep(&t,&t)<0 && errno==EINTR) {}
}
static void hex(const char*,const uint8_t*);
static int usb_call(void*,int fd,unsigned long cmd,void *p) {
    if(stopping && (cmd==USBDEVFS_CONTROL || cmd==USBDEVFS_CLAIMINTERFACE || cmd==USBDEVFS_SUBMITURB ||
        (cmd==USBDEVFS_SETINTERFACE && static_cast<usbdevfs_setinterface*>(p)->altsetting!=0))) { errno=EINTR;return -1; }
    if(cmd==USBDEVFS_CONTROL) {
        const auto &c=*static_cast<usbdevfs_ctrltransfer*>(p);
        if(c.bRequestType==0x21 && c.wValue==0x200)hex("COMMIT_BEFORE",static_cast<uint8_t*>(c.data));
    }
    const int rc=ioctl(fd,cmd,p);
    if(cmd==USBDEVFS_CONTROL && rc==26) {
        const auto &c=*static_cast<usbdevfs_ctrltransfer*>(p);
        if(c.bRequestType==0xa1)hex(c.bRequest==0x83?"GET_MAX_AFTER":"GET_CUR_AFTER",static_cast<uint8_t*>(c.data));
    }
    return rc;
}
static int usb_close(void*,int fd) { return close(fd); }
static bool ram_directory(char (&directory)[160]) {
    char path[160]{};ssize_t n=readlink("/proc/self/exe",path,sizeof(path)-1);
    if(n<0 || n>=ssize_t(sizeof(path)-1))return false;
    const char prefix[]="/tmp/apcg-zt30.session.";
    if(std::strncmp(path,prefix,sizeof(prefix)-1))return false;
    char *tail=path+sizeof(prefix)-1;
    for(unsigned i=0;i<6;++i)if(!((tail[i]>='0' && tail[i]<='9') || (tail[i]>='A' && tail[i]<='Z') || (tail[i]>='a' && tail[i]<='z')))return false;
    if(std::strcmp(tail+6,"/usb-iso-probe"))return false;
    tail[6]=0;struct statfs fs{};
    if(statfs(path,&fs) || fs.f_type!=0x01021994 || uint64_t(fs.f_bavail)*fs.f_bsize<UINT64_C(64)*1024*1024)return false;
    std::strcpy(directory,path);return true;
}
static bool write_all(int fd,const void *data,size_t n) {
    auto *p=static_cast<const uint8_t*>(data);
    while(n) {
        if(stopping)return false;
        const ssize_t count=write(fd,p,n);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)return false;
        p+=count;n-=size_t(count);
    }
    return true;
}
struct File {
    int fd=-1;
    ~File() { if(fd>=0)close(fd); }
    bool open_at(int directory,const char *name) { fd=openat(directory,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);return fd>=0; }
};
struct PacketRecord {
    uint64_t submission,received_ns;
    uint32_t index;
    int32_t status;
    uint32_t bytes,reserved;
};
static_assert(sizeof(PacketRecord)==32,"fixed little-endian ISO record");
struct Capture {
    int raw,frames,metadata;
    size_t raw_bytes=8,frame_bytes=0;
    unsigned count=0;
    uint64_t packets=0;
    UvcFrame *assembler=nullptr;
    Capture(int r,int f,int m):raw(r),frames(f),metadata(m) {}
    static bool frame(void *v,const uint8_t *p,size_t n,const UvcFrameInfo &info) {
        auto &c=*static_cast<Capture*>(v);
        if(c.count>=8)return true;
        if(n!=ThermalFrameBytes || c.frame_bytes+n>8*size_t(ThermalFrameBytes) || !write_all(c.frames,p,n))return false;
        char line[256];const int bytes=std::snprintf(line,sizeof(line),"%u,%llu,%llu,%u,%u,%u,%u,%u,%u,%u\n",c.count,
            (unsigned long long)info.first_ns,(unsigned long long)info.last_ns,info.has_pts,info.pts,info.has_scr,
            info.scr_first,info.scr_last,info.sof_first,info.sof_last);
        if(bytes<=0 || size_t(bytes)>=sizeof(line) || !write_all(c.metadata,line,size_t(bytes)))return false;
        c.frame_bytes+=n;++c.count;return true;
    }
    static bool packet(void *v,const IsoPacket &p) {
        auto &c=*static_cast<Capture*>(v);
        PacketRecord header{p.submission,p.received_ns,p.index,p.status,p.bytes,0};
        const size_t n=sizeof(header)+p.bytes;
        if(c.raw_bytes+n>16*size_t(1024*1024) || !write_all(c.raw,&header,sizeof(header)) || !write_all(c.raw,p.data,p.bytes))return false;
        c.raw_bytes+=n;++c.packets;
        if(c.count>=8)return true;
        if(p.status)c.assembler->lost();
        else if(p.bytes && !c.assembler->packet(p.data,p.bytes,p.received_ns))return false;
        return true;
    }
};
static void hex(const char *name,const uint8_t *p) { std::printf("%s ",name);for(unsigned i=0;i<26;++i)std::printf("%02x",p[i]);std::puts(""); }
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    if(argc==1) { std::printf("LAYOUT ISO endpoint=81 alt=7 packets=32 packet_bytes=3072 ring=8 frame_bytes=1310720 no-USB-access\n");return 0; }
    if(argc!=2 || std::strcmp(argv[1],"--capture"))return 2;
    char directory[160]{};
    if(!ram_directory(directory)) { std::fputs("RAM qualification directory required\n",stderr);return 3; }
    const int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0)return 3;
    struct DirClose { int fd;~DirClose() { close(fd); } } dir_close{dir};
    const int marker=openat(dir,"owned-usb-preflight.ok",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(marker<0)return 3;
    struct stat m{};const bool marker_ok=fstat(marker,&m)==0 && S_ISREG(m.st_mode) && m.st_uid==0 && m.st_size>0 && m.st_size<4096;
    close(marker);if(!marker_ok)return 3;
    struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,nullptr) || sigaction(SIGINT,&action,nullptr) || sigaction(SIGHUP,&action,nullptr))return 4;
    File raw,frames,metadata;
    if(!raw.open_at(dir,"thermal-packets.bin") || !frames.open_at(dir,"thermal-frames.bin") || !metadata.open_at(dir,"thermal-frames.csv"))return 4;
    static const char columns[]="frame,first_ns,last_ns,has_pts,pts,has_scr,scr_first,scr_last,sof_first,sof_last\n";
    if(!write_all(raw.fd,"ZT30ISO1",8) || !write_all(metadata.fd,columns,sizeof(columns)-1))return 4;
    Capture capture{raw.fd,frames.fd,metadata.fd};UvcFrame assembler(ThermalFrameBytes,Capture::frame,&capture);capture.assembler=&assembler;
    if(!assembler.ready() || stopping)return 4;
    int fd=open("/dev/bus/usb/002/002",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)return 5;
    struct stat device{};
    if(fstat(fd,&device) || !S_ISCHR(device.st_mode) || major(device.st_rdev)!=189 || minor(device.st_rdev)!=129) { close(fd);return 5; }
    uint32_t capabilities=0;
    if(ioctl(fd,USBDEVFS_GET_CAPABILITIES,&capabilities)<0) { close(fd);return 5; }
    std::printf("CAPABILITIES %08x\n",capabilities);
    UsbIsoApi api{nullptr,usb_call,usb_close,now,pause_ms};UsbIso usb(fd,api);
    const uint64_t start=now(nullptr);
    if(!start || !usb.start()) {
        const int error=errno;const bool clean=usb.finish();
        std::printf("RESULT FAIL startup errno=%d cleanup=%u close_fallback=%u\n",error,clean,usb.close_fallback());return 6;
    }
    hex("GET_MAX",usb.get_max());hex("GET_CUR_COMMIT",usb.negotiated());
    const uint64_t capture_start=now(nullptr),deadline=capture_start+4000000000;
    bool ok=capture_start!=0;unsigned reaped=0;int error=0;
    while(ok && !stopping && capture.count<8 && now(nullptr)<deadline) {
        const int rc=usb.step(Capture::packet,&capture);
        if(rc<0) { error=errno;ok=false; }
        else if(rc==0)pause_ms(nullptr,1);
        else ++reaped;
    }
    const bool cleaned=usb.finish();
    ok=ok && !stopping && capture.count==8 && !usb.packet_errors() && !assembler.malformed() && !assembler.dropped() && !assembler.pts_changes() && cleaned;
    std::printf("RESULT %s frames=%u bytes=%zu packets=%llu urbs=%u packet_errors=%llu dropped=%llu malformed=%llu pts_changes=%llu cleanup=%u close_fallback=%u errno=%d elapsed_ns=%llu\n",
        ok?"PASS":"FAIL",capture.count,capture.frame_bytes,(unsigned long long)capture.packets,reaped,
        (unsigned long long)usb.packet_errors(),(unsigned long long)assembler.dropped(),(unsigned long long)assembler.malformed(),
        (unsigned long long)assembler.pts_changes(),cleaned,usb.close_fallback(),error,(unsigned long long)(now(nullptr)-start));
    return ok?0:7;
}
