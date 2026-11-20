#include "usb_iso.h"
#include <cerrno>
#include <cstring>
#include <linux/usbdevice_fs.h>
#include <new>
#include <sys/ioctl.h>
namespace ca_zt30 {
static_assert(sizeof(usbdevfs_iso_packet_desc)==12,"fixed ISO packet descriptor ABI");
#if UINTPTR_MAX==UINT32_MAX
static_assert(sizeof(usbdevfs_urb)==44 && offsetof(usbdevfs_urb,buffer)==12 &&
    offsetof(usbdevfs_urb,number_of_packets)==28 && offsetof(usbdevfs_urb,usercontext)==40,"ARM32 URB ABI");
static_assert(USBDEVFS_SUBMITURB==0x802c550a && USBDEVFS_REAPURBNDELAY==0x4004550d,"ARM32 USB commands");
#endif
bool thermal_usb_descriptors(const uint8_t *desc,size_t bytes) {
    if(!desc || bytes<18 || bytes>4096 || desc[0]!=18 || desc[1]!=1 ||
       desc[8]!=0xda || desc[9]!=0x0b || desc[10]!=0x30 || desc[11]!=0x58)return false;
    bool endpoint=false;unsigned interface=999,alt=999;
    for(size_t at=18;at<bytes;) {
        if(bytes-at<2)return false;
        const unsigned length=desc[at];if(length<2 || length>bytes-at)return false;
        if(desc[at+1]==4) {
            if(length<9)return false;
            interface=desc[at+2];alt=desc[at+3];
        }
        if(desc[at+1]==5) {
            if(length<7)return false;
            if(interface==1 && alt==7 && desc[at+2]==0x81 &&
               (desc[at+3]&3)==1 && desc[at+4]==0 && desc[at+5]==0x14 && desc[at+6]==1)endpoint=true;
        }
        at+=length;
    }
    return endpoint;
}
static const uint8_t request[26]={
    0x01,0x00,0x01,0x02,0x2a,0x2c,0x0a,0x00,
    0,0,0,0,0,0,0,0,0x20,0,0,0,0x14,0,0,0x0c,0,0
};
static uint32_t le32(const uint8_t *p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
UsbIso::~UsbIso() { finish(); }
bool UsbIso::transfer(uint8_t type,uint8_t command,uint16_t selector,uint8_t *data) {
    usbdevfs_ctrltransfer c{};c.bRequestType=type;c.bRequest=command;c.wValue=selector;c.wIndex=1;c.wLength=26;c.timeout=1000;c.data=data;
    const int rc=api_.ioctl(api_.opaque,fd_,USBDEVFS_CONTROL,&c);
    if(rc==26)return true;
    if(rc>=0)errno=EPROTO;
    return false;
}
bool UsbIso::submit(Entry &e) {
    if(e.submitted) { errno=EBUSY;return false; }
    std::memset(e.urb,0,sizeof(usbdevfs_urb)+ThermalPackets*sizeof(usbdevfs_iso_packet_desc));
    e.urb->type=USBDEVFS_URB_TYPE_ISO;e.urb->endpoint=0x81;e.urb->flags=USBDEVFS_URB_ISO_ASAP;
    e.urb->buffer=e.buffer.get();e.urb->buffer_length=ThermalUrbBytes;e.urb->number_of_packets=ThermalPackets;e.urb->usercontext=&e;
    for(unsigned i=0;i<ThermalPackets;++i)e.urb->iso_frame_desc[i].length=ThermalPacketBytes;
    if(api_.ioctl(api_.opaque,fd_,USBDEVFS_SUBMITURB,e.urb)<0)return false;
    e.sequence=next_++;e.submitted=true;return true;
}
bool UsbIso::start() {
    if(started_ || finished_ || fd_<0 || !api_.ioctl || !api_.close || !api_.now || !api_.pause_ms) { errno=EINVAL;return false; }
    started_=true;
    for(auto &e:entries_) {
        e.storage.reset(new(std::nothrow) uint8_t[sizeof(usbdevfs_urb)+ThermalPackets*sizeof(usbdevfs_iso_packet_desc)]);
        e.buffer.reset(new(std::nothrow) uint8_t[ThermalUrbBytes]);
        if(!e.storage || !e.buffer) { errno=ENOMEM;return false; }
        e.urb=reinterpret_cast<usbdevfs_urb*>(e.storage.get());
    }
    for(unsigned i=0;i<2;++i) {
        unsigned number=i;
        if(api_.ioctl(api_.opaque,fd_,USBDEVFS_CLAIMINTERFACE,&number)<0)return false;
        claims_[i]=true;
    }
    if(!transfer(0xa1,0x83,0x100,maximum_))return false;
    std::memcpy(negotiated_,request,26);
    if(!transfer(0x21,1,0x100,negotiated_) || !transfer(0xa1,0x81,0x100,negotiated_))return false;
    const uint32_t payload=le32(negotiated_+22);
    if(negotiated_[2]!=1 || negotiated_[3]!=2 || le32(negotiated_+4)!=666666 ||
        le32(negotiated_+18)!=ThermalFrameBytes || !payload || payload>ThermalPacketBytes) { errno=EPROTO;return false; }
    if(!transfer(0x21,1,0x200,negotiated_))return false;
    usbdevfs_setinterface iface{};iface.interface=1;iface.altsetting=7;
    if(api_.ioctl(api_.opaque,fd_,USBDEVFS_SETINTERFACE,&iface)<0)return false;
    alt_=true;
    last_reap_ns_=api_.now(api_.opaque);
    for(auto &e:entries_)if(!submit(e))return false;
    active_=true;return true;
}
UsbIso::Entry *UsbIso::find(usbdevfs_urb *pointer) {
    if(!pointer)return nullptr;
    for(auto &e:entries_)if(e.urb==pointer)return &e;
    return nullptr;
}
unsigned UsbIso::pending() const { unsigned n=0;for(const auto &e:entries_)if(e.submitted)++n;return n; }
int UsbIso::step(Sink sink,void *opaque) {
    if(!active_ || !sink) { errno=EINVAL;return fail(); }
    usbdevfs_urb *p=nullptr;
    if(api_.ioctl(api_.opaque,fd_,USBDEVFS_REAPURBNDELAY,&p)<0)return errno==EAGAIN?0:fail();
    Entry *e=find(p);
    if(!e || !e->submitted) { errno=EPROTO;return fail(); }
    e->submitted=false;
    if(e->sequence!=reaped_++ || p->type!=USBDEVFS_URB_TYPE_ISO || p->endpoint!=0x81 || p->flags!=USBDEVFS_URB_ISO_ASAP ||
        p->buffer!=e->buffer.get() || p->buffer_length!=int(ThermalUrbBytes) || p->number_of_packets!=int(ThermalPackets) || p->usercontext!=e) { errno=EPROTO;return fail(); }
    if(p->status==-ENODEV || p->status==-ESHUTDOWN) { ++packet_errors_;errno=-p->status;return fail(); }
    uint32_t total=0;
    for(unsigned i=0;i<ThermalPackets;++i) {
        const auto &packet=p->iso_frame_desc[i];
        if(packet.length!=ThermalPacketBytes || packet.actual_length>ThermalPacketBytes) { errno=EPROTO;return fail(); }
        total+=packet.actual_length;
    }
    if(p->actual_length<0 || total!=uint32_t(p->actual_length)) { errno=EPROTO;return fail(); }
    const uint64_t now=api_.now(api_.opaque);
    if(!now) { errno=EIO;return fail(); }
    ++stats_.completions;
    if(!total)++stats_.empty_completions;
    stats_.bytes+=total;
    if(last_reap_ns_ && now>=last_reap_ns_ && now-last_reap_ns_>stats_.max_reap_gap_ns)
        stats_.max_reap_gap_ns=now-last_reap_ns_;
    last_reap_ns_=now;
    for(unsigned i=0;i<ThermalPackets;++i) {
        const auto &packet=p->iso_frame_desc[i];
        if(packet.actual_length)++stats_.data_packets;
        int status=p->status ? p->status : static_cast<int32_t>(packet.status);
        if(status)++packet_errors_;
        if(packet.actual_length || status) {
            IsoPacket event{e->sequence,now,i,status,e->buffer.get()+i*ThermalPacketBytes,packet.actual_length};
            if(!sink(opaque,event)) { errno=EIO;return fail(); }
        }
    }
    return submit(*e)?1:fail();
}
bool UsbIso::finish() {
    if(finished_)return cleanup_ok_;
    finished_=true;active_=false;
    if(fd_<0)return cleanup_ok_;
    if(!api_.ioctl || !api_.close || !api_.now || !api_.pause_ms) { cleanup_ok_=false;return false; }
    for(auto &e:entries_)if(e.submitted && api_.ioctl(api_.opaque,fd_,USBDEVFS_DISCARDURB,e.urb)<0 && errno!=EINVAL)cleanup_ok_=false;
    const uint64_t now=api_.now(api_.opaque);
    const uint64_t deadline=now+100000000;
    unsigned polls=0;
    while(pending() && now && polls++<200 && api_.now(api_.opaque)<deadline) {
        usbdevfs_urb *p=nullptr;
        const int rc=api_.ioctl(api_.opaque,fd_,USBDEVFS_REAPURBNDELAY,&p);
        if(rc<0) {
            if(errno==EAGAIN || errno==EINTR) { api_.pause_ms(api_.opaque,1);continue; }
            cleanup_ok_=false;break;
        }
        Entry *e=find(p);
        if(!e || !e->submitted) { cleanup_ok_=false;break; }
        e->submitted=false;
    }
    if(pending()) { fallback_=true;cleanup_ok_=false; }
    else {
        if(alt_) {
            usbdevfs_setinterface iface{};iface.interface=1;iface.altsetting=0;
            if(api_.ioctl(api_.opaque,fd_,USBDEVFS_SETINTERFACE,&iface)<0) {
                stats_.alt_zero=-errno;cleanup_ok_=false;
            }
        }
        for(int i=1;i>=0;--i)if(claims_[i]) {
            unsigned number=unsigned(i);
            if(api_.ioctl(api_.opaque,fd_,USBDEVFS_RELEASEINTERFACE,&number)<0) {
                (i?stats_.release_stream:stats_.release_control)=-errno;cleanup_ok_=false;
            }
        }
    }
    // Linux usbfs release synchronously kills pending URBs. Storage remains alive
    // throughout close, including the failure fallback. Never retry close(EINTR).
    if(api_.close(api_.opaque,fd_)<0)cleanup_ok_=false;
    fd_=-1;for(auto &e:entries_)e.submitted=false;
    return cleanup_ok_;
}
}
