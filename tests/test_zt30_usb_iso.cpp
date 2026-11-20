#ifdef NDEBUG
#error ISO qualification requires assertions and their side effects
#endif
#include "../camera_app/src/backends/zt30/usb_iso.h"
#include "../camera_app/src/backends/zt30/uvc_frame.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
using namespace ca_zt30;
struct Fake {
    std::deque<usbdevfs_urb*> pending;
    bool claimed[2]{},alt=false,closed=false,block=false,unknown=false,short_control=false,mismatch=false;
    unsigned count=0,fail_at=0,discarded=0,steps=0,controls=0,completion_mode=0;
    uint64_t clock=1000000000;
    uint8_t data[26]={1,0,1,2,0x2a,0x2c,0x0a,0,0,0,0,0,0,0,0,0,0x20,0,0,0,0x14,0,0,0xc,0,0};
    static int call(void *v,int fd,unsigned long cmd,void *arg) {
        auto &f=*static_cast<Fake*>(v);assert(fd==41 && !f.closed);
        if(++f.count==f.fail_at) { errno=EIO;return -1; }
        switch(cmd) {
        case USBDEVFS_CLAIMINTERFACE: {
            unsigned i=*static_cast<unsigned*>(arg);assert(i<2 && !f.claimed[i]);f.claimed[i]=true;return 0;
        }
        case USBDEVFS_RELEASEINTERFACE: {
            unsigned i=*static_cast<unsigned*>(arg);assert(i<2 && f.claimed[i] && f.pending.empty());f.claimed[i]=false;return 0;
        }
        case USBDEVFS_CONTROL: {
            auto &c=*static_cast<usbdevfs_ctrltransfer*>(arg);
            assert(f.claimed[1] && c.wIndex==1 && c.wLength==26 && c.timeout==1000);
            assert((c.bRequestType==0xa1 && c.wValue==0x100 && (c.bRequest==0x81 || c.bRequest==0x83)) ||
                (c.bRequestType==0x21 && c.bRequest==1 && (c.wValue==0x100 || c.wValue==0x200)));
            ++f.controls;
            assert(f.controls<=4);
            const unsigned types[]={0xa1,0x21,0xa1,0x21},commands[]={0x83,1,0x81,1},values[]={0x100,0x100,0x100,0x200};
            assert(c.bRequestType==types[f.controls-1] && c.bRequest==commands[f.controls-1] && c.wValue==values[f.controls-1]);
            if(f.controls==2)assert(std::memcmp(c.data,f.data,26)==0);
            if(f.controls==4)assert(static_cast<uint8_t*>(c.data)[8]==99);
            if(f.short_control)return 12;
            if(c.bRequestType==0xa1) {
                std::memcpy(c.data,f.data,26);
                static_cast<uint8_t*>(c.data)[25]=f.mismatch?1:0;
                if(c.bRequest==0x81)static_cast<uint8_t*>(c.data)[8]=99; // Informational fill is allowed.
            }
            return 26;
        }
        case USBDEVFS_SETINTERFACE: {
            auto &c=*static_cast<usbdevfs_setinterface*>(arg);assert(c.interface==1 && f.claimed[1]);
            assert(c.altsetting==0 || c.altsetting==7);if(c.altsetting==0)assert(f.pending.empty());f.alt=c.altsetting==7;return 0;
        }
        case USBDEVFS_SUBMITURB: {
            auto *u=static_cast<usbdevfs_urb*>(arg);assert(f.alt && f.claimed[1]);
            assert(u->type==USBDEVFS_URB_TYPE_ISO && u->endpoint==0x81 && u->flags==USBDEVFS_URB_ISO_ASAP);
            assert(u->number_of_packets==32 && u->buffer_length==98304 && u->buffer && u->usercontext);
            for(auto *old:f.pending)assert(old!=u);
            for(unsigned i=0;i<32;++i)assert(u->iso_frame_desc[i].length==3072);
            f.pending.push_back(u);return 0;
        }
        case USBDEVFS_DISCARDURB:
            ++f.discarded;errno=EINVAL;return -1; // Already complete; caller must still reap.
        case USBDEVFS_REAPURBNDELAY: {
            if(f.completion_mode==7) { errno=ENODEV;return -1; }
            if(f.unknown) { f.unknown=false;*static_cast<usbdevfs_urb**>(arg)=reinterpret_cast<usbdevfs_urb*>(1);return 0; }
            if(f.block || f.pending.empty()) { errno=EAGAIN;return -1; }
            auto *u=f.pending.front();f.pending.pop_front();
            u->actual_length=8;
            for(unsigned i=0;i<2;++i) {
                u->iso_frame_desc[i].actual_length=4;
                auto *p=static_cast<uint8_t*>(u->buffer)+i*3072;p[0]=2;p[1]=i?2:0;p[2]=uint8_t(i+1);p[3]=uint8_t(i+2);
            }
            switch(f.completion_mode) {
            case 1:u->number_of_packets=33;break;
            case 2:u->buffer=reinterpret_cast<void*>(1);break;
            case 3:u->iso_frame_desc[0].actual_length=3073;break;
            case 4:u->actual_length=7;break;
            case 5:u->status=-EXDEV;break;
            case 6:u->iso_frame_desc[0].status=unsigned(-EPROTO);break;
            case 8:u->status=-ENODEV;break;
            case 9:u->status=-ESHUTDOWN;break;
            case 10:u->actual_length=0;for(unsigned i=0;i<2;i++)u->iso_frame_desc[i].actual_length=0;break;
            }
            *static_cast<usbdevfs_urb**>(arg)=u;++f.steps;return 0;
        }
        default: assert(false && "unspecified or forbidden ioctl");return -1;
        }
    }
    static int close(void *v,int fd) {
        auto &f=*static_cast<Fake*>(v);assert(fd==41 && !f.closed);
        // Kernel ownership remains valid through close, even if cancellation drain timed out.
        for(auto *u:f.pending) { assert(u->buffer && u->number_of_packets==32);static_cast<uint8_t*>(u->buffer)[0]=42; }
        f.pending.clear();f.claimed[0]=f.claimed[1]=f.alt=false;f.closed=true;return 0;
    }
    static uint64_t now(void *v) { return static_cast<Fake*>(v)->clock; }
    static void pause(void *v,unsigned n) { static_cast<Fake*>(v)->clock+=uint64_t(n)*1000000; }
    UsbIsoApi api() { return {this,call,close,now,pause}; }
};
struct Seen {
    unsigned packets=0;
    static bool sink(void *v,const IsoPacket &p) {
        auto &s=*static_cast<Seen*>(v);assert(p.index<2 && p.bytes==4 && p.status==0 && p.submission>0);
        assert(p.data[2]==p.index+1);++s.packets;return true;
    }
    static bool reject(void*,const IsoPacket&) { return false; }
    static bool errors(void *v,const IsoPacket &p) { if(p.status)++*static_cast<unsigned*>(v);return true; }
};
struct Frames {
    std::vector<std::vector<uint8_t>> data;
    std::vector<UvcFrameInfo> infos;
    static bool sink(void *v,const uint8_t *p,size_t n,const UvcFrameInfo &info) {
        assert(info.first_ns>0 && info.last_ns>=info.first_ns);
        auto &f=*static_cast<Frames*>(v);f.data.emplace_back(p,p+n);f.infos.push_back(info);return true;
    }
    static bool reject(void*,const uint8_t*,size_t,const UvcFrameInfo&) { return false; }
};
static void assembly() {
    Frames f;UvcFrame a(4,Frames::sink,&f);assert(a.ready());
    uint8_t first[]={2,2,7,7,7,7};assert(a.packet(first,sizeof(first),1));assert(f.data.empty());
    uint8_t x[]={2,1,1,2},y[]={2,3,3,4};assert(a.packet(x,sizeof(x),2));assert(a.packet(y,sizeof(y),3));
    assert(f.data.size()==1 && f.data[0]==std::vector<uint8_t>({1,2,3,4}));
    assert(a.packet(y,sizeof(y),4));assert(f.data.size()==1); // Same-FID bytes after EOF are ignored.
    uint8_t no_eof[]={2,0,5,6,7,8};assert(a.packet(no_eof,sizeof(no_eof),5));
    assert(a.packet(x,sizeof(x),6));assert(f.data.size()==2 && f.data[1][0]==5); // Size at FID toggle.
    a.lost();assert(a.packet(y,sizeof(y),7));assert(f.data.size()==2);
    assert(a.packet(no_eof,sizeof(no_eof),8));assert(a.packet(x,sizeof(x),9));assert(f.data.size()==3);
    uint8_t invalid[]={9,1,1};assert(a.packet(invalid,sizeof(invalid),10));assert(a.malformed()==1);
    assert(a.packet(y,sizeof(y),11));assert(f.data.size()==3);
    uint8_t short_eof[]={2,2,1};assert(a.packet(short_eof,sizeof(short_eof),12));
    uint8_t over[]={2,1,1,2,3,4,5};assert(a.packet(over,sizeof(over),13));
    assert(a.packet(y,sizeof(y),14));assert(f.data.size()==3 && a.dropped()>=3);
    uint8_t pts_scr[]={12,0x0e,1,0,0,0,2,0,0,0,3,0,1,2,3,4};
    assert(a.packet(pts_scr,sizeof(pts_scr),15));assert(f.data.size()==4);
    uint8_t missing_optional[]={2,0x05,1,2};assert(a.packet(missing_optional,sizeof(missing_optional),16));assert(a.malformed()==2);
    uint8_t error[]={2,0x42,1,2,3,4};assert(a.packet(error,sizeof(error),17));assert(f.data.size()==4);
    UvcFrame invalid_size(1310721,Frames::sink,&f);assert(!invalid_size.ready());
    UvcFrame rejects(4,Frames::reject,nullptr);assert(rejects.packet(first,sizeof(first),1));
    uint8_t complete[]={2,3,1,2,3,4};assert(!rejects.packet(complete,sizeof(complete),2));
}
static void full_frame() {
    Frames frames;UvcFrame a(ThermalFrameBytes,Frames::sink,&frames);
    uint8_t first[]={2,0};assert(a.packet(first,2,1));
    std::vector<uint8_t> p(3072);p[0]=12;p[1]=0x0d;p[2]=123;
    size_t offset=0;uint64_t ns=1000000000;
    while(offset<ThermalFrameBytes) {
        const size_t count=ThermalFrameBytes-offset<3060 ? ThermalFrameBytes-offset:3060;
        p[1]=uint8_t(0x0d|(offset+count==ThermalFrameBytes?2:0));
        for(size_t i=0;i<count;++i)p[12+i]=uint8_t(offset+i);
        p[6]=uint8_t(ns/1000000);p[10]=uint8_t(ns/125000);
        assert(a.packet(p.data(),12+count,ns));offset+=count;ns+=125000;
    }
    assert(frames.data.size()==1 && frames.data[0].size()==ThermalFrameBytes);
    for(size_t i=0;i<ThermalFrameBytes;++i)assert(frames.data[0][i]==uint8_t(i));
    assert(frames.infos[0].has_pts && frames.infos[0].pts==123 && frames.infos[0].has_scr && a.pts_changes()==0);
}
int main() {
    uint8_t descriptors[34]{};
    descriptors[0]=18;descriptors[1]=1;descriptors[8]=0xda;descriptors[9]=0x0b;descriptors[10]=0x30;descriptors[11]=0x58;
    descriptors[18]=9;descriptors[19]=4;descriptors[20]=1;descriptors[21]=7;
    descriptors[27]=7;descriptors[28]=5;descriptors[29]=0x81;descriptors[30]=1;descriptors[32]=0x14;descriptors[33]=1;
    assert(thermal_usb_descriptors(descriptors,sizeof(descriptors)));
    for(size_t n=0;n<sizeof(descriptors);++n)assert(!thermal_usb_descriptors(descriptors,n));
    assert(!thermal_usb_descriptors(nullptr,sizeof(descriptors)));
    std::vector<uint8_t> multiple(descriptors,descriptors+18);
    multiple.insert(multiple.end(),descriptors+18,descriptors+34);
    multiple[21]=6; // same endpoint on another alternate is not sufficient
    assert(!thermal_usb_descriptors(multiple.data(),multiple.size()));
    multiple.insert(multiple.end(),descriptors+18,descriptors+34);
    assert(thermal_usb_descriptors(multiple.data(),multiple.size()));
    multiple[20]=0; // unrelated interface before the matching interface
    assert(thermal_usb_descriptors(multiple.data(),multiple.size()));
    multiple.push_back(2);multiple.push_back(0x24);
    assert(thermal_usb_descriptors(multiple.data(),multiple.size()));
    multiple.push_back(1); // reject a truncated tail even after a valid endpoint
    assert(!thermal_usb_descriptors(multiple.data(),multiple.size()));

    for(unsigned i:{0U,1U,8U,9U,10U,11U,18U,19U,20U,21U,27U,28U,29U,30U,31U,32U,33U}) {
        uint8_t saved=descriptors[i];descriptors[i]^=0xff;
        assert(!thermal_usb_descriptors(descriptors,sizeof(descriptors)));descriptors[i]=saved;
    }

    assembly();full_frame();unsigned cases=2;
    { Fake f;UsbIso t(41,f.api());assert(t.start());assert(t.negotiated()[8]==99);Seen s;
      for(unsigned i=0;i<25;++i)assert(t.step(Seen::sink,&s)==1);
      assert(s.packets==50 && f.pending.size()==8);
      const auto &stats=t.statistics();assert(stats.completions==25 && !stats.empty_completions && stats.bytes==200 && stats.data_packets==50);
      assert(t.finish());assert(!stats.alt_zero && !stats.release_control && !stats.release_stream);assert(f.closed && f.discarded==8);assert(t.finish());++cases; }
    { Fake f;UsbIso t(41,f.api());assert(t.start());f.completion_mode=10;f.clock+=17000000;Seen seen;
      assert(t.step(Seen::sink,&seen)==1 && !seen.packets);
      const auto &stats=t.statistics();assert(stats.completions==1 && stats.empty_completions==1 && !stats.bytes && !stats.data_packets && stats.max_reap_gap_ns==17000000);
      assert(t.finish());++cases; }
    for(unsigned fail=1;fail<=15;++fail) {
        Fake f;f.fail_at=fail;UsbIso t(41,f.api());assert(!t.start());assert(t.finish());assert(f.closed);++cases;
    }
    for(bool mismatch:{false,true}) {
        Fake f;f.mismatch=mismatch;f.short_control=!mismatch;UsbIso t(41,f.api());assert(!t.start());assert(errno==EPROTO);assert(t.finish());++cases;
    }
    { Fake f;UsbIso t(41,f.api());assert(t.start());f.unknown=true;Seen s;assert(t.step(Seen::sink,&s)==-1);assert(errno==EPROTO);assert(t.finish());++cases; }
    { Fake f;UsbIso t(41,f.api());assert(t.start());f.block=true;Seen s;assert(t.step(Seen::sink,&s)==0);assert(!t.finish());assert(t.close_fallback() && f.closed && f.clock<=1100000000);++cases; }
    { Fake f;UsbIso t(41,f.api());assert(t.start());assert(t.step(Seen::reject,nullptr)==-1);assert(t.finish());++cases; }
    { Fake f;UsbIso t(41,f.api());assert(t.start());std::swap(f.pending[0],f.pending[1]);Seen s;assert(t.step(Seen::sink,&s)==-1);assert(t.finish());++cases; }
    { Fake f; { UsbIso t(41,f.api());assert(t.start()); } assert(f.closed && f.pending.empty());++cases; }
    for(unsigned mode=1;mode<=9;++mode) {
        Fake f;f.completion_mode=mode;UsbIso t(41,f.api());assert(t.start());unsigned errors=0;
        const int rc=t.step(Seen::errors,&errors);
        if(mode==5 || mode==6)assert(rc==1 && errors==(mode==5?32U:1U) && t.packet_errors()==errors);
        else { assert(rc==-1);assert(t.step(Seen::errors,&errors)==-1 && errno==EINVAL); }
        f.completion_mode=0;assert(t.finish());++cases;
    }
    { Fake f;UsbIso t(41,f.api());assert(t.step(Seen::reject,nullptr)==-1);assert(t.finish());++cases; }
    for(unsigned failure:{16U,32U,33U,34U}) {
        Fake f;UsbIso t(41,f.api());assert(t.start());f.fail_at=failure;assert(!t.finish());assert(f.closed);++cases;
    }
    { Frames f;UvcFrame a(4,Frames::sink,&f);uint8_t first[]={2,0},complete[]={2,3,1,2,3,4};
      assert(a.packet(first,2,1));uint8_t malformed[]={12,1};assert(a.packet(malformed,2,2));
      assert(a.packet(complete,sizeof(complete),3));assert(f.data.empty());
      complete[1]=2;assert(a.packet(complete,sizeof(complete),4));assert(f.data.size()==1);++cases; }
    { Frames f;UvcFrame a(4,Frames::sink,&f);uint8_t first[]={2,0},complete[]={2,3,1,2,3,4};
      assert(a.packet(first,2,1));a.lost();assert(a.packet(complete,sizeof(complete),2));assert(f.data.empty());
      complete[1]=2;assert(a.packet(complete,sizeof(complete),3));assert(f.data.size()==1);++cases; }
    std::printf("ZT30 ISO/UVC private fake PASS cases=%u no-peripheral-access\n",cases);
}
