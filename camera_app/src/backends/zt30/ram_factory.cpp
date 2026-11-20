/* Explicit bounded RAM full-app factory. Package activation remains guarded.
 * Reuses the qualified diagnostic transport hooks; no actuator integration. */
#include "ram_app.h"
#include "owned_media.h"
#include "mi/native_sdk.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <cstdlib>
static_assert(sizeof(uintptr_t)==4,"ZT30 RAM factory requires ARM32 ABI");
static const volatile sig_atomic_t *ram_stopping;
static void (*ram_restore_signals)();
#define stopped (*ram_stopping)
static uint64_t monotonic_ns() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec);
}
static void pause_ms(unsigned ms) {
    if(ram_restore_signals)ram_restore_signals();
    timespec t{time_t(ms/1000),long(ms%1000)*1000000};
    while(!stopped && nanosleep(&t,&t)<0 && errno==EINTR) {}
}
static bool packet_slot(const void *p,uint32_t *slot) {
    const uintptr_t value=reinterpret_cast<uintptr_t>(p);if(!value)return false;*slot=uint32_t(value);return true;
}
static bool copy_packet(uint32_t address,uint8_t *out,size_t bytes) {
    if(!address || !bytes || uint64_t(address)+bytes>UINT64_C(0x100000000))return false;
    FILE *maps=std::fopen("/proc/self/maps","r");if(!maps)return false;
    char line[512]{};bool readable=false;
    while(std::fgets(line,sizeof(line),maps)) {
        unsigned long first=0,last=0;char permissions[5]{};
        if(std::sscanf(line,"%lx-%lx %4s",&first,&last,permissions)==3 && permissions[0]=='r' &&
           first<=address && uint64_t(address)+bytes<=last) { readable=true;break; }
    }
    std::fclose(maps);if(readable)std::memcpy(out,reinterpret_cast<const void*>(uintptr_t(address)),bytes);return readable;
}
static bool write_all(int fd,const uint8_t *data,size_t bytes) {
    while(bytes && !stopped) { ssize_t n=write(fd,data,bytes);if(n<0 && errno==EINTR)continue;if(n<=0)return false;data+=n;bytes-=size_t(n); }
    return !bytes && !stopped;
}
// These hooks are the unchanged Stage12 qualified diagnostic wiring. The RAM
// factory intentionally remains separate from a continuous production backend.
#define ZT30_H264_WEB_PROBE
#define ZT30_RAM_FACTORY
#include "../../../../tools/zt30_stream_probe.h"
#include "../../../../tools/zt30_native_media_probe.h"
#undef stopped
namespace ca_zt30 {
static char session[128]{};
static bool prepared=false,used=false,closed=false;
static bool wide_source=false;
static uint64_t factory_begin=0;
static MediaBackend *active_media=nullptr;
static int final_result=1;
bool ram_app_prepare(int argc,const volatile sig_atomic_t *flag,void (*restore_signals)()) {
    factory_begin=monotonic_ns();
    if(std::getenv("LD_PRELOAD") && *std::getenv("LD_PRELOAD"))return false;
    if(argc!=1 || !flag || !restore_signals || getuid()!=0 || geteuid()!=0 || prepared)return false;
    const char *source=std::getenv("CAMERA_APP_ZT30_RAM_SOURCE");
    if(source && std::strcmp(source,"zoom") && std::strcmp(source,"wide"))return false;
    wide_source=source && !std::strcmp(source,"wide");
    const char *path=std::getenv("CAMERA_APP_ZT30_RAM_SESSION");
    const char prefix[]="/tmp/apcg-zt30.session.";
    if(!path || std::strlen(path)!=sizeof(prefix)-1+6 || std::strncmp(path,prefix,sizeof(prefix)-1))return false;
    for(const char *p=path+sizeof(prefix)-1;*p;p++)if(!((*p>='0'&&*p<='9')||(*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')))return false;
    struct stat st{};struct statfs fs{};
    if(lstat(path,&st) || !S_ISDIR(st.st_mode) || st.st_uid || (st.st_mode&0022) || statfs(path,&fs) || fs.f_type!=0x01021994)return false;
    char expected[160]{},exe[160]{};std::snprintf(expected,sizeof(expected),"%s/rgb-probe",path);
    ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
    if(n<0 || n>=ssize_t(sizeof(exe)-1) || std::strcmp(exe,expected) || lstat(expected,&st) || !S_ISREG(st.st_mode) || st.st_uid || st.st_nlink!=1 || (st.st_mode&0022))return false;
    std::snprintf(session,sizeof(session),"%s",path);
    for(const char *name:{"camera.ini","camera-app.ready","frames.ready","sampled.ready","stream.ready","stream.done","encoded.h264","encoded.jsonl","full-app.checked","consumer.sampled"}) {
        char file[160]{};std::snprintf(file,sizeof(file),"%s/%s",session,name);
        if(!lstat(file,&st) || errno!=ENOENT)return false;
    }
    const char *envs[]={"CAMERA_APP_CONFIG","CAMERA_APP_RECORD_ROOT","CAMERA_APP_CAPTURE_ROOT","CAMERA_APP_LOG_ROOT","CAMERA_APP_READY_PATH"};
    const char *names[]={"camera.ini","record","capture","logs","camera-app.ready"};
    for(unsigned i=0;i<5;i++) {
        char file[160]{};std::snprintf(file,sizeof(file),"%s/%s",session,names[i]);
        if(i>0 && i<4 && mkdir(file,0700))return false;
        if(setenv(envs[i],file,1))return false;
    }
    if(setenv("CAMERA_APP_BACKEND","zt30",1) || setenv("CAMERA_APP_PORT","37360",1) ||
       setenv("CAMERA_APP_MAVLINK_TCP_PORT","37361",1) || setenv("CAMERA_APP_MAVLINK_UDP_PORT","37361",1) ||
       setenv("CAMERA_APP_RTSP_PORT","18554",1) || unsetenv("CAMERA_APP_UNIGCS_PORT") || unsetenv("CAMERA_APP_DISCOVERY_PORT") || unsetenv("CAMERA_APP_UART") ||
       unsetenv("CAMERA_APP_EXTERNAL_UART") || unsetenv("CAMERA_APP_Z1_NATIVE_HELPER"))return false;
    char ini[160]{};std::snprintf(ini,sizeof(ini),"%s/camera.ini",session);
    const int fd=::open(ini,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    const char defaults[]="[mavlink]\nsystem_id=1\ntcp_port=37361\nudp_port=37361\n";
    if(fd<0)return false;
    const ssize_t written=write(fd,defaults,sizeof(defaults)-1);const int rc=close(fd);
    if(written!=ssize_t(sizeof(defaults)-1) || rc)return false;
    ram_stopping=flag;ram_restore_signals=restore_signals;prepared=true;return true;
}
void ram_app_configure(ca_config &c) {
    // Ignore any inherited or supplied configuration for this one experiment.
    ca_config_defaults(&c);c.mavlink_system_id=1;c.mavlink_tcp_port=c.mavlink_udp_port=37361;
    c.main_resolution=c.sub_resolution=c.recording_resolution=CA_VIDEO_720P;
    c.position_targeting=false;c.osd_recording=false;
}
struct RamContext {
    NativeSdk sdk; // destroyed last, after consumer/encoder/RGB and sink
    Api &api=sdk.api;EncoderApi &encoder=sdk.encoder;
    PacketMemory memory{packet_slot,copy_packet};
    std::unique_ptr<uint8_t[]> buffer{new(std::nothrow) uint8_t[4U*1024*1024]};
    char marker[160]{};
    NativeMediaContext capture{};
    ~RamContext() {
        const int video=capture.fd>=0?close(capture.fd):0;
        const int meta=capture.meta?std::fclose(capture.meta):0;
        if(video || meta)_exit(23);
    }
    bool open() {
        if(!buffer)return false;
        std::snprintf(marker,sizeof(marker),"%s/frames.ready",session);
        char bundle[160]{};std::snprintf(bundle,sizeof(bundle),"%s/rgb-sdk.so",session);
        // Constructors can touch drivers. The outer exclusive ownership/runtime
        // gate must already have passed. On any loader error do not unload.
        if(!sdk.open(bundle) || !sdk.resolve_encoder() || !sdk.resolve_h264_getter() || !sdk.resolve_clock())_exit(4);
        capture.marker=marker;capture.sdk=&sdk;capture.begin=monotonic_ns();capture.wide=wide_source;capture.full_app=true;
        if(wide_source)std::printf("RAM_FACTORY_TIMING phase=sdk-ready elapsed_ns=%llu\n",static_cast<unsigned long long>(monotonic_ns()-factory_begin));
        std::snprintf(capture.sampled,sizeof(capture.sampled),"%s/sampled.ready",session);
        char video[160]{},meta[160]{};std::snprintf(video,sizeof(video),"%s/encoded.h264",session);std::snprintf(meta,sizeof(meta),"%s/encoded.jsonl",session);
        capture.fd=::open(video,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        const int fd=::open(meta,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(capture.fd<0 || fd<0 || !(capture.meta=fdopen(fd,"w")))_exit(20);
        return true;
    }
};
class RamBackend final : public OwnedMediaBackend<RamContext> {
public:
    explicit RamBackend(std::unique_ptr<RamContext> c):OwnedMediaBackend<RamContext>(std::move(c)) {}
    static bool gate(void *p,bool after) { ram_restore_signals();return NativeMediaContext::gate(p,after); }
    static bool drain(void *p) {
        if(!NativeMediaContext::drain(p))return false;
        if(static_cast<NativeMediaContext*>(p)->wide)return true; // checked in the shared6s hold
        char marker[160]{};std::snprintf(marker,sizeof(marker),"%s/full-app.checked",session);
        const uint64_t deadline=monotonic_ns()+2000000000ULL;
        while(!*ram_stopping && monotonic_ns()<deadline) {
            struct stat st{};
            if(!lstat(marker,&st))return S_ISREG(st.st_mode) && !st.st_uid && st.st_nlink==1 && !st.st_size;
            if(errno!=ENOENT)return false;
            pause_ms(20);
        }
        return false;
    }
    int start() {
        MediaHooks h{&context->capture,ram_stopping,gate,NativeMediaContext::open_sink,NativeMediaContext::vui,NativeMediaContext::frame,drain};
        std::puts("H264_VUI_POLICY read-only");
        if(initialize(h,context->buffer.get(),4U*1024*1024,wide_source?EncoderSource::Wide:EncoderSource::Zoom)) { const int rc=shutdown();_exit(rc && rc!=EOWNERDEAD?25:result()); }
        ram_restore_signals();active_media=this;
        std::puts("NATIVE_MEDIA READY tracking=0 recording=0 thermal_fps=0 visible_fps=30 hfov=0 zoom=unknown");
        if(wide_source)std::puts("NATIVE_MEDIA READY lens=wide");
        return 0;
    }
    ~RamBackend() override {
        const int rc=shutdown();final_result=result();active_media=nullptr;
        std::printf("ENCODER CLOSE raw=%d frames=%u bytes=%zu elapsed_ns=%llu\n",rc,frames(),size_t(bytes()),static_cast<unsigned long long>(monotonic_ns()-context->capture.begin));
        std::printf("CLOSE raw=%d elapsed_ns=%llu\n",rc,static_cast<unsigned long long>(monotonic_ns()-context->capture.begin));
        if(wide_source)std::printf("RAM_FACTORY_TIMING phase=closed elapsed_ns=%llu\n",static_cast<unsigned long long>(monotonic_ns()-factory_begin));
        if(rc)_exit(25);
        closed=true;
        if(!final_result)std::puts("NATIVE_MEDIA PASS joined consumer; copied/released buffers; unsupported controls");
    }
};
bool ram_media_finished() { return active_media && active_media->finished(); }
int ram_media_result() { return active_media?active_media->result():final_result; }
bool ram_media_closed() { return closed; }
bool ram_media_wide() { return wide_source; }
}
std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config) {
    using namespace ca_zt30;
    if(!prepared || used || std::strcmp(config.backend,"zt30") || config.rtsp_port!=18554) { errno=EPERM;return nullptr; }
    used=true;
    auto c=std::unique_ptr<RamContext>(new(std::nothrow) RamContext);
    if(!c || !c->open()) { errno=ENOMEM;return nullptr; }
    auto backend=std::unique_ptr<RamBackend>(new(std::nothrow) RamBackend(std::move(c)));
    if(!backend)_exit(20);
    backend->start();return std::unique_ptr<APC_Media_Backend>(backend.release());
}
