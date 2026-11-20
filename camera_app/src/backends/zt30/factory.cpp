/* Continuous native development backend. Shares the normal app, transport and
 * qualified RGB/encoder owners; no probe files, client gates or vendor fallback.
 * Configurable RGB and thermal streams, paired recordings and still capture;
 * the browser retains a separate 720p H264 RGB preview.
 */
#include "owned_media.h"
#include "zoom_feedback.h"
#include "thermal_file_test.h"
#include "thermal_live.h"
#include "photo_owner.h"
#include "isp.h"
#include "output.h"
#include "recording.h"
#include "overlay.h"
#include "media_graph.h"
#include "mi/native_sdk.h"
#include "pts_clock.h"
#include "camera_app/rtsp.h"
#include "camera_app/live_video_server.h"
#include "camera_app/video_metadata.h"
#include "camera_app/log.h"
#include "camera_app/raw_thermal.h"
#include <string>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace ca_zt30 {
static_assert(sizeof(uintptr_t)==4,"ZT30 native SDK requires ARM32");
static uint64_t monotonic_ns() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
static bool pointer_slot(const void *p,uint32_t *slot) {
    const auto address=reinterpret_cast<uintptr_t>(p);
    if(!address)return false;
    *slot=uint32_t(address);return true;
}
static bool copy_packet(uint32_t address,uint8_t *out,size_t length) {
    if(!address || !length || uint64_t(address)+length>UINT64_C(0x100000000))return false;
    FILE *maps=std::fopen("/proc/self/maps","r");if(!maps)return false;
    char line[512]{};bool valid=false;
    while(std::fgets(line,sizeof(line),maps)) {
        unsigned long first=0,last=0;char permission[5]{};
        if(std::sscanf(line,"%lx-%lx %4s",&first,&last,permission)==3 && permission[0]=='r' &&
           first<=address && uint64_t(address)+length<=last) { valid=true;break; }
    }
    std::fclose(maps);
    if(valid)std::memcpy(out,reinterpret_cast<const void*>(uintptr_t(address)),length);
    return valid;
}
static bool vendor_stopped() {
    // Development launcher must suspend/stop vendor owners first. Refuse a
    // concurrent vendor app before SDK constructors can open any MI devices.
    DIR *proc=opendir("/proc");if(!proc)return false;
    bool idle=true;
    while(auto *entry=readdir(proc)) {
        if(entry->d_name[0]<'0' || entry->d_name[0]>'9')continue;
        char path[320]{},name[512]{};
        std::snprintf(path,sizeof(path),"/proc/%s/exe",entry->d_name);
        const ssize_t n=readlink(path,name,sizeof(name)-1);
        if(n<0) { if(errno!=ENOENT && errno!=ESRCH) { idle=false;break; }continue; }
        if(n>=ssize_t(sizeof(name)-1)) { idle=false;break; }
        const char *base=std::strrchr(name,'/');base=base?base+1:name;
        if(!std::strncmp(base,"siyi_app",8)) { idle=false;break; }
        std::snprintf(path,sizeof(path),"/proc/%s/cmdline",entry->d_name);
        FILE *cmd=std::fopen(path,"r");
        if(!cmd) { if(errno!=ENOENT && errno!=ESRCH) { idle=false;break; }continue; }
        const size_t bytes=std::fread(name,1,sizeof(name)-1,cmd);
        const bool read_error=std::ferror(cmd);std::fclose(cmd);
        if(read_error) { idle=false;break; }
        for(size_t i=0;i<bytes;i++)if(!name[i])name[i]=' ';
        name[bytes]=0;
        if(std::strstr(name,"/customer/app_restore.sh")) {
            std::snprintf(path,sizeof(path),"/proc/%s/stat",entry->d_name);
            FILE *stat=std::fopen(path,"r");if(!stat) { idle=false;break; }
            const bool read=std::fgets(name,sizeof(name),stat)!=nullptr;std::fclose(stat);
            const char *end=read?std::strrchr(name,')'):nullptr;
            if(!end || end[1]!=' ' || (end[2]!='T' && end[2]!='t')) { idle=false;break; }
        }
    }
    closedir(proc);return idle;
}
struct Context {
    NativeSdk sdk; // context destroyed only after MediaBackend joins/unwinds
    Api &api=sdk.api;EncoderApi &encoder=sdk.encoder;
    ImageControls image{sdk.image};
    Overlay overlay{sdk};
    std::unique_ptr<VideoOutput> main_output,sub_output,record_output;
    Recording recordings;
    std::string record_root;
    std::atomic<unsigned> thermal_hfov_cd{0};
    bool external_main=false,external_sub=false;
    std::mutex route_lock;bool thermal_primary=false,waiting_key[2]{true,true};
    PacketMemory memory{pointer_slot,copy_packet};
    std::unique_ptr<uint8_t[]> buffer{new(std::nothrow) uint8_t[4U*1024*1024]};
    ca_rtsp *rtsp=nullptr;
    ca_live_video_server *live=nullptr;
    PtsClock clock;
    std::unique_ptr<ThermalFileTest> thermal_test;
    std::unique_ptr<ThermalLive> thermal_live;
    std::unique_ptr<PhotoOwner> photos;
    std::atomic<EncoderSource> primary_source{EncoderSource::Zoom};
    EncoderSource original_source=EncoderSource::Zoom;
    bool other_ports[5]{};
    ThermalStartBudget photo_budget;bool photo_opened=false;
    int owner_lock=-1;
    unsigned rtsp_port=8554;
    ca_config settings{};
    std::string capture_root;
    ca_raw_thermal_server *raw_server=nullptr;ca_raw_thermal *raw_photo=nullptr;
    ca_thermal_stream *raw_stream=nullptr;
    ~Context() {
        ca_thermal_stream_close(raw_stream);ca_raw_thermal_server_close(raw_server);ca_raw_thermal_close(raw_photo);
        ca_live_video_server_close(live);ca_rtsp_close(rtsp);
        sdk.close(); // keep cross-process ownership through SDK destruction
        if(owner_lock>=0)close(owner_lock);
    }
    bool lock_owner() {
        owner_lock=::open("/tmp/apcg-zt30-media.lock",O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        struct stat st{};
        return owner_lock>=0 && !fstat(owner_lock,&st) && S_ISREG(st.st_mode) &&
            !st.st_uid && st.st_nlink==1 && !(st.st_mode&0022) && !flock(owner_lock,LOCK_EX|LOCK_NB);
    }
    bool observe() {
        PtsObservation sample;
        const bool valid=observe_pts_clock(clock,[this](uint64_t *pts) { return sdk.current_pts(0,pts); },monotonic_ns,sample);
        if(!valid)ca_log("ZT30 presentation clock failed: raw=%d bracket_ns=%llu pts=%llu attempts=%u",
            sample.status,(unsigned long long)sample.bracket_ns,(unsigned long long)sample.pts,sample.attempts);
        return valid;
    }
    static bool isp_frames(uint64_t &frames,unsigned wanted_channel) {
        FILE *proc=std::fopen("/proc/mi_modules/mi_isp/mi_isp0","r");if(!proc)return false;
        char line[512]{};bool found=false;
        // Per-channel checkout counter, independently observed in mi_isp0.
        // Device-level FrameDoneCnt cannot qualify a second sensor.
        while(std::fgets(line,sizeof(line),proc)) {
            if(!std::strstr(line,"PreCnt") || !std::strstr(line,"checkout"))continue;
            for(unsigned row=0;row<32 && std::fgets(line,sizeof(line),proc);row++) {
                unsigned dev=0,ch=0;unsigned long long pre=0,enq=0,bar=0,in=0,out=0,deq=0,drop=0,ot=0;
                if(std::sscanf(line,"%u %u %llu %llu %llu %llu %llu %llu %llu %llu",
                    &dev,&ch,&pre,&enq,&bar,&in,&out,&deq,&drop,&ot)!=10)break;
                if(!dev && ch==wanted_channel) { frames=out;found=true;break; }
            }
            break;
        }
        std::fclose(proc);return found;
    }
    static bool gate(void *opaque,bool after_iq) {
        return gate_channel(opaque,after_iq,uint32_t(static_cast<Context*>(opaque)->primary_source.load()));
    }
    static bool gate_channel(void *opaque,bool after_iq,unsigned channel) {
        (void)opaque;
        uint64_t first=0;bool sampled=false;
        const uint64_t deadline=monotonic_ns()+4000000000ULL;
        while(monotonic_ns()<deadline) {
            uint64_t frames=0;
            if(isp_frames(frames,channel)) {
                if(sampled && frames>=first && frames-first>=3) {
                    ca_log("ZT30 ISP channel=%u frames ready %s IQ: %llu -> %llu",
                        channel,after_iq?"after":"before",(unsigned long long)first,(unsigned long long)frames);
                    return true;
                }
                if(!sampled) { first=frames;sampled=true; }
            }
            timespec delay{0,50000000};nanosleep(&delay,nullptr);
        }
        ca_log("ZT30 ISP frame startup timed out %s IQ",after_iq?"after":"before");
        return false;
    }
    static bool vui(void*,const char*,const H264Vui&) { return true; }
    static bool open_sink(void *opaque) {
        auto &c=*static_cast<Context*>(opaque);
        if(ca_live_video_server_open(&c.live,8555)) {
            ca_log("ZT30 web video listener failed: errno=%d",errno);return false;
        }
        if(ca_live_video_server_configure(c.live,0,1280,720,30,true)) {
            ca_log("ZT30 web video configuration failed: errno=%d",errno);return false;
        }
        if(ca_rtsp_open(&c.rtsp,c.rtsp_port,"video1",c.settings.main_codec,30)) {
            ca_log("ZT30 RTSP listener failed: port=%u errno=%d",c.rtsp_port,errno);return false;
        }
        unsigned id=0;
        if(ca_rtsp_add_video(c.rtsp,"video2",c.settings.sub_codec,15,&id) || id!=1 ||
           ca_live_video_server_configure(c.live,1,640,512,15,true))return false;
        ca_rtsp_add_config_aliases(c.rtsp,&c.settings);
        if(ca_rtsp_support_proxy(c.rtsp,&c.settings.support)) {
            ca_log("ZT30 video SupportProxy configuration failed: errno=%d",errno);return false;
        }
        return c.observe();
    }
    static bool frame(void *opaque,const EncodedFrame &f,uint64_t,uint64_t after) {
        auto &c=*static_cast<Context*>(opaque);
        if(!c.observe() || !f.packets)return false;
        uint64_t mapped=0;
        // Use receipt after the clock sample, not the earlier stream-copy call.
        if(!c.clock.frame(f.pts[0],monotonic_ns()/1000,mapped))return false;
        (void)after;
        for(unsigned i=1;i<f.packets;i++)if(f.pts[i]!=f.pts[0])return false;
        size_t offset=0,nal=0,length=0;bool key=false;
        while(ca_annexb_next(f.data,f.bytes,&offset,&nal,&length))
            if(length && (f.data[nal]&31)==5)key=true;
        // Browser preview stays 720p H264 independently from the RTSP format.
        std::lock_guard<std::mutex> g(c.route_lock);
        const unsigned stream=c.thermal_primary?1:0;
        if(!(stream?c.external_sub:c.external_main))c.push(stream,f,mapped,key);
        return !ca_live_video_server_publish(c.live,stream,f.data,f.bytes,mapped,key,0);
    }
    static bool main_frame(void *opaque,const EncodedFrame &frame,uint64_t pts,bool key) {
        auto &c=*static_cast<Context*>(opaque);
        std::lock_guard<std::mutex> g(c.route_lock);
        if(!c.thermal_primary)c.push(0,frame,pts,key);
        return true;
    }
    void push(unsigned stream,const EncodedFrame &f,uint64_t pts,bool key) {
        if(waiting_key[stream] && !key)return;
        waiting_key[stream]=false;
        (void)ca_rtsp_push_video_timed(rtsp,stream,f.data,f.bytes,key,0,pts);
    }
    static bool sub_frame(void *opaque,const EncodedFrame &f,uint64_t pts,bool key) {
        auto &c=*static_cast<Context*>(opaque);std::lock_guard<std::mutex> g(c.route_lock);
        if(c.thermal_primary)c.push(1,f,pts,key);
        return true;
    }
    static void thermal_video_frame(void *opaque,EncoderCodec codec,const EncodedFrame &f,uint64_t pts,bool key) {
        auto &c=*static_cast<Context*>(opaque);std::lock_guard<std::mutex> g(c.route_lock);
        const unsigned stream=c.thermal_primary?0:1;
        if((codec==EncoderCodec::H265?CA_VIDEO_H265:CA_VIDEO_H264)==(stream?c.settings.sub_codec:c.settings.main_codec))c.push(stream,f,pts,key);
        if(codec==EncoderCodec::H264)(void)ca_live_video_server_publish(c.live,stream,f.data,f.bytes,pts,key,0);
    }
    static bool recording_frame(void *opaque,const EncodedFrame &frame,uint64_t pts,bool key) {
        auto &c=*static_cast<Context*>(opaque);
        c.recordings.frame(0,frame,pts,key,zoom_feedback().hfov(c.primary_source==EncoderSource::Wide,monotonic_ns()/1000000));
        return true;
    }
    static void thermal_recording_frame(void *opaque,const EncodedFrame &frame,uint64_t pts,bool key) {
        auto &c=*static_cast<Context*>(opaque);
        c.recordings.frame(1,frame,pts,key,c.thermal_hfov_cd.load()*.01f);
    }
    static bool drain(void*) { return true; }
};
class Backend final : public OwnedMediaBackend<Context> {
public:
    explicit Backend(std::unique_ptr<Context> c):OwnedMediaBackend<Context>(std::move(c)) {}
    ~Backend() override { if(shutdown())_exit(25); }
    int shutdown() {
        if(context->thermal_test)context->thermal_test->join();
        if(context->thermal_live)context->thermal_live->join();
        if(context->sub_output && context->sub_output->close())return EOWNERDEAD;
        if(context->record_output && context->record_output->close())return EOWNERDEAD;
        if(context->main_output && context->main_output->close())return EOWNERDEAD;
        (void)context->recordings.close();
        if(stop_video())return EOWNERDEAD;
        if(context->overlay.close())return EOWNERDEAD;
        for(unsigned port=1;port<5;port++)if(context->other_ports[port]) {
            const unsigned ch=context->original_source==EncoderSource::Zoom?1:0;
            if(context->sdk.api.scl_disable_port(0,ch,port))return EOWNERDEAD;
            context->other_ports[port]=false;
        }
        if(context->photos && context->photos->close())return EOWNERDEAD;
        if(context->photo_opened) { (void)context->photo_budget.reset();context->photo_opened=false; }
        return MediaBackend::shutdown();
    }
    bool ready() const override {
        return MediaBackend::ready() && (!context->main_output || context->main_output->healthy()) &&
            (!context->sub_output || context->sub_output->healthy()) &&
            (!context->record_output || context->record_output->healthy());
    }
    int set_lens(enum ca_media_lens value) override {
        if(value!=CA_MEDIA_LENS_WIDE && value!=CA_MEDIA_LENS_ZOOM) { errno=EINVAL;return -1; }
        const auto selected=value==CA_MEDIA_LENS_WIDE?EncoderSource::Wide:EncoderSource::Zoom;
        const auto before=context->primary_source.load();
        if(selected==before)return set_thermal_main(false);
        if(!context->photo_opened) { errno=ENODATA;return -1; }
        int rc=select_source(selected);
        if(!rc && context->main_output)rc=context->main_output->select_source(selected);
        if(!rc && context->sub_output)rc=context->sub_output->select_source(selected);
        if(!rc && context->record_output)rc=context->record_output->select_source(selected);
        if(!rc)rc=context->overlay.select_source(unsigned(selected));
        if(!rc)rc=set_thermal_main(false);
        if(rc) {
            if(rc==EOWNERDEAD || select_source(before) ||
               (context->main_output && context->main_output->select_source(before)) ||
               (context->sub_output && context->sub_output->select_source(before)) ||
               (context->record_output && context->record_output->select_source(before)) ||
               context->overlay.select_source(unsigned(before)))_exit(28);
            errno=EIO;return -1;
        }
        context->primary_source.store(selected);
        ca_log("ZT30 RGB source selected: %s",value==CA_MEDIA_LENS_WIDE?"wide":"zoom");
        return 0;
    }
    int set_thermal_main(bool thermal) override {
        if(thermal && (!context->thermal_live || !context->thermal_live->ready())) { errno=ENODATA;return -1; }
        std::lock_guard<std::mutex> g(context->route_lock);
        if(thermal==context->thermal_primary)return 0;
        for(unsigned stream=0;stream<2;stream++) {
            const bool th=(stream==0)==thermal;
            if(ca_live_video_server_configure(context->live,stream,th?640:1280,th?512:720,th?15:30,true)) {
                const int error=errno;
                for(unsigned restore=0;restore<2;restore++) {
                    const bool was_thermal=(restore==0)==context->thermal_primary;
                    if(ca_live_video_server_configure(context->live,restore,was_thermal?640:1280,
                                                     was_thermal?512:720,was_thermal?15:30,true))_exit(28);
                    (void)ca_rtsp_set_frame_rate(context->rtsp,restore,was_thermal?15:30);
                    context->waiting_key[restore]=true;
                }
                errno=error;return -1;
            }
            (void)ca_rtsp_set_frame_rate(context->rtsp,stream,th?15:30);
            context->waiting_key[stream]=true;
        }
        context->thermal_primary=thermal;
        ca_log("ZT30 main source selected: %s",thermal?"thermal":"RGB");return 0;
    }
    bool thermal_main() const override { std::lock_guard<std::mutex> g(context->route_lock);return context->thermal_primary; }
    void periodic(uint64_t now_ms) override {
        (void)context->overlay.refresh(now_ms);
    }
    int set_recording(bool active) override {
        if(active && (!context->thermal_live || !context->thermal_live->ready())) { errno=ENODATA;return -1; }
        return context->recordings.set(active);
    }
    bool recording() const override { return context->recordings.active(); }
    const char *recording_path() const override { return context->recordings.path(); }
    bool photo_available() const override {
        // Both scopes include thermal; do not advertise working photos when
        // the required live source has failed or has never produced a frame.
        return context->photos && context->photo_opened && context->photos->available() &&
            context->thermal_live && context->thermal_live->ready();
    }
    int capture_photo(ca_photo_scope scope) override {
        if(!photo_available()) { errno=ENODATA;return -1; }
        auto *frame=context->photos->snapshot_buffer();timespec at{};
        if(!context->thermal_live->snapshot(frame,ThermalPixelFrameBytes,at))return -1;
        return context->photos->capture(context->capture_root.c_str(),scope,frame,ThermalPixelFrameBytes,at,context->raw_photo);
    }
    float survey_hfov(unsigned lens) const override {
        if(lens==0)return context->thermal_hfov_cd.load()*.01f;
        if(lens==1)return zoom_feedback().hfov(true,monotonic_ns()/1000000);
        if(lens==2)return zoom_feedback().hfov(false,monotonic_ns()/1000000);
        return 0;
    }
    bool survey_available(unsigned lens) const override {
        if(lens==0)return context->raw_stream && context->thermal_live && context->thermal_live->ready() && context->thermal_hfov_cd.load()>=100;
        return lens>=1 && lens<=2 && context->photo_opened && context->photos && context->photos->available();
    }
    int capture_survey(const ca_survey_request &request,ca_survey_result &result,
                       const std::atomic<uint64_t> &generation) override {
        if(!survey_available(request.lens)) { errno=ENOTSUP;return -1; }
        if(request.lens==0)return ca_thermal_stream_capture(context->raw_stream,context->capture_root.c_str(),request,result,generation);
        return context->photos->capture_survey(context->capture_root.c_str(),request,result,generation);
    }
    // SIYI/UniGCS use background module zoom even when wide is selected;
    // MAVLink selected-source geometry uses lens_zoom() and hfov().
    float zoom() const override { return zoom_feedback().get(monotonic_ns()/1000000); }
    float lens_zoom(enum ca_media_lens value) const override {
        return value==CA_MEDIA_LENS_WIDE ? 1 : value==CA_MEDIA_LENS_ZOOM ? zoom() : NAN;
    }
    float hfov(bool thermal) const override {
        return thermal ? context->thermal_hfov_cd.load()*.01f : zoom_feedback().hfov(lens()==CA_MEDIA_LENS_WIDE,monotonic_ns()/1000000);
    }
    unsigned frame_rate(bool thermal) const override {
        if(thermal)return context->thermal_live && context->thermal_live->ready()?15:0;
        return MediaBackend::frame_rate(false);
    }
    int apply_overlay(const ca_config *settings) override {
        if(!settings) { errno=EINVAL;return -1; }
        if(context->overlay.apply(*settings))return -1;
        if(context->thermal_live)context->thermal_live->overlay(settings->osd_cross,settings->osd_recording);
        return 0;
    }
    int apply_image(const ca_config *settings) override {
        return settings?context->image.apply(*settings):(errno=EINVAL,-1);
    }
    int exposure(unsigned lens,ca_exposure *sample) override {
        return context->image.exposure(lens,sample);
    }
    int get_thermal_palette(uint8_t *value) override {
        if(!value) { errno=EINVAL;return -1; }
        const auto palette=context->thermal_live?context->thermal_live->cached_palette():uint8_t(255);
        *value=palette==255?uint8_t(context->settings.thermal_palette):palette;
        return 0;
    }
    int set_thermal_palette(uint8_t value) override {
        if(!context->thermal_live) { errno=ENODATA;return -1; }
        return context->thermal_live->set_palette(value);
    }
    int configure_raw_thermal(const ca_config *settings) override {
        if(settings->thermal_hfov_cd && settings->thermal_hfov_cd<100) { errno=EINVAL;return -1; }
        if(ca_thermal_stream_configure(context->raw_stream,settings))return -1;
        context->thermal_hfov_cd.store(settings->thermal_hfov_cd);return 0;
    }
    int set_inverted(bool inverted) override { if(inverted) { errno=ENOTSUP;return -1; }return 0; }
    int start(EncoderSource source) {
        MediaHooks h{context.get(),nullptr,Context::gate,Context::open_sink,Context::vui,Context::frame,Context::drain};
        context->primary_source=source;context->original_source=source;
        unsigned main_width,main_height;
        ca_video_resolution_size(context->settings.main_resolution,&main_width,&main_height);
        context->external_main=main_width!=1280 || main_height!=720 || context->settings.main_codec!=CA_VIDEO_H264;
        ca_video_resolution_size(context->settings.sub_resolution,&main_width,&main_height);
        context->external_sub=main_width!=1280 || main_height!=720 || context->settings.sub_codec!=CA_VIDEO_H264;
        const int rc=initialize(h,context->buffer.get(),4U*1024*1024,source,MediaRun::Continuous,31);
        if(rc)return rc;
        if(ca_raw_thermal_open(&context->raw_photo,context->capture_root.c_str()))
            ca_log("ZT30 raw photo writer unavailable errno=%d",errno);
        if(ca_raw_thermal_server_open(&context->raw_server,context->capture_root.c_str(),7345,640,512))
            ca_log("ZT30 raw TCP unavailable errno=%d; video continues",errno);
        if(ca_thermal_stream_open(&context->raw_stream,context->rtsp_port,false,&context->settings))
            ca_log("ZT30 lossless stream unavailable errno=%d; video continues",errno);
        ca_raw_thermal_server_source_timeout(context->raw_server,1000);
        ca_thermal_stream_source_timeout(context->raw_stream,1000);
        const char *photos_off=std::getenv("CAMERA_APP_ZT30_PHOTOS_OFF");
        // A root-controlled persistent marker permits raw-only development
        // qualification without changing saved settings or the startup script.
        const bool disabled=(photos_off && !std::strcmp(photos_off,"1")) ||
            !access("/customer/camera-app/photos.disabled",F_OK);
        if(!disabled) {
            const char *directory="/tmp/apcg-zt30-photos";struct stat st{};
            const bool directory_ok=(!mkdir(directory,0700) || errno==EEXIST) &&
                !lstat(directory,&st) && S_ISDIR(st.st_mode) && !st.st_uid && (st.st_mode&0777)==0700;
            if(!context->raw_photo || !directory_ok || !context->photo_budget.begin("/tmp/apcg-zt30-photos/start-budget"))
                ca_log("ZT30 photos unavailable: startup budget/writer; RGB continues");
            else if(!context->sdk.resolve_jpeg() || !context->sdk.resolve_input())
                ca_log("ZT30 JPEG API unavailable; RGB continues");
            else {
                context->photos.reset(new(std::nothrow) PhotoOwner(context->sdk,context->memory));
                if(context->photos && !context->photos->open(source,Context::gate_channel,context.get()))
                    context->photo_opened=true;
            }
        }
        const unsigned image_channels=context->photo_opened?3U:(1U<<unsigned(source));
        if(context->image.initialize(image_channels,context->settings)) {
            ca_log("ZT30 initial image settings failed: errno=%d",errno);return EIO;
        }
        unsigned width,height;
        ca_video_resolution_size(context->settings.main_resolution,&width,&height);
        context->external_main=width!=1280 || height!=720 || context->settings.main_codec!=CA_VIDEO_H264;
        if(context->external_main) {
            context->main_output.reset(new(std::nothrow) VideoOutput(context->sdk,context->memory));
            const auto codec=context->settings.main_codec==CA_VIDEO_H265?EncoderCodec::H265:EncoderCodec::H264;
            if(!context->main_output || context->main_output->start(source,2,3,codec,width,height,1570000,Context::main_frame,context.get())) {
                ca_log("ZT30 main format initialization failed");return EIO;
            }
        }
        if(context->external_sub) {
            ca_video_resolution_size(context->settings.sub_resolution,&width,&height);
            context->sub_output.reset(new(std::nothrow) VideoOutput(context->sdk,context->memory));
            const auto codec=context->settings.sub_codec==CA_VIDEO_H265?EncoderCodec::H265:EncoderCodec::H264;
            if(!context->sub_output || context->sub_output->start(source,4,4,codec,width,height,1570000,Context::sub_frame,context.get()))return EIO;
        }
        ca_video_resolution_size(context->settings.recording_resolution,&width,&height);
        context->recordings.configure(context->record_root.c_str(),width,height,context->raw_stream);
        context->record_output.reset(new(std::nothrow) VideoOutput(context->sdk,context->memory));
        if(!context->record_output || context->record_output->start(source,3,7,EncoderCodec::H264,width,height,12000000,Context::recording_frame,context.get())) {
            ca_log("ZT30 recording encoder initialization failed");return EIO;
        }
        if(context->photo_opened) {
            const unsigned other=source==EncoderSource::Zoom?1:0;
            for(unsigned port=1;port<5;port++) {
                unsigned w=1280,h=720;
                if(port==2)ca_video_resolution_size(context->settings.main_resolution,&w,&h);
                if(port==3)ca_video_resolution_size(context->settings.recording_resolution,&w,&h);
                if(port==4)ca_video_resolution_size(context->settings.sub_resolution,&w,&h);
                // An unused main-format port need not consume scaler bandwidth.
                if(port==2 && !context->external_main)continue;
                if(port==4 && !context->external_sub)continue;
                auto attr=scl_port(uint16_t(w),uint16_t(h));
                if(context->sdk.api.scl_set_port(0,other,port,&attr))return EIO;
                context->other_ports[port]=true;
                if(context->sdk.api.scl_enable_port(0,other,port))return EIO;
                Port out{Scl,0,other,port};
                if(context->sdk.api.sys_depth(0,&out,0,4))return EIO;
            }
        }
        if(context->overlay.configure(unsigned(source),context->external_main,context->settings))return EIO;
        const char *test=std::getenv("CAMERA_APP_ZT30_THERMAL_TEST_INPUT");
        if(test && *test && context->sdk.resolve_input()) {
            context->thermal_test.reset(new(std::nothrow) ThermalFileTest(context->sdk,context->memory));
            if(!context->thermal_test || !context->thermal_test->start(test)) {
                ca_log("ZT30 thermal file test refused; RGB continues");context->thermal_test.reset();
            }
        }
        const char *live=std::getenv("CAMERA_APP_ZT30_THERMAL_RUN");
        const char *off=std::getenv("CAMERA_APP_ZT30_THERMAL_OFF");
        if((!test || !*test) && (!off || std::strcmp(off,"1")) && context->sdk.resolve_input()) {
            context->thermal_live.reset(new(std::nothrow) ThermalLive(context->sdk,context->memory,context->rtsp,context->live,context->raw_server,context->raw_stream));
            if(context->thermal_live) {
                context->thermal_live->set_frame_callback(Context::thermal_recording_frame,context.get());
                context->thermal_live->initial_palette(context->settings.thermal_palette);
                context->thermal_live->set_video_callback(Context::thermal_video_frame,context.get(),
                    context->settings.main_codec==CA_VIDEO_H265 || context->settings.sub_codec==CA_VIDEO_H265);
                context->thermal_live->overlay(context->settings.osd_cross,context->settings.osd_recording);
            }
            if(!context->thermal_live || !context->thermal_live->start(live && *live?live:nullptr)) {
                ca_log("ZT30 live thermal refused; RGB continues");context->thermal_live.reset();
            }
        }
        return 0;
    }
};
}
std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &config) {
    using namespace ca_zt30;
    if(!config.backend || std::strcmp(config.backend,"zt30")) { errno=EINVAL;return nullptr; }
    if(!vendor_stopped()) { ca_log("ZT30 vendor application still owns media; refusing startup");errno=EBUSY;return nullptr; }
    const char *selected=std::getenv("CAMERA_APP_ZT30_SOURCE");
    if(selected && std::strcmp(selected,"zoom") && std::strcmp(selected,"wide")) { errno=EINVAL;return nullptr; }
    const EncoderSource source=selected && !std::strcmp(selected,"wide")?EncoderSource::Wide:EncoderSource::Zoom;
    auto c=std::unique_ptr<Context>(new(std::nothrow) Context);
    if(!c || !c->buffer) { errno=ENOMEM;return nullptr; }
    if(!c->lock_owner()) { ca_log("Another AP media owner is active or lock is invalid");errno=EBUSY;return nullptr; }
    if(!config.capture_root || !*config.capture_root) { errno=EINVAL;return nullptr; }
    if(!config.record_root || !*config.record_root) { errno=EINVAL;return nullptr; }
    c->capture_root=config.capture_root;c->record_root=config.record_root;
    c->settings=config.settings;c->thermal_hfov_cd.store(config.settings.thermal_hfov_cd);c->rtsp_port=config.rtsp_port;
    const char *sdk=std::getenv("CAMERA_APP_ZT30_SDK");
    if(!sdk || !*sdk)sdk="/customer/camera-app/encoder-sdk.so";
    // Partial dlopen/init can retain SDK callbacks: terminate without dlclose.
    if(!c->sdk.open(sdk) || !c->sdk.resolve_encoder() || !c->sdk.resolve_h264_getter() || !c->sdk.resolve_clock() || !c->sdk.resolve_image() || !c->sdk.resolve_rgn())_exit(4);
    auto backend=std::unique_ptr<Backend>(new(std::nothrow) Backend(std::move(c)));
    if(!backend)_exit(20);
    if(backend->start(source)) {
        ca_log("ZT30 media startup failed: %s raw=%d",backend->failed_call()?backend->failed_call():"unknown",backend->raw_status());
        if(backend->shutdown())_exit(25);
        return nullptr;
    }
    ca_log("ZT30 continuous RGB ready: source=%s main=%s/%s recording=%s; 720p H264 browser preview and optional live thermal",
        source==EncoderSource::Wide?"wide":"zoom",ca_video_resolution_name(config.settings.main_resolution),
        ca_video_codec_name(config.settings.main_codec),ca_video_resolution_name(config.settings.recording_resolution));
    return std::unique_ptr<APC_Media_Backend>(backend.release());
}
