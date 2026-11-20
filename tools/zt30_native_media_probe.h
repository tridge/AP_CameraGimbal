#pragma once
#include "../camera_app/src/backends/zt30/media_backend.h"
struct NativeMediaContext {
    const char *marker;
    char sampled[160]{};
    int fd=-1;
    FILE *meta=nullptr;
    StreamProbe sink;
    uint64_t begin=0;
    bool wide=false,full_app=false;
    unsigned frames=0;
    size_t bytes=0;
    static bool gate(void *opaque,bool after_iq) {
        auto &c=*static_cast<NativeMediaContext*>(opaque);
        if(after_iq)std::printf("IQ PASS key=1234\n");
        else {
            const auto &path=c.wide?ca_zt30::WidePath:ca_zt30::ZoomPath;
            std::printf("BIND_READY pid=%ld elapsed_ns=%llu sensor=%u vif=%u isp=%u scl=%u port=1 size=1280x720\n",long(getpid()),static_cast<unsigned long long>(monotonic_ns()-c.begin),path.sensor_pad,path.vif_device,path.isp_channel,path.scl_channel);
        }
        const char *path=after_iq?c.sampled:c.marker;
        const uint64_t deadline=monotonic_ns()+(c.wide?(after_iq?6000000000ULL:8000000000ULL):10000000000ULL);struct stat st{};
        while(!stopped && monotonic_ns()<deadline) {
            if(!lstat(path,&st))return S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 && st.st_size==0;
            if(errno!=ENOENT)return false;
            pause_ms(20);
        }
        return false;
    }
    static bool open_sink(void *opaque) {
        auto &c=*static_cast<NativeMediaContext*>(opaque);
        return c.sink.open(c.marker,c.wide) && c.sink.observe(*c.sdk);
    }
    static bool vui(void*,const char *phase,const ca_zt30::H264Vui &v) {
        if(!std::strcmp(phase,"pre-start"))std::printf("H264_VUI_DEFAULT hex=");
        else std::printf("H264_VUI_OBSERVED phase=%s hex=",phase);
        const auto *raw=reinterpret_cast<const uint8_t*>(&v);
        for(size_t i=0;i<sizeof(v);i++)std::printf("%02x",unsigned(raw[i]));
        std::printf("\n");return true;
    }
    static bool frame(void *opaque,const ca_zt30::EncodedFrame &f,uint64_t before,uint64_t after) {
        auto &c=*static_cast<NativeMediaContext*>(opaque);
        if(!c.sink.observe(*c.sdk) || !c.sink.push(f) || !write_all(c.fd,f.data,f.bytes))return false;
        std::fprintf(c.meta,"{\"sequence\":%u,\"packets\":%u,\"bytes\":%zu,\"copy_begin_ns\":%llu,\"copy_end_ns\":%llu,\"pts\":[",f.sequence,f.packets,f.bytes,static_cast<unsigned long long>(before),static_cast<unsigned long long>(after));
        for(unsigned i=0;i<f.packets;i++)std::fprintf(c.meta,"%s%llu",i?",":"",static_cast<unsigned long long>(f.pts[i]));
        std::fprintf(c.meta,"],\"clock_pts\":%llu,\"clock_before_ns\":%llu,\"clock_after_ns\":%llu,\"mapped_us\":%llu}\n",static_cast<unsigned long long>(c.sink.raw_clock),static_cast<unsigned long long>(c.sink.before_ns),static_cast<unsigned long long>(c.sink.after_ns),static_cast<unsigned long long>(c.sink.mapped_us));
        if(std::ferror(c.meta))return false;
        c.frames++;c.bytes+=f.bytes;return true;
    }
    static bool drain(void *opaque) {
        auto &c=*static_cast<NativeMediaContext*>(opaque);
        if(!c.sink.drained())return false;
        if(!c.wide)return true;
        std::printf("ENCODER_CAPTURE_READY source_scl=1 venc=1 frames=%u bytes=%zu\n",c.frames,c.bytes);
        char consumer[160]{},checked[160]{};
        std::snprintf(consumer,sizeof(consumer),"%s",c.marker);
        char *tail=std::strrchr(consumer,'/');if(!tail)return false;
        std::snprintf(tail+1,sizeof(consumer)-size_t(tail+1-consumer),"consumer.sampled");
        std::snprintf(checked,sizeof(checked),"%s",consumer);tail=std::strrchr(checked,'/');
        std::snprintf(tail+1,sizeof(checked)-size_t(tail+1-checked),"full-app.checked");
        auto acknowledged=[](const char *path) {
            struct stat st{};
            return !lstat(path,&st) && S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 && st.st_size==0;
        };
        bool have_consumer=false,have_app=!c.full_app;
        const uint64_t deadline=monotonic_ns()+6000000000ULL;
        while(!stopped && monotonic_ns()<deadline) {
            have_consumer=acknowledged(consumer);have_app=!c.full_app || acknowledged(checked);
            if(have_consumer && have_app) {
                std::puts("CONSUMER_GATE PASS wide bind snapshot acknowledged");return true;
            }
            pause_ms(20);
        }
        std::fprintf(stderr,"CONSUMER_GATE FAIL consumer=%d full_app=%d\n",int(have_consumer),int(have_app));return false;
    }
    ca_zt30::NativeSdk *sdk=nullptr;
};
#ifndef ZT30_RAM_FACTORY
static int capture_native_media(ca_zt30::NativeSdk &sdk,const char *marker,uint8_t *buffer) {
    NativeMediaContext c{};c.marker=marker;c.sdk=&sdk;c.begin=monotonic_ns();
    char video[160]{},metrics[160]{};
    std::memcpy(video,marker,sizeof(video));char *tail=std::strrchr(video,'/');
    if(!tail || size_t(tail+1-video)+sizeof("encoded.h264")>sizeof(video))return 20;
    std::memcpy(tail+1,"encoded.h264",sizeof("encoded.h264"));
    std::memcpy(metrics,marker,sizeof(metrics));tail=std::strrchr(metrics,'/');
    if(!tail || size_t(tail+1-metrics)+sizeof("encoded.jsonl")>sizeof(metrics))return 20;
    std::memcpy(tail+1,"encoded.jsonl",sizeof("encoded.jsonl"));
    std::memcpy(c.sampled,marker,sizeof(c.sampled));tail=std::strrchr(c.sampled,'/');
    if(!tail || size_t(tail+1-c.sampled)+sizeof("sampled.ready")>sizeof(c.sampled))return 20;
    std::memcpy(tail+1,"sampled.ready",sizeof("sampled.ready"));
    c.fd=open(video,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);
    const int metadata_fd=open(metrics,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);
    if(c.fd<0 || metadata_fd<0)_exit(20);
    c.meta=fdopen(metadata_fd,"w");if(!c.meta)_exit(20);
    ca_zt30::PacketMemory memory{packet_slot,copy_packet};
    ca_zt30::MediaBackend media(sdk.api,sdk.encoder,memory);
    ca_zt30::MediaHooks hooks{&c,&stopped,NativeMediaContext::gate,NativeMediaContext::open_sink,NativeMediaContext::vui,NativeMediaContext::frame,NativeMediaContext::drain};
    std::printf("H264_VUI_POLICY read-only\n");
    if(media.initialize(hooks,buffer,4U*1024*1024)) {
        const int closed=media.shutdown();
        std::fprintf(stderr,"NATIVE_MEDIA INIT FAIL result=%d cleanup=%d\n",media.result(),closed);_exit(closed && closed!=EOWNERDEAD?25:media.result());
    }
    // These getters must not touch actuators or guess a physical zoom value.
    if(media.tracking_available() || media.recording() || media.thermal_main() || media.hfov(false)!=0 ||
       media.frame_rate(true)!=0 || media.frame_rate(false)!=30 || std::isfinite(media.zoom()) || media.lens()!=CA_MEDIA_LENS_ZOOM)_exit(30);
    if(media.ready())std::printf("NATIVE_MEDIA READY tracking=0 recording=0 thermal_fps=0 visible_fps=30 hfov=0 zoom=unknown\n");
    while(!stopped && !media.finished())pause_ms(10);
    const int closed=media.shutdown();const int code=media.result();
    const unsigned frames=media.frames();const size_t bytes=media.bytes();
    std::printf("ENCODER CLOSE raw=%d frames=%u bytes=%zu elapsed_ns=%llu\n",closed,frames,bytes,static_cast<unsigned long long>(monotonic_ns()-c.begin));
    std::printf("CLOSE raw=%d elapsed_ns=%llu\n",closed,static_cast<unsigned long long>(monotonic_ns()-c.begin));
    if(closed)_exit(25);
    const int video_closed=close(c.fd),metadata_closed=std::fclose(c.meta);
    if(video_closed || metadata_closed)return 23;
    if(code)return code;
    if(media.ready())return 30;
    std::printf("NATIVE_MEDIA PASS joined consumer; copied/released buffers; unsupported controls\n");
    std::printf("ENCODER PASS copied H264 payloads; independent host decode required\n");
    return 0;
}

#endif // !ZT30_RAM_FACTORY
