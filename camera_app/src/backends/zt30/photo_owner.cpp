#include "photo_owner.h"
#include "camera_app/log.h"
#include "pts_clock.h"
#include <cerrno>
#include <cstring>
#include <new>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
namespace ca_zt30 {
static uint64_t now() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec);
}
[[noreturn]] static void failed(const char *call,int rc=0) {
    ca_log("ZT30 JPEG fatal call=%s raw=%d errno=%d",call,rc,errno);_exit(26);
}
// Parse the bounded SOF before publishing, so marker-only/corrupt packets or
// a different native output size cannot be reported as a successful photo.
static bool dimensions(const uint8_t *p,size_t bytes,unsigned width,unsigned height) {
    if(bytes<4 || p[0]!=0xff || p[1]!=0xd8 || p[bytes-2]!=0xff || p[bytes-1]!=0xd9)return false;
    size_t n=2;
    while(n+4<=bytes) {
        if(p[n++]!=0xff)return false;
        while(n<bytes && p[n]==0xff)++n;
        if(n>=bytes)return false;
        const unsigned marker=p[n++];
        if(marker==0xda || marker==0xd9)return false;
        const size_t length=(unsigned(p[n])<<8)|p[n+1];
        if(length<2 || length>bytes-n)return false;
        if(marker>=0xc0 && marker<=0xc3) {
            return length>=8 && p[n+2]==8 && ((unsigned(p[n+3])<<8)|p[n+4])==height &&
                ((unsigned(p[n+5])<<8)|p[n+6])==width;
        }
        n+=length;
    }
    return false;
}
int PhotoOwner::open(EncoderSource primary,bool (*gate)(void*,bool,unsigned),void *opaque) {
    if(opened || !gate)return -1;
    // Allocate before creating SDK owners or starting the thermal worker. Large
    // temporary mappings must not disappear during its before/Get/after proof.
    buffer.reset(new(std::nothrow) uint8_t[8U*1024*1024]);
    nv12.reset(new(std::nothrow) uint8_t[ThermalNv12Bytes]);
    values.reset(new(std::nothrow) uint16_t[ThermalPixelCount]);
    frame.reset(new(std::nothrow) uint8_t[ThermalPixelFrameBytes]);
    if(!buffer || !nv12 || !values || !frame) { errno=ENOMEM;return -1; }
    copied.data=buffer.get();copied.capacity=8U*1024*1024;
    // The first RGB path already owns SYS/SNR and device0 ISP/SCL.
    if(other.open(primary==EncoderSource::Zoom,true))failed(other.failed_call,other.raw_status);
    const unsigned secondary=primary==EncoderSource::Zoom?1:0;
    if(!gate(opaque,false,secondary))failed("secondary RGB frame gate");
    if(other.load_iq())failed(other.failed_call,other.raw_status);
    if(!gate(opaque,true,secondary))failed("secondary RGB post-IQ frame gate");
    for(unsigned ch=0;ch<2;ch++) {
        auto p=scl_port(ch?2560:3840,ch?1440:2160);
        if(int rc=s.api.scl_set_port(0,ch,0,&p))failed("JPEG SCL port configuration",rc);
        ports[ch]=true;
        if(int rc=s.api.scl_enable_port(0,ch,0))failed("JPEG SCL port enable",rc);
        Port port{Scl,0,ch,0};
        if(int rc=s.api.sys_depth(0,&port,0,4))failed("JPEG SCL buffer depth",rc);
    }
    if(zoom.open_jpeg(EncoderSource::Zoom,4,3840,2160,true))failed(zoom.failed_call,zoom.raw_status);
    if(wide.open_jpeg(EncoderSource::Wide,5,2560,1440,false))failed(wide.failed_call,wide.raw_status);
    if(thermal.open_jpeg())failed(thermal.failure(),thermal.status());
    if(thermal.start_once())failed("JPEG metadata probe start",thermal.status());
    const bool probed=thermal.probe_input_discarded();
    if(thermal.stop_once())failed("JPEG metadata probe stop",thermal.status());
    if(!probed)failed("JPEG input metadata/mapping probe");
    ca_log("ZT30 JPEG input lease qualified: discarded flag1, no pixel writes");
    opened=true;return 0;
}
bool PhotoOwner::jpeg(EncoderOwner &owner,unsigned width,unsigned height) {
    // First Query before StartRecvPic failed on this SDK. Each successful start
    // admits exactly one picture, consumed/released before stopping. After a
    // timeout never reuse this rig: a late queued frame could look like a new shot.
    if(owner.start_once())failed(owner.failed_call,owner.raw_status);
    const uint64_t deadline=now()+1000000000ULL;
    int result=0;
    do {
        result=owner.copy_frame(copied);
        if(result || now()>=deadline)break;
        usleep(5000);
    } while(true);
    uint32_t pending=0;
    if(result>0 && owner.pending_packs(pending))failed(owner.failed_call,owner.raw_status);
    if(owner.stop_once())failed(owner.failed_call,owner.raw_status);
    if(pending) { reusable=false;errno=EBUSY;return false; }
    if(result<0)failed(owner.failed_call,owner.raw_status);
    if(!result) { reusable=false;errno=ETIMEDOUT;return false; }
    if(!dimensions(copied.data,copied.bytes,width,height)) { errno=EBADMSG;return false; }
    return true;
}
bool PhotoOwner::thermal_jpeg(const uint8_t *nv12) {
    if(thermal.start_once())failed(thermal.failure(),thermal.status());
    uint64_t pts=0;
    if(s.current_pts(0,&pts))failed("JPEG input PTS");
    const auto submitted=thermal.submit(nv12,ThermalNv12Bytes,pts);
    if(submitted!=InputBufferResult::Submitted) {
        ca_log("ZT30 JPEG input result=%u get=%d put=%d",unsigned(submitted),thermal.get_status(),thermal.put_status());
        // Invalid/discarded input is released, but do not unload an ambiguous
        // SDK input lease or reuse an encoder which may have failed internally.
        failed("JPEG input submission");
    }
    const uint64_t deadline=now()+1000000000ULL;int result=0;
    do {
        result=thermal.copy_frame(copied);
        if(result || now()>=deadline)break;
        usleep(5000);
    } while(true);
    uint32_t pending=0;
    if(result>0 && thermal.pending_packs(pending))failed(thermal.failure(),thermal.status());
    if(thermal.stop_once())failed(thermal.failure(),thermal.status());
    if(pending) { reusable=false;errno=EBUSY;return false; }
    if(result<0)failed(thermal.failure(),thermal.status());
    if(!result) { reusable=false;errno=ETIMEDOUT;return false; }
    if(!dimensions(copied.data,copied.bytes,640,512)) { errno=EBADMSG;return false; }
    return true;
}
static int raw_sidecar(const char *path,const timespec &at) {
    char filename[4096],temporary[4096],json[768];
    if(std::snprintf(filename,sizeof(filename),"%s.json",path)>=int(sizeof(filename)) ||
       std::snprintf(temporary,sizeof(temporary),"%s.part",filename)>=int(sizeof(temporary))) { errno=ENAMETOOLONG;return -1; }
    const int bytes=std::snprintf(json,sizeof(json),
        "{\"schema\":\"apcg.thermal.v1\",\"width\":640,\"height\":512,\"pixel_format\":\"gray16le\","
        "\"bits_per_sample\":16,\"timestamp_source\":\"usb_receive\",\"capture_unix_us\":%llu,"
        "\"utc_valid\":%s,\"temperature_scale_k\":null,\"temperature_offset_k\":null,"
        "\"temperature_calibration\":\"unverified_sensor_counts\",\"rotation_deg\":0}\n",
        (unsigned long long)(uint64_t(at.tv_sec)*1000000+at.tv_nsec/1000),at.tv_sec>=1483228800?"true":"false");
    if(bytes<0 || bytes>=int(sizeof(json))) { errno=EOVERFLOW;return -1; }
    const int fd=::open(temporary,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return -1;
    int result=0;size_t offset=0;
    while(offset<size_t(bytes)) {
        const ssize_t n=write(fd,json+offset,size_t(bytes)-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0) { if(!n)errno=EIO;result=-1;break; }offset+=size_t(n);
    }
    if(!result && fsync(fd))result=-1;
    int saved=errno;if(::close(fd) && !result) { result=-1;saved=errno; }
    if(!result && rename(temporary,filename)) { result=-1;saved=errno; }
    if(result)unlink(temporary);errno=saved;return result;
}
int PhotoOwner::capture(const char *root,ca_photo_scope scope,const uint8_t *frame,size_t frame_bytes,
                         const timespec &at,ca_raw_thermal *raw) {
    std::lock_guard<std::mutex> guard(capture_lock);
    if(!available() || (!frame || frame_bytes!=ThermalPixelFrameBytes) || (scope!=CA_PHOTO_SCOPE_ALL && scope!=CA_PHOTO_SCOPE_THERMAL)) { errno=EINVAL;return -1; }
    if(thermal_uyvy_nv12(frame,frame_bytes,nv12.get(),640*512,640,nv12.get()+640*512,640*256,640)!=ThermalPixelResult::Ok ||
       thermal_be16(frame,frame_bytes,values.get(),640*512)!=ThermalPixelResult::Ok) { errno=EINVAL;return -1; }
    char path[4096];
    if(scope==CA_PHOTO_SCOPE_ALL) {
        timespec rgb_at{};clock_gettime(CLOCK_REALTIME,&rgb_at);
        if(!jpeg(zoom,3840,2160) || ca_still_write_jpeg(root,'Z',copied.data,copied.bytes,&rgb_at,path,sizeof(path)))return -1;
        ca_log("ZT30 Z JPEG saved: %s (%zu bytes)",path,copied.bytes);
        clock_gettime(CLOCK_REALTIME,&rgb_at);
        if(!jpeg(wide,2560,1440) || ca_still_write_jpeg(root,'C',copied.data,copied.bytes,&rgb_at,path,sizeof(path)))return -1;
        ca_log("ZT30 C JPEG saved: %s (%zu bytes)",path,copied.bytes);
    }
    if(!thermal_jpeg(nv12.get()) || ca_still_write_jpeg(root,'I',copied.data,copied.bytes,&at,path,sizeof(path)))return -1;
    ca_log("ZT30 I JPEG saved: %s (%zu bytes)",path,copied.bytes);
    if(ca_raw_thermal_write_at(raw,values.get(),640,512,&at))return -1;
    if(raw_sidecar(ca_raw_thermal_path(raw),at)) {
        const int saved=errno;unlink(ca_raw_thermal_path(raw));errno=saved;return -1;
    }
    ca_log("ZT30 raw thermal photo saved: %s",ca_raw_thermal_path(raw));
    return 0;
}
int PhotoOwner::capture_survey(const char *root,const ca_survey_request &request,ca_survey_result &result,
                               const std::atomic<uint64_t> &generation) {
    std::lock_guard<std::mutex> guard(capture_lock);
    if(!available() || request.lens<1 || request.lens>2) { errno=ENOTSUP;return -1; }
    if(generation.load()!=request.generation) { errno=ECANCELED;return -1; }
    if(now()/1000000>request.deadline_ms) { errno=ETIMEDOUT;return -1; }
    // Each request has its own clock mapping. The JPEG packet PTS identifies
    // the sensor frame; receipt time must not qualify a stale queued picture.
    PtsClock clock;PtsObservation sample;
    if(!observe_pts_clock(clock,[this](uint64_t *pts){return s.current_pts(0,pts);},now,sample)) {
        errno=EIO;return -1;
    }
    const bool wide_lens=request.lens==1;
    if(!jpeg(wide_lens?wide:zoom,wide_lens?2560:3840,wide_lens?1440:2160))return -1;
    uint64_t mapped=0;
    if(!copied.packets || !clock.frame(copied.pts[0],now()/1000,mapped)) { errno=EBADMSG;return -1; }
    result.frame_ms=mapped/1000;
    if(result.frame_ms<=request.eligible_ms || result.frame_ms>request.deadline_ms ||
       !ca_metadata_at(result.frame_ms,&result.pose)) { errno=ETIMEDOUT;return -1; }
    if(generation.load()!=request.generation) { errno=ECANCELED;return -1; }
    timespec wall{};clock_gettime(CLOCK_REALTIME,&wall);
    const uint64_t received=now()/1000;
    const uint64_t age_us=received>mapped?received-mapped:0;
    const uint64_t unix_us=uint64_t(wall.tv_sec)*1000000+wall.tv_nsec/1000-age_us;
    result.captured_at={time_t(unix_us/1000000),long(unix_us%1000000)*1000};
    const int rc=ca_still_write_jpeg(root,wide_lens?'C':'Z',copied.data,copied.bytes,&result.captured_at,
                                   result.path,sizeof(result.path),&result.pose);
    if(!rc)ca_log("ZT30 survey JPEG saved: %s (%zu bytes)",result.path,copied.bytes);
    return rc;
}
int PhotoOwner::close() {
    int error=0;
    for(int rc:{thermal.close(),wide.close(),zoom.close()})if(rc && !error)error=rc;
    for(unsigned ch=0;ch<2;ch++)if(ports[ch]) {
        const int rc=s.api.scl_disable_port(0,ch,0);if(rc && !error)error=rc;ports[ch]=false;
    }
    const int rc=other.close();if(rc && !error)error=rc;
    opened=false;return error;
}
}
