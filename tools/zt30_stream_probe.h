#pragma once
#include "camera_app/rtsp.h"
#ifdef ZT30_H264_WEB_PROBE
#include "camera_app/live_video_server.h"
#endif
#include "camera_app/video_metadata.h"
#include "../camera_app/src/backends/zt30/pts_clock.h"
/* Diagnostic transport shares the production RTSP implementation. No SDK
 * loads here; instantiate only in the exclusive, qualified RAM experiment.
 */
class StreamProbe {
public:
    ~StreamProbe() {
#ifdef ZT30_H264_WEB_PROBE
        ca_live_video_server_close(live);
#endif
        ca_rtsp_close(server);
    }
    bool open(const char *marker,bool wide=false) {
        timed_gates=wide;
        char gate[160]{};
        std::memcpy(gate,marker,sizeof(gate));
        char *name=std::strrchr(gate,'/');
        if(!name || size_t(name+1-gate)+sizeof("stream.ready")>sizeof(gate))return false;
        std::memcpy(name+1,"stream.ready",sizeof("stream.ready"));
        struct stat st{};
        if(!lstat(gate,&st) || errno!=ENOENT)return false;
        std::memcpy(done,gate,sizeof(done));
        std::memcpy(std::strrchr(done,'/')+1,"stream.done",sizeof("stream.done"));
        if(!lstat(done,&st) || errno!=ENOENT)return false;
#ifdef ZT30_H264_WEB_PROBE
        if(ca_live_video_server_open(&live,8555)) { std::fprintf(stderr,"LIVE OPEN FAIL errno=%d\n",errno);return false; }
        if(ca_live_video_server_configure(live,0,1280,720,30,true)) { std::fprintf(stderr,"LIVE CONFIG FAIL errno=%d\n",errno);return false; }
#endif
        if(ca_rtsp_open(&server,18554,"video1",codec,30)) { std::fprintf(stderr,"RTSP OPEN FAIL errno=%d\n",errno);return false; }
        // This diagnostic builds xop without its asynchronous SIGTERM-ignore
        // policy; retain the explicit process-owned stop handlers.
        #ifndef ZT30_RAM_FACTORY
        signal(SIGTERM,stop);signal(SIGINT,stop);
#endif
        std::printf("RTSP_READY pid=%ld port=18554 path=video1\n",long(getpid()));
        const uint64_t begin=monotonic_ns(),deadline=begin+(wide?6000000000ULL:8000000000ULL);
        while(!stopped && monotonic_ns()<deadline) {
            if(!lstat(gate,&st)) {
                const bool valid=S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 && st.st_size==0;
                if(timed_gates)std::printf("STREAM_CLIENT_GATE valid=%d elapsed_ns=%llu\n",int(valid),static_cast<unsigned long long>(monotonic_ns()-begin));
                return valid;
            }
            if(errno!=ENOENT)return false;
            pause_ms(20);
        }
        return false;
    }
    bool drained() {
        const uint64_t begin=monotonic_ns(),deadline=begin+2000000000ULL;
        struct stat st{};
        while(!stopped && monotonic_ns()<deadline) {
            if(!lstat(done,&st)) {
                const bool valid=S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 && st.st_size==0;
                if(timed_gates)std::printf("STREAM_DRAIN_GATE valid=%d elapsed_ns=%llu\n",int(valid),static_cast<unsigned long long>(monotonic_ns()-begin));
                return valid;
            }
            if(errno!=ENOENT)return false;
            pause_ms(20);
        }
        return false;
    }
    bool observe(ca_zt30::NativeSdk &sdk) {
        const bool initial=!clock.healthy();
        raw_clock=0;
        before_ns=monotonic_ns();
        const int rc=sdk.current_pts(0,&raw_clock);
        after_ns=monotonic_ns();
        const bool ok=!rc && clock.observe(raw_clock,before_ns,after_ns);
        if(ok && initial)std::printf("CLOCK_ORIGIN pts=%llu before_ns=%llu after_ns=%llu\n",static_cast<unsigned long long>(raw_clock),static_cast<unsigned long long>(before_ns),static_cast<unsigned long long>(after_ns));
        return ok;
    }
    bool push(const ca_zt30::EncodedFrame &frame) {
        if(!clock.frame(frame.pts[0],after_ns/1000,mapped_us))return false;
        // All packets in this access unit must carry its single presentation
        // timestamp. No mixing of SDK clocks or malformed multi-frame units.
        for(unsigned i=1;i<frame.packets;i++)if(frame.pts[i]!=frame.pts[0])return false;
        size_t offset=0,nal=0,length=0;bool key=false;
        while(ca_annexb_next(frame.data,frame.bytes,&offset,&nal,&length)) {
            if(length<2)continue;
            const unsigned type=codec==CA_VIDEO_H264 ? frame.data[nal]&31 : (frame.data[nal]>>1)&63;
            if(codec==CA_VIDEO_H264 ? type==5 : type>=16 && type<=21)key=true;
        }
        // No UART ownership/physical zoom-state qualification in this stage:
        // HFOV unknown, rather than assuming the motor happens to be at1x.
        (void)ca_rtsp_push_video_timed(server,0,frame.data,frame.bytes,key,0,mapped_us);
#ifdef ZT30_H264_WEB_PROBE
        if(ca_live_video_server_publish(live,0,frame.data,frame.bytes,mapped_us,key,0))return false;
#endif
        return true; // no-viewer PushFrame can return -1; host validates receipt
    }
    uint64_t raw_clock=0,before_ns=0,after_ns=0,mapped_us=0;
private:
#ifdef ZT30_H264_WEB_PROBE
    static constexpr ca_video_codec codec=CA_VIDEO_H264;
    ca_live_video_server *live=nullptr;
#else
    static constexpr ca_video_codec codec=CA_VIDEO_H265;
#endif
    char done[160]{};
    bool timed_gates=false;
    ca_rtsp *server=nullptr;
    ca_zt30::PtsClock clock;
};
