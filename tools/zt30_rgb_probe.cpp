/* Bounded one-path native qualification, not camera-app/release activation.
 * Default invocation is layout-only and never dlopens SDK libraries.
 * Controller must establish exclusive ownership plus runtime/IQ provenance.
 */
#include "../camera_app/src/backends/zt30/rgb_owner.h"
#include "../camera_app/src/backends/zt30/mi/native_sdk.h"
#ifdef ZT30_ENCODER_PROBE
#include "../camera_app/src/backends/zt30/encoder_owner.h"
#include <memory>
#include <new>
static_assert(sizeof(uintptr_t)==4,"encoder probe requires ARM32 pointer ABI");
#endif
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
static volatile sig_atomic_t stopped;
static void stop(int) { stopped=1; }
static uint64_t monotonic_ns() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000000+t.tv_nsec;
}
static void pause_ms(unsigned ms) {
    timespec t{time_t(ms/1000),long(ms%1000)*1000000};
    while(!stopped && nanosleep(&t,&t)<0 && errno==EINTR) {}
}
static bool ram_marker(char (&path)[160]) {
    char executable[160]{};
    const ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if(n<0 || n>=ssize_t(sizeof(executable)-1))return false;
    const char prefix[]="/tmp/apcg-zt30.session.";
    if(std::strncmp(executable,prefix,sizeof(prefix)-1))return false;
    char *tail=executable+sizeof(prefix)-1;
    for(unsigned i=0;i<6;i++)if(!((tail[i]>='0' && tail[i]<='9') || (tail[i]>='a' && tail[i]<='z') || (tail[i]>='A' && tail[i]<='Z')))return false;
    if(std::strcmp(tail+6,"/rgb-probe"))return false;
    tail[6]=0;
    struct statfs fs{};
    if(statfs(executable,&fs) || fs.f_type!=0x01021994)return false;
    constexpr size_t directory_length=sizeof(prefix)-1+6;
    const char suffix[]="/frames.ready";
    static_assert(directory_length+sizeof(suffix)<=sizeof(path),"RAM marker path capacity");
    std::memcpy(path,executable,directory_length);
    std::memcpy(path+directory_length,suffix,sizeof(suffix));
    return true;
}

#ifdef ZT30_STREAM_PROBE
#include "zt30_stream_probe.h"
#endif
#ifdef ZT30_ENCODER_PROBE
static bool packet_slot(const void *pointer,uint32_t *slot) {
    const uintptr_t value=reinterpret_cast<uintptr_t>(pointer);
    if(!value)return false;
    *slot=uint32_t(value);return true;
}
static bool copy_packet(uint32_t address,uint8_t *out,size_t bytes) {
    if(!address || !bytes || uint64_t(address)+bytes>UINT64_C(0x100000000))return false;
    // SDK mappings are borrowed and can be VM_IO/PFNMAP, which proc/self/mem
    // cannot necessarily read. Validate a readable mapping before memcpy;
    // this process has exactly one consumer, and release happens afterwards.
    FILE *maps=std::fopen("/proc/self/maps","r");
    if(!maps)return false;
    char line[512]{};bool readable=false;
    while(std::fgets(line,sizeof(line),maps)) {
        unsigned long first=0,last=0;char permissions[5]{};
        if(std::sscanf(line,"%lx-%lx %4s",&first,&last,permissions)==3 && permissions[0]=='r' &&
           first<=address && uint64_t(address)+bytes<=last) { readable=true;break; }
    }
    std::fclose(maps);
    if(readable)std::memcpy(out,reinterpret_cast<const void*>(uintptr_t(address)),bytes);
    return readable;
}
static bool write_all(int fd,const uint8_t *data,size_t bytes) {
    while(bytes && !stopped) {
        const ssize_t n=write(fd,data,bytes);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return false;
        data+=n;bytes-=size_t(n);
    }
    return !bytes && !stopped;
}
#ifdef ZT30_NATIVE_MEDIA_PROBE
#include "zt30_native_media_probe.h"
#endif
static int capture_encoder(ca_zt30::NativeSdk &sdk,const char *marker,uint8_t *buffer,bool observe_vui,bool wide=false) {
#ifndef ZT30_H264_WEB_PROBE
    (void)observe_vui;
#endif
#ifdef ZT30_H264_WEB_PROBE
    const char filename[]="encoded.h264";
#else
    const char filename[]="encoded.hevc";
#endif
    char video[160]{},metrics[160]{};
    std::memcpy(video,marker,sizeof(video));char *p=std::strrchr(video,'/');
    if(!p || size_t(p+1-video)+sizeof(filename)>sizeof(video))return 20;
    std::memcpy(p+1,filename,sizeof(filename));
    std::memcpy(metrics,marker,sizeof(metrics));p=std::strrchr(metrics,'/');
    if(!p || size_t(p+1-metrics)+sizeof("encoded.jsonl")>sizeof(metrics))return 20;
    std::memcpy(p+1,"encoded.jsonl",sizeof("encoded.jsonl"));
    const int fd=open(video,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return 20;
    const int meta_fd=open(metrics,O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);
    if(meta_fd<0) { close(fd);return 20; }
    FILE *meta=fdopen(meta_fd,"w");
    if(!meta) { close(fd);close(meta_fd);return 20; }
#ifdef ZT30_STREAM_PROBE
    StreamProbe stream;
    if(!stream.open(marker,wide) || !stream.observe(sdk)) { close(fd);std::fclose(meta);return 26; }
#endif
    ca_zt30::EncodedFrame frame{};frame.data=buffer;frame.capacity=4*1024*1024;
    ca_zt30::PacketMemory memory{packet_slot,copy_packet};
    ca_zt30::EncoderOwner encoder(sdk.api,sdk.encoder,memory);
#ifdef ZT30_H264_WEB_PROBE
    std::printf("H264_VUI_POLICY %s\n",observe_vui?"read-only":"fixed30");
    const int opened=encoder.open(ca_zt30::EncoderCodec::H264,observe_vui?ca_zt30::H264Timing::ReadOnly:ca_zt30::H264Timing::Fixed30,
                                  wide?ca_zt30::EncoderSource::Wide:ca_zt30::EncoderSource::Zoom);
#else
    const int opened=encoder.open(ca_zt30::EncoderCodec::H265,ca_zt30::H264Timing::ReadOnly,
                                  wide?ca_zt30::EncoderSource::Wide:ca_zt30::EncoderSource::Zoom);
#endif
#ifdef ZT30_H264_WEB_PROBE
    if(encoder.have_default_vui()) {
        std::printf("H264_VUI_DEFAULT hex=");
        const auto *raw=reinterpret_cast<const uint8_t*>(&encoder.default_vui());
        for(size_t i=0;i<sizeof(ca_zt30::H264Vui);i++)std::printf("%02x",unsigned(raw[i]));
        std::printf("\n");
    }
    if(!opened && !observe_vui) { const auto &vui=encoder.applied_vui();std::printf("H264_VUI_APPLIED timing=%u fixed=%u units=%u scale=%u\n",unsigned(vui.timing_present),unsigned(vui.fixed_rate),vui.units,vui.time_scale); }
#endif
    if(opened) {
        std::fprintf(stderr,"ENCODER OPEN FAIL raw=%d call=%s\n",opened,encoder.failed_call?encoder.failed_call:"unknown");
        // Failed create can leave SDK mutex state. Process teardown follows;
        // do not unload SDK or close SYS while cleanup health is uncertain.
        _exit(21);
    }
#ifdef ZT30_H264_WEB_PROBE
    auto observe=[&](const char *phase) {
        ca_zt30::H264Vui value{};
        const int rc=encoder.read_h264_vui(value);
        if(rc) { std::fprintf(stderr,"H264_VUI_OBSERVE FAIL phase=%s raw=%d\n",phase,rc);return false; }
        std::printf("H264_VUI_OBSERVED phase=%s hex=",phase);
        const auto *raw=reinterpret_cast<const uint8_t*>(&value);
        for(size_t i=0;i<sizeof(value);i++)std::printf("%02x",unsigned(raw[i]));
        std::printf("\n");return true;
    };
    const bool initial_vui_ok=!observe_vui || observe("started");
#endif
    unsigned frames=0;size_t bytes=0;int code=0;
#ifdef ZT30_H264_WEB_PROBE
    if(!initial_vui_ok)code=22;
#endif
    const uint64_t begin=monotonic_ns(),deadline=begin+6000000000ULL;
    while(!code && !stopped && frames<90 && monotonic_ns()<deadline) {
        const uint64_t before=monotonic_ns();
        const int got=encoder.copy_frame(frame);
        const uint64_t after=monotonic_ns();
        if(got<0) { std::fprintf(stderr,"ENCODER COPY FAIL raw=%d call=%s\n",encoder.raw_status,encoder.failed_call?encoder.failed_call:"unknown");code=22;break; }
        if(!got) { pause_ms(5);continue; }
#ifdef ZT30_H264_WEB_PROBE
        if(observe_vui && frames==0 && !observe("first-frame")) { code=22;break; }
#endif
#ifdef ZT30_STREAM_PROBE
        if(!stream.observe(sdk) || !stream.push(frame)) { code=27;break; }
#endif
        if(frame.bytes>32U*1024*1024-bytes || !write_all(fd,frame.data,frame.bytes)) { code=23;break; }
        std::fprintf(meta,"{\"sequence\":%u,\"packets\":%u,\"bytes\":%zu,\"copy_begin_ns\":%llu,\"copy_end_ns\":%llu,\"pts\":[",frame.sequence,frame.packets,frame.bytes,static_cast<unsigned long long>(before),static_cast<unsigned long long>(after));
        for(unsigned i=0;i<frame.packets;i++)std::fprintf(meta,"%s%llu",i?",":"",static_cast<unsigned long long>(frame.pts[i]));
#ifdef ZT30_STREAM_PROBE
        std::fprintf(meta,"],\"clock_pts\":%llu,\"clock_before_ns\":%llu,\"clock_after_ns\":%llu,\"mapped_us\":%llu}\n",static_cast<unsigned long long>(stream.raw_clock),static_cast<unsigned long long>(stream.before_ns),static_cast<unsigned long long>(stream.after_ns),static_cast<unsigned long long>(stream.mapped_us));
#else
        std::fprintf(meta,"]}\n");
#endif
        if(std::ferror(meta)) { code=23;break; }
        frames++;bytes+=frame.bytes;
    }
#ifdef ZT30_STREAM_PROBE
    if(!code && (stopped || frames!=90))code=24; // transport qualification requires90
#else
    if(!code && (stopped || frames<60))code=24;
#endif
#ifdef ZT30_STREAM_PROBE
    // The RTSP publisher queues asynchronously. Host acknowledges all90 AUs
    // before removing sessions; never infer network delivery from PushFrame.
    if(!code && !stream.drained())code=28;
#endif
    if(wide && !code) {
        char consumer[160]{};
        std::memcpy(consumer,marker,sizeof(consumer));
        char *leaf=std::strrchr(consumer,'/');
        if(!leaf || size_t(leaf+1-consumer)+sizeof("consumer.sampled")>sizeof(consumer))code=29;
        else {
            std::memcpy(leaf+1,"consumer.sampled",sizeof("consumer.sampled"));
            std::printf("ENCODER_CAPTURE_READY source_scl=1 venc=1 frames=%u bytes=%zu\n",frames,bytes);
            const uint64_t hold=monotonic_ns()+6000000000ULL;
            bool sampled=false;
            while(!stopped && monotonic_ns()<hold) {
                struct stat st{};
                if(!lstat(consumer,&st)) {
                    sampled=S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 && st.st_size==0;
                    break;
                }
                if(errno!=ENOENT)break;
                pause_ms(20);
            }
            if(!sampled || stopped) { std::fprintf(stderr,"CONSUMER_GATE FAIL\n");code=29; }
            else std::printf("CONSUMER_GATE PASS wide bind snapshot acknowledged\n");
        }
    }
    const int closed=encoder.close();
    std::printf("ENCODER CLOSE raw=%d frames=%u bytes=%zu elapsed_ns=%llu\n",closed,frames,bytes,static_cast<unsigned long long>(monotonic_ns()-begin));
    if(closed)_exit(25); // callback/mapping cleanup must precede RGB/SYS exit
    const int video_closed=close(fd),meta_closed=std::fclose(meta);
    if(video_closed || meta_closed)code=23;
#ifdef ZT30_H264_WEB_PROBE
    if(!code)std::printf("ENCODER PASS copied H264 payloads; independent host decode required\n");
#else
    if(!code)std::printf("ENCODER PASS copied H265 payloads; independent host decode required\n");
#endif
    return code;
}
#endif
int main(int argc,char **argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc==1) { std::printf("RESULT PASS layout only SensorPad=%zu SensorPlane=%zu IspChannel=%zu SclPort=%zu H26xChannel=%zu\n",sizeof(ca_zt30::SensorPad),sizeof(ca_zt30::SensorPlane),sizeof(ca_zt30::IspChannel),sizeof(ca_zt30::SclPort),sizeof(ca_zt30::H26xChannel));return 0; }
#ifdef ZT30_H264_WEB_PROBE
    const char *activation="--activate-owned-h264-web";
#elif defined(ZT30_STREAM_PROBE)
    const char *activation="--activate-owned-stream";
#elif defined(ZT30_ENCODER_PROBE)
    const char *activation="--activate-owned-encoder";
#else
    const char *activation="--activate-owned-rgb";
#endif
    bool observe_vui=false,native_media=false,wide=false,graph_only=false;
#if defined(ZT30_H264_WEB_PROBE) && !defined(ZT30_NATIVE_MEDIA_PROBE)
    wide=argc>=2 && !std::strcmp(argv[1],"--activate-owned-wide-h264-web");
    if(wide)activation="--activate-owned-wide-h264-web";
#elif !defined(ZT30_ENCODER_PROBE)
    wide=argc>=2 && !std::strcmp(argv[1],"--activate-owned-wide-rgb");
    graph_only=wide && argc==3 && !std::strcmp(argv[2],"--graph-only");
    if(wide)activation="--activate-owned-wide-rgb";
#elif !defined(ZT30_STREAM_PROBE)
    // Standalone HEVC qualification only; streaming/web/full-app remain zoom.
    wide=argc==2 && !std::strcmp(argv[1],"--activate-owned-wide-encoder");
    if(wide)activation="--activate-owned-wide-encoder";
#endif
#ifdef ZT30_H264_WEB_PROBE
    observe_vui=argc==3 && !std::strcmp(argv[2],"--observe-h264-vui");
    if(wide && !observe_vui)return 2; // Wide never writes H264 VUI.
#ifdef ZT30_NATIVE_MEDIA_PROBE
    native_media=argc==4 && !std::strcmp(argv[2],"--observe-h264-vui") && !std::strcmp(argv[3],"--native-media");
    if(native_media)observe_vui=true;
#endif
#endif
    if((argc!=2 && !observe_vui && !graph_only) || std::strcmp(argv[1],activation))return 2;
#ifdef ZT30_NATIVE_MEDIA_PROBE
    if(!native_media)return 2;
#endif
    char marker[160]{},sampled[160]{},bundle[160]{};
    if(geteuid()!=0 || !ram_marker(marker)) { std::fprintf(stderr,"activation requires fixed RAM controller location\n");return 3; }
    std::memcpy(sampled,marker,sizeof(sampled));
    char *name=std::strrchr(sampled,'/');
    if(!name || size_t(name+1-sampled)+sizeof("sampled.ready")>sizeof(sampled))return 3;
    std::memcpy(name+1,"sampled.ready",sizeof("sampled.ready"));
    // Stale markers cannot authorize IQ loading or completion in a new run.
    struct stat st{};
    if(!lstat(marker,&st) || errno!=ENOENT)return 3;
    if(!lstat(sampled,&st) || errno!=ENOENT)return 3;
#ifdef ZT30_ENCODER_PROBE
    if(wide) {
        char consumer[160]{};
        std::memcpy(consumer,marker,sizeof(consumer));
        char *leaf=std::strrchr(consumer,'/');
        if(!leaf || size_t(leaf+1-consumer)+sizeof("consumer.sampled")>sizeof(consumer))return 3;
        std::memcpy(leaf+1,"consumer.sampled",sizeof("consumer.sampled"));
        if(!lstat(consumer,&st) || errno!=ENOENT)return 3;
    }
#endif
    std::memcpy(bundle,marker,sizeof(bundle));
    name=std::strrchr(bundle,'/');
    if(!name || size_t(name+1-bundle)+sizeof("rgb-sdk.so")>sizeof(bundle))return 3;
    std::memcpy(name+1,"rgb-sdk.so",sizeof("rgb-sdk.so"));
    signal(SIGTERM,stop);signal(SIGINT,stop);
#ifdef ZT30_ENCODER_PROBE
    // Allocate before any SDK constructor or hardware activation.
    std::unique_ptr<uint8_t[]> encoder_buffer(new(std::nothrow) uint8_t[4*1024*1024]);
    if(!encoder_buffer)return 20;
#endif
    ca_zt30::NativeSdk sdk;
    if(!sdk.open(bundle))_exit(4);
#ifdef ZT30_ENCODER_PROBE
#ifdef ZT30_H264_WEB_PROBE
    if(!(observe_vui?sdk.resolve_h264_getter():sdk.resolve_h264_timing()))_exit(10);
#endif
    if(!sdk.resolve_encoder())_exit(4);
#ifdef ZT30_STREAM_PROBE
    if(!sdk.resolve_clock())_exit(4);
#endif
#endif
#ifdef ZT30_NATIVE_MEDIA_PROBE
    const int native_code=capture_native_media(sdk,marker,encoder_buffer.get());
    if(!native_code)std::printf("RESULT PASS owned zoom RGB IQ H264 RTSP web capture and unwind; no UART/USB\n");
    return native_code;
#else
    (void)native_media;
#endif
    ca_zt30::RgbOwner rgb(sdk.api);
    const uint64_t begin=monotonic_ns();
    const int opened=rgb.open(wide);
    if(opened) { std::fprintf(stderr,"OPEN FAIL raw=%d call=%s\n",opened,rgb.failed_call?rgb.failed_call:"unknown");_exit(5); }
    const auto &path=wide?ca_zt30::WidePath:ca_zt30::ZoomPath;
    std::printf("BIND_READY pid=%ld elapsed_ns=%llu sensor=%u vif=%u isp=%u scl=%u port=1 size=1280x720%s\n",long(getpid()),static_cast<unsigned long long>(monotonic_ns()-begin),path.sensor_pad,path.vif_device,path.isp_channel,path.scl_channel,graph_only?" graph_only=1":"");
    // Host requires independently parsed, advancing hardware frame counters.
    const uint64_t pre_window=wide?15000000000ULL:10000000000ULL;
#ifdef ZT30_ENCODER_PROBE
#ifdef ZT30_H264_WEB_PROBE
    const uint64_t deadline=monotonic_ns()+(wide?8000000000ULL:10000000000ULL);
#else
    const uint64_t deadline=monotonic_ns()+10000000000ULL;
#endif
    (void)pre_window;
#else
    const uint64_t deadline=monotonic_ns()+(graph_only?25000000000ULL:pre_window);
#endif
    bool frames=false;
    while(!stopped && monotonic_ns()<deadline) {
        if(!lstat(graph_only?sampled:marker,&st)) {
            if(!S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_nlink!=1 || st.st_size!=0)break;
            frames=true;break;
        }
        if(errno!=ENOENT)break;
        pause_ms(20);
    }
    int code=0;
    if(!frames || stopped) { std::fprintf(stderr,"FRAME_GATE FAIL stopped=%d\n",int(stopped));code=6; }
    else if(graph_only)std::printf("GRAPH_ONLY PASS IQ not explicitly loaded; no frame consumer\n");
    else {
        if(wide)std::printf("IQ_LOAD sensor=%u isp=%u path=%s key=1234\n",path.sensor_pad,path.isp_channel,path.iq_file);
#ifdef ZT30_H264_WEB_PROBE
        const uint64_t iq_begin=monotonic_ns();
#endif
        const int iq=rgb.load_iq();
        if(wide)std::printf("IQ_REPLY isp=%u raw=%d\n",path.isp_channel,iq);
#ifdef ZT30_H264_WEB_PROBE
        if(wide)std::printf("IQ_TIMING elapsed_ns=%llu\n",static_cast<unsigned long long>(monotonic_ns()-iq_begin));
#endif
        // Failed IQ initialization can leave SDK callback state uncertain.
        // Let process teardown release ownership; never unload its code here.
        if(iq) { std::fprintf(stderr,"IQ FAIL raw=%d\n",iq);_exit(7); }
        else {
            std::printf("IQ PASS key=1234\n");
#ifdef ZT30_ENCODER_PROBE
#ifdef ZT30_H264_WEB_PROBE
            const uint64_t hold_deadline=monotonic_ns()+(wide?6000000000ULL:8000000000ULL);
#else
            const uint64_t hold_deadline=monotonic_ns()+8000000000ULL;
#endif
#else
            const uint64_t hold_deadline=monotonic_ns()+(wide?12000000000ULL:8000000000ULL);
#endif
            bool complete=false;
            while(!stopped && monotonic_ns()<hold_deadline) {
                if(!lstat(sampled,&st)) {
                    complete=S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 && st.st_size==0;
                    break;
                }
                if(errno!=ENOENT)break;
                pause_ms(20);
            }
            if(!complete || stopped) { std::fprintf(stderr,"POST_IQ_GATE FAIL\n");code=9; }
        }
    }
#ifdef ZT30_ENCODER_PROBE
    if(!code)code=capture_encoder(sdk,marker,encoder_buffer.get(),observe_vui,wide);
#endif
    const int closed=rgb.close();
    std::printf("CLOSE raw=%d elapsed_ns=%llu\n",closed,static_cast<unsigned long long>(monotonic_ns()-begin));
    // A failed SDK cleanup can leave callback threads alive. Do not unload
    // their code; process exit releases driver ownership for the watchdog.
    if(closed)_exit(8);
    if(stopped && !code)code=9;
    sdk.close();
#ifdef ZT30_H264_WEB_PROBE
    if(!code)std::printf("RESULT PASS owned %s RGB IQ H264 RTSP web capture and unwind; no UART/USB\n",wide?"wide":"zoom");
#elif defined(ZT30_STREAM_PROBE)
    if(!code)std::printf("RESULT PASS owned zoom RGB IQ H265 RTSP capture and unwind; no UART/USB\n");
#elif defined(ZT30_ENCODER_PROBE)
    if(!code)std::printf("RESULT PASS owned %s RGB IQ H265 capture and unwind; no UART/USB\n",wide?"wide":"zoom");
#else
    if(!code)std::printf("RESULT PASS owned %s RGB graph%s closed; no frame pointers/encoders/UART/USB\n",wide?"wide":"zoom",graph_only?" without explicit IQ load":" and IQ load");
#endif
    return code;
}
