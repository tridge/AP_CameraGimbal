#include "thermal_live.h"
#include "thermal_pixels.h"
#include "camera_app/log.h"
#include "camera_app/video_metadata.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <ctime>
#include <unistd.h>
namespace ca_zt30 {
[[noreturn]] static void fatal(const char *call,int status=0) {
    ca_log("ZT30 live thermal fatal call=%s raw=%d errno=%d",call,status,errno);_exit(26);
}
uint64_t ThermalLive::now(void*) { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec; }
void ThermalLive::pause(void*,unsigned ms) { timespec t{time_t(ms/1000),long(ms%1000)*1000000};nanosleep(&t,nullptr); }
int ThermalLive::ioctl_call(void*,int fd,unsigned long cmd,void *arg) { return ioctl(fd,cmd,arg); }
int ThermalLive::close_call(void*,int fd) { return ::close(fd); }
ThermalLive::ThermalLive(NativeSdk &s,PacketMemory &p,ca_rtsp *r,ca_live_video_server *l,ca_raw_thermal_server *rs,ca_thermal_stream *stream):
    sdk(s),encoder(s.api,s.encoder,p,s.input),clean_encoder(s.api,s.encoder,p,s.input,0,6),hevc_encoder(s.api,s.encoder,p,s.input,0,8),raw_server(rs),raw_stream(stream),rtsp(r),live(l),assembler(ThermalFrameBytes,frame,this),
    latest(new(std::nothrow) uint8_t[ThermalFrameBytes]),owned(new(std::nothrow) uint8_t[ThermalFrameBytes]),
    nv12(new(std::nothrow) uint8_t[ThermalNv12Bytes]),encoded(new(std::nothrow) uint8_t[4*1024*1024]),clean_encoded(new(std::nothrow) uint8_t[4*1024*1024]),hevc_encoded(new(std::nothrow) uint8_t[4*1024*1024]),radiometric(new(std::nothrow) uint16_t[640*512]) {}
static bool sys_number(const char *directory,const char *name,unsigned &number,unsigned base) {
    char path[320],text[40]{};std::snprintf(path,sizeof(path),"%s/%s",directory,name);
    FILE *f=std::fopen(path,"r");if(!f)return false;
    const bool ok=std::fgets(text,sizeof(text),f)!=nullptr;std::fclose(f);
    if(!ok)return false;
    char *end=nullptr;const unsigned long value=std::strtoul(text,&end,base);
    if(end==text || (*end!='\n' && *end) || value>65535)return false;
    number=unsigned(value);return true;
}
int ThermalLive::open_device() {
    DIR *dir=opendir("/sys/bus/usb/devices");if(!dir)return -1;
    int fd=-1;unsigned matches=0;
    while(auto *e=readdir(dir)) {
        if(e->d_name[0]=='.' || std::strchr(e->d_name,':'))continue;
        char path[320];std::snprintf(path,sizeof(path),"/sys/bus/usb/devices/%s",e->d_name);
        unsigned vendor=0,product=0,bus=0,device=0;
        if(!sys_number(path,"idVendor",vendor,16) || !sys_number(path,"idProduct",product,16) || vendor!=0x0bda || product!=0x5830)continue;
        ++matches;
        if(!sys_number(path,"busnum",bus,10) || !sys_number(path,"devnum",device,10) || !bus || bus>999 || !device || device>127)continue;
        char usb_path[80];std::snprintf(usb_path,sizeof(usb_path),"/dev/bus/usb/%03u/%03u",bus,device);
        int candidate=::open(usb_path,O_RDWR|O_NOFOLLOW|O_CLOEXEC);struct stat st{};
        if(candidate<0)continue;
        if(fstat(candidate,&st) || !S_ISCHR(st.st_mode) || major(st.st_rdev)!=189 || minor(st.st_rdev)!=(bus-1)*128+device-1) { ::close(candidate);continue; }
        // Read exact device identity and ISO endpoint shape from the opened
        // node, to catch device-number reuse between sysfs discovery and open.
        uint8_t desc[4096]{};const ssize_t size=::read(candidate,desc,sizeof(desc));
        if(size<=0 || !thermal_usb_descriptors(desc,size_t(size))) { ::close(candidate);continue; }
        if(fd>=0)::close(fd);
        fd=candidate;
    }
    closedir(dir);if(matches!=1) { if(fd>=0)::close(fd);return -1; }return fd;
}
bool ThermalLive::start(const char *name) {
    if(start_started || (name && (!*name || std::strlen(name)>48)))return false;
    normal=name==nullptr;
    if(name)std::strcpy(run_name,name);
    const int rc=pthread_create(&start_thread,nullptr,[](void *p)->void* {
        auto &self=*static_cast<ThermalLive*>(p);
        if(!self.initialize(self.normal?nullptr:self.run_name)) {
            self.degraded.store(true);ca_log("ZT30 thermal unavailable; RGB continues");
        }
        return nullptr;
    },this);
    start_started=rc==0;
    return start_started;
}
bool ThermalLive::initialize(const char *name) {
    if(!assembler.ready() || !latest || !owned || !nv12 || !encoded || !clean_encoded || !hevc_encoded || !radiometric)return false;
    normal=name==nullptr;
    if(!normal) {
        if(!*name || std::strlen(name)>48)return false;
        for(const char *p=name;*p;++p)if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-'))return false;
    }
    if(mkdir("/tmp/apcg-zt30-thermal",0700) && errno!=EEXIST)return false;
    struct stat directory{};
    if(lstat("/tmp/apcg-zt30-thermal",&directory) || !S_ISDIR(directory.st_mode) || directory.st_uid || (directory.st_mode&0777)!=0700)return false;
    if(!normal) {
        char marker[128];std::snprintf(marker,sizeof(marker),"/tmp/apcg-zt30-thermal/%s.live-started",name);
        const int once=::open(marker,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(once<0)return false;
        ::close(once);
        std::snprintf(raw_sample,sizeof(raw_sample),"/tmp/apcg-zt30-thermal/%s.raw",name);
    }
    int fd=-1;
    const uint64_t deadline=now()+15000000000ULL;
    do {
        if(stop.load())return false;
        fd=open_device();
        if(fd>=0 || !normal)break;
        pause(nullptr,100);
    } while(now()<deadline);
    if(fd<0) { ca_log("ZT30 thermal USB discovery failed");return false; }
    if(stop.load()) { ::close(fd);return false; }
    if(normal && !budget.begin("/tmp/apcg-zt30-thermal/start-budget")) {
        ::close(fd);ca_log("ZT30 thermal startup budget exhausted or invalid; RGB continues until reboot");return false;
    }
    usb.reset(new(std::nothrow) UsbIso(fd,{nullptr,ioctl_call,close_call,now,pause}));
    if(!usb) { ::close(fd);return false; }
    if(!usb->start()) { const int saved=errno;if(!usb->finish())fatal("USB finish");ca_log("ZT30 thermal USB startup failed errno=%d",saved);return false; }
    char negotiated[53]{};for(unsigned i=0;i<26;i++)std::snprintf(negotiated+2*i,3,"%02x",usb->negotiated()[i]);
    ca_log("ZT30 thermal UVC negotiated=%s endpoint=81 alt=7 ring=8 packets=32",negotiated);
    controls.attach(fd);
    // Keep ISO transfers requeued while palette/MI encoder setup may block.
    // Previously the entire ring could complete before capture() was started.
    int rc=pthread_create(&usb_thread,nullptr,[](void *p)->void* { static_cast<ThermalLive*>(p)->capture();return nullptr; },this);
    if(rc)encoder_fatal("USB worker create",rc);
    usb_started=true;
    if(controls.set_palette(wanted_palette))ca_log("ZT30 thermal palette unavailable errno=%d; video continues",errno);
    struct ca_overlay_geometry cross{};ca_overlay_geometry(&cross,640,512,true,false,0);
    if(ca_overlay_bitmaps(cross_bits,640,512,&cross))encoder_fatal("thermal cross bitmap");
    if(clean_encoder.open())encoder_fatal(clean_encoder.failure()?clean_encoder.failure():"clean thermal open",clean_encoder.status());
    if(encoder.open())encoder_fatal(encoder.failure()?encoder.failure():"thermal open",encoder.status());
    if(need_hevc && hevc_encoder.open(EncoderCodec::H265))encoder_fatal("thermal HEVC open",hevc_encoder.status());
    if(!video_callback && (ca_rtsp_add_video(rtsp,"video2",CA_VIDEO_H264,15,&stream_id) || stream_id!=1 || ca_live_video_server_configure(live,1,640,512,15,true)))encoder_fatal("stream setup");
    rc=pthread_create(&encode_thread,nullptr,[](void *p)->void* { static_cast<ThermalLive*>(p)->encode();return nullptr; },this);
    if(rc)encoder_fatal("worker create",rc);
    encode_started=true;
    ca_log("ZT30 live thermal started 640x512 H264/15fps video2; mode=%s",normal?"normal":"diagnostic");return true;
}
bool ThermalLive::frame(void *opaque,const uint8_t *bytes,size_t length,const UvcFrameInfo &info) {
    auto &self=*static_cast<ThermalLive*>(opaque);
    if(length!=ThermalFrameBytes || !info.last_ns)return false;
    std::lock_guard<std::mutex> guard(self.lock);
    timespec wall{};if(clock_gettime(CLOCK_REALTIME,&wall))return false;
    std::memcpy(self.latest.get(),bytes,length);self.receipt=info.last_ns;self.captured_at=wall;++self.generation;return true;
}
bool ThermalLive::snapshot(uint8_t *frame,size_t capacity,timespec &at) {
    if(!frame || capacity<ThermalFrameBytes) { errno=EINVAL;return false; }
    if(stop.load() || usb_failed.load() || degraded.load()) { errno=ENODATA;return false; }
    std::lock_guard<std::mutex> guard(lock);
    const uint64_t current=now();
    if(!receipt || current<receipt || current-receipt>250000000ULL) { errno=EAGAIN;return false; }
    std::memcpy(frame,latest.get(),ThermalFrameBytes);at=captured_at;return true;
}
bool ThermalLive::packet(void *opaque,const IsoPacket &p) {
    auto &self=*static_cast<ThermalLive*>(opaque);
    if(p.status)self.assembler.lost();
    else if(p.bytes && !self.assembler.packet(p.data,p.bytes,p.received_ns))return false;
    return true;
}
void ThermalLive::capture() {
    uint64_t last=now(),seen=0;const char *reason="requested";int error=0;
    while(!stop.load()) {
        const int rc=usb->step(packet,this);
        if(rc<0) { reason="USB transfer";error=errno;usb_failed.store(true);break; }
        if(assembler.frames()!=seen) { seen=assembler.frames();last=now(); }
        if(now()-last>(seen?2000000000ULL:8000000000ULL)) { reason=seen?"frame gap >2s":"no first frame >8s";usb_failed.store(true);break; }
        if(!rc)pause(nullptr,1);
    }
    controls.detach();
    const bool clean=usb->finish();const auto &stats=usb->statistics();
    ca_log("ZT30 thermal USB diagnostic reason=%s errno=%d completions=%llu empty=%llu bytes=%llu data_packets=%llu max_reap_gap_us=%llu cleanup=%u alt0=%d release1=%d release0=%d fallback=%u",
        reason,error,(unsigned long long)stats.completions,(unsigned long long)stats.empty_completions,
        (unsigned long long)stats.bytes,(unsigned long long)stats.data_packets,(unsigned long long)(stats.max_reap_gap_ns/1000),
        unsigned(clean),stats.alt_zero,stats.release_stream,stats.release_control,unsigned(usb->close_fallback()));
    if(!clean)fatal("USB finish");
    ca_log("ZT30 thermal USB stopped frames=%llu dropped=%llu malformed=%llu packet_errors=%llu failure=%u",(unsigned long long)assembler.frames(),(unsigned long long)assembler.dropped(),(unsigned long long)assembler.malformed(),(unsigned long long)usb->packet_errors(),unsigned(usb_failed.load()));
    usb_finished.store(true);
}
[[noreturn]] void ThermalLive::encoder_fatal(const char *call,int status) {
    // Ambiguous MI leases still require a process fence. First give the
    // independent USB worker time to cancel/reap and switch back to alt 0;
    // exiting from the encoder thread previously bypassed that teardown.
    const int saved=errno;stop.store(true);
    if(usb && !usb_started) {
        controls.detach();usb_finished.store(usb->finish());
    }
    const uint64_t deadline=now()+2000000000ULL;
    while(!usb_finished.load() && now()<deadline)pause(nullptr,2);
    ca_log("ZT30 thermal encoder failure: USB drained=%u",unsigned(usb_finished.load()));
    errno=saved;fatal(call,status);
}
void ThermalLive::encode() {
    uint64_t seen=0,frames=0,last=now(),last_receipt=0,healthy_since=0;
    bool budget_reset=false,raw_error_logged=false;
    uint64_t clean_last=now(),record_pts=0,hevc_last=now();bool clean_frames=false,hevc_frames=false;
    EncodedFrame hevc{};hevc.data=hevc_encoded.get();hevc.capacity=4*1024*1024;
    unsigned record_source=2;
    const auto record=[&](unsigned source,const EncodedFrame &frame,uint64_t pts,bool key) {
        const unsigned wanted=(overlay_options.load()&2U)?1U:0U;
        if(source!=wanted || pts<=record_pts)return;
        // Switching encoders mid-GOP would splice incompatible references.
        // Wait for the selected encoder's IDR and preserve increasing PTS.
        if(record_source!=source) { if(!key)return;record_source=source; }
        record_pts=pts;
        if(callback)callback(callback_opaque,frame,pts,key);
    };
    EncodedFrame clean{};clean.data=clean_encoded.get();clean.capacity=4*1024*1024;
    EncodedFrame f{};f.data=encoded.get();f.capacity=4*1024*1024;
    while(!stop.load() && !usb_failed.load()) {
        uint64_t received=0;timespec at{};
        {
            std::lock_guard<std::mutex> guard(lock);
            if(generation!=seen) { seen=generation;received=receipt;at=captured_at;std::memcpy(owned.get(),latest.get(),ThermalFrameBytes); }
        }
        if(received) {
            last_receipt=received;
            if(now()-received>150000000ULL)continue; // bounded age, discard stale input
            if(thermal_uyvy_nv12(owned.get(),ThermalFrameBytes,nv12.get(),640*512,640,nv12.get()+640*512,640*256,640)!=ThermalPixelResult::Ok) { ca_log("ZT30 thermal degraded: pixel conversion");degraded.store(true);break; }
            if(thermal_be16(owned.get(),ThermalFrameBytes,radiometric.get(),640*512)!=ThermalPixelResult::Ok) {
                ca_log("ZT30 thermal degraded: raw plane conversion");degraded.store(true);break;
            }
            if(raw_server) {
                const bool error=ca_raw_thermal_server_publish(raw_server,radiometric.get(),&at)<0;
                if(error && !raw_error_logged)ca_log("ZT30 raw TCP publish failed errno=%d; repeats suppressed",errno);
                raw_error_logged=error;
            }
            ca_thermal_stream_publish_counts(raw_stream,radiometric.get(),&at,255,false,received/1000);
            uint64_t pts=0;const auto before=now();const int rc=sdk.current_pts(0,&pts);const auto after=now();
            if(rc) { ca_log("ZT30 thermal degraded: GetCurPts raw=%d",rc);degraded.store(true);break; }
            if(after-before>2000000ULL) {
                ca_log("ZT30 thermal dropping slow PTS sample bracket_ns=%llu",(unsigned long long)(after-before));
            } else {
                if(!clock.observe(pts,before,after) || !clean_clock.observe(pts,before,after) || (need_hevc && !hevc_clock.observe(pts,before,after))) { ca_log("ZT30 thermal degraded: SYS clock ordering/drift");degraded.store(true);break; }
            // Presentation uses SYS's sampled clock; USB receipt is not claimed
            // to be exposure time or a calibrated conversion of UVC timestamps.
            if(stop.load())break;
            const auto clean_result=clean_encoder.submit(nv12.get(),ThermalNv12Bytes,pts);
            if(clean_result!=InputBufferResult::Submitted) {
                ca_log("ZT30 clean thermal submit result=%u get=%d put=%d stop=%u",unsigned(clean_result),clean_encoder.get_status(),clean_encoder.put_status(),unsigned(stop.load()));
                encoder_fatal("clean thermal input submit");
            }
            if(overlay_options.load()&1U) {
                // Overlay only the display plane. Raw 16-bit counts are never modified.
                for(const auto &b:cross_bits)if(b.pixels)
                    for(unsigned y=0;y<b.height;y++)for(unsigned x=0;x<b.width;x++) {
                        const uint16_t color=b.pixels[y*b.width+x];if(!(color&0x8000))continue;
                        nv12[(b.y+y)*640+b.x+x]=(color&0x7fff)?235:16;
                        const unsigned chroma=640*512+((b.y+y)/2)*640+((b.x+x)&~1U);
                        nv12[chroma]=nv12[chroma+1]=128;
                    }
            }
            if(stop.load())break;
            if(need_hevc) {
                const auto result=hevc_encoder.submit(nv12.get(),ThermalNv12Bytes,pts);
                if(result!=InputBufferResult::Submitted) {
                    ca_log("ZT30 thermal HEVC submit result=%u get=%d put=%d stop=%u",unsigned(result),hevc_encoder.get_status(),hevc_encoder.put_status(),unsigned(stop.load()));
                    encoder_fatal("thermal HEVC input submit");
                }
            }
            if(stop.load())break;
            const auto result=encoder.submit(nv12.get(),ThermalNv12Bytes,pts);
                if(result!=InputBufferResult::Submitted) {
                    ca_log("ZT30 thermal submit result=%u get=%d put=%d stop=%u",unsigned(result),encoder.get_status(),encoder.put_status(),unsigned(stop.load()));
                    encoder_fatal("input submit");
                }
                if(!frames && raw_sample[0]) {
                    FILE *sample=std::fopen(raw_sample,"wbx");
                    if(sample) { const size_t bytes=std::fwrite(owned.get(),1,ThermalFrameBytes,sample);std::fclose(sample);ca_log("ZT30 thermal raw sample bytes=%zu generation=%llu pts=%llu",bytes,(unsigned long long)seen,(unsigned long long)pts); }
                    raw_sample[0]=0;
                }
            }
        }
        const int got=encoder.copy_frame(f);if(got<0)encoder_fatal(encoder.failure()?encoder.failure():"copy frame",encoder.status());
        if(got) {
            uint64_t mapped=0;bool valid=f.packets && clock.frame(f.pts[0],now()/1000,mapped);
            for(unsigned i=1;i<f.packets;++i)if(f.pts[i]!=f.pts[0])valid=false;
            if(!valid) { ca_log("ZT30 thermal degraded: frame/packet PTS");degraded.store(true);break; }
            size_t offset=0,nal=0,length=0;bool key=false;
            while(ca_annexb_next(f.data,f.bytes,&offset,&nal,&length))if(length && (f.data[nal]&31)==5)key=true;
            if(video_callback)video_callback(video_opaque,EncoderCodec::H264,f,mapped,key);
            else {
                (void)ca_rtsp_push_video_timed(rtsp,stream_id,f.data,f.bytes,key,0,mapped);
                if(ca_live_video_server_publish(live,1,f.data,f.bytes,mapped,key,0)) { ca_log("ZT30 thermal degraded: web publish");degraded.store(true);break; }
            }
            record(1,f,mapped,key);
            ++frames;const auto previous=last;last=now();published_ns.store(last);
            if(!healthy_since || last-previous>=1000000000ULL)healthy_since=last;
            if(normal && !budget_reset && frames>=9000 && last-healthy_since>=600000000000ULL) {
                budget_reset=budget.reset();
                if(!budget_reset)ca_log("ZT30 thermal startup budget reset failed");
            }
            if(frames==1 || !(frames%150))ca_log("ZT30 thermal live frames=%llu latest=%llu age_ms=%llu",(unsigned long long)frames,(unsigned long long)seen,(unsigned long long)((now()-last_receipt)/1000000));
        }
        const int clean_got=clean_encoder.copy_frame(clean);
        if(clean_got<0)encoder_fatal("clean thermal frame",clean_encoder.status());
        if(clean_got) {
            uint64_t mapped=0;bool valid=clean.packets && clean_clock.frame(clean.pts[0],now()/1000,mapped);
            for(unsigned i=1;i<clean.packets;i++)if(clean.pts[i]!=clean.pts[0])valid=false;
            if(!valid) { degraded.store(true);break; }
            size_t offset=0,nal=0,length=0;bool key=false;
            while(ca_annexb_next(clean.data,clean.bytes,&offset,&nal,&length))if(length && (clean.data[nal]&31)==5)key=true;
            record(0,clean,mapped,key);
            clean_last=now();clean_frames=true;
        }
        if(need_hevc) {
            const int got_hevc=hevc_encoder.copy_frame(hevc);if(got_hevc<0)encoder_fatal("thermal HEVC frame",hevc_encoder.status());
            if(got_hevc) {
                uint64_t mapped=0;bool valid=hevc.packets && hevc_clock.frame(hevc.pts[0],now()/1000,mapped);
                for(unsigned i=1;i<hevc.packets;i++)if(hevc.pts[i]!=hevc.pts[0])valid=false;
                if(!valid) { degraded.store(true);break; }
                size_t offset=0,nal=0,length=0;bool key=false;
                while(ca_annexb_next(hevc.data,hevc.bytes,&offset,&nal,&length))if(length) {
                    const unsigned type=(hevc.data[nal]>>1)&63;if(type>=16 && type<=21)key=true;
                }
                if(video_callback)video_callback(video_opaque,EncoderCodec::H265,hevc,mapped,key);
                hevc_last=now();hevc_frames=true;
            }
            if(now()-hevc_last>(hevc_frames?4000000000ULL:10000000000ULL)) { ca_log("ZT30 thermal HEVC starvation");degraded.store(true);break; }
        }
        if(now()-clean_last>(clean_frames?4000000000ULL:10000000000ULL)) { ca_log("ZT30 clean thermal encoder starvation");degraded.store(true);break; }
        if(now()-last>(frames?4000000000ULL:10000000000ULL)) { ca_log("ZT30 thermal degraded: encoder starvation");degraded.store(true);break; }
        pause(nullptr,2);
    }
    stop.store(true);
    if(hevc_encoder.close())encoder_fatal("HEVC encoder close",hevc_encoder.status());
    if(clean_encoder.close())encoder_fatal("clean encoder close",clean_encoder.status());
    if(encoder.close())encoder_fatal("encoder close",encoder.status());
    ca_log("ZT30 thermal encoder stopped frames=%llu",(unsigned long long)frames);
}
bool ThermalLive::ready() const {
    const auto last=published_ns.load();
    return last && !stop.load() && !usb_failed.load() && !degraded.load() && now()-last<1000000000ULL;
}
void ThermalLive::join() {
    stop.store(true);
    if(start_started) { if(pthread_join(start_thread,nullptr))fatal("startup join");start_started=false; }
    if(usb_started) { if(pthread_join(usb_thread,nullptr))fatal("USB join");usb_started=false; }
    if(encode_started) { if(pthread_join(encode_thread,nullptr))fatal("encoder join");encode_started=false; }
    if(hevc_encoder.close())fatal("HEVC encoder close",hevc_encoder.status());
    if(clean_encoder.close())fatal("clean encoder close",clean_encoder.status());
    if(encoder.close())fatal("encoder close",encoder.status());
    controls.detach();
    ca_overlay_free(cross_bits);
    if(normal && !usb_failed.load() && !degraded.load() && published_ns.load() && !budget.reset())
        ca_log("ZT30 thermal clean-shutdown budget reset failed");
}
}
