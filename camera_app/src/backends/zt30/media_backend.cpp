#include "media_backend.h"
#include <cstdio>
#include <ctime>
#include <unistd.h>
namespace ca_zt30 {
uint64_t MediaBackend::now() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec);
}
MediaBackend::~MediaBackend() {
    // A failed cleanup can leave SDK callbacks alive. Do not return to an
    // owner which might unload that SDK; independent RAM watchdog recovers.
    if(shutdown())_exit(25);
}
int MediaBackend::init_failed(int code) {
    failure_call.store(encoder.failed_call?encoder.failed_call:rgb.failed_call?rgb.failed_call:"media startup callback");
    failure_status.store(encoder.failed_call?encoder.raw_status:rgb.failed_call?rgb.raw_status:-1);
    worker_result.store(code);done.store(true);active.store(false);
    (void)shutdown();errno=EIO;return -1;
}
int MediaBackend::initialize(const MediaHooks &h,uint8_t *buffer,size_t capacity,EncoderSource selected,MediaRun policy,unsigned scaler_mask) {
    if(used) { errno=EALREADY;return -1; }
    used=true;
    if((policy!=MediaRun::Qualification && policy!=MediaRun::Continuous) ||
       (selected!=EncoderSource::Zoom && selected!=EncoderSource::Wide) ||
       !buffer || capacity<4U*1024*1024 || !h.gate || !h.open_sink || !h.vui || !h.frame || !h.drain) {
        errno=EINVAL;return -1;
    }
    source=selected;run=policy;hooks=h;copied.data=buffer;copied.capacity=capacity;
    const bool wide=source==EncoderSource::Wide;
    if(rgb.open(wide,false,scaler_mask)) { unsafe_sdk_failure=true;return init_failed(5); }
    if(stopping() || !hooks.gate(hooks.opaque,false))return init_failed(6);
    if(wide)std::printf("IQ_LOAD sensor=1 isp=1 path=%s key=1234\n",WidePath.iq_file);
    const uint64_t iq_begin=now();const int iq=rgb.load_iq();
    if(wide)std::printf("IQ_REPLY isp=1 raw=%d\nIQ_TIMING elapsed_ns=%llu\n",iq,static_cast<unsigned long long>(now()-iq_begin));
    if(iq) { unsafe_sdk_failure=true;return init_failed(7); }
    if(stopping() || !hooks.gate(hooks.opaque,true))return init_failed(8);
    if(!hooks.open_sink(hooks.opaque))return init_failed(26);
    const int opened=encoder.open(EncoderCodec::H264,H264Timing::ReadOnly,source);
    unsafe_sdk_failure=opened!=0; // same process-recovery fence as the qualified probe
    if(encoder.have_default_vui() && !hooks.vui(hooks.opaque,"pre-start",encoder.default_vui()))return init_failed(21);
    if(opened)return init_failed(21);
    H264Vui started{};
    if(encoder.read_h264_vui(started) || !hooks.vui(hooks.opaque,"started",started))return init_failed(21);
    // Publish before pthread_create: an immediately exiting worker can clear
    // readiness without initialize subsequently resurrecting it.
    active.store(true);
    const int rc=pthread_create(&thread,nullptr,[](void *opaque)->void* {
        static_cast<MediaBackend*>(opaque)->consume();return nullptr;
    },this);
    if(rc) { (void)init_failed(29);errno=rc;return -1; }
    thread_started=true;return 0;
}
void MediaBackend::consume() {
    int code=0;unsigned count=0;uint64_t total=0;
    const bool bounded=run==MediaRun::Qualification;
    const uint64_t deadline=now()+6000000000ULL;
    uint64_t last_frame=now();
    while(!stopping() && (!bounded || (count<90 && now()<deadline))) {
        const uint64_t before=now();const int got=encoder.copy_frame(copied);const uint64_t after=now();
        if(got<0) { code=22;break; }
        if(!got) {
            // A dead encoder must clear readiness and let the app supervisor
            // restart it. A healthy unattended stream has no frame-count cap.
            if(!bounded && after-last_frame>2000000000ULL) { code=24;break; }
            timespec delay{0,5000000};nanosleep(&delay,nullptr);continue;
        }
        if(!count) {
            H264Vui first{};
            if(encoder.read_h264_vui(first) || !hooks.vui(hooks.opaque,"first-frame",first)) { code=22;break; }
        }
        if((bounded && copied.bytes>32U*1024*1024-total) || !hooks.frame(hooks.opaque,copied,before,after)) { code=27;break; }
        last_frame=after;total+=copied.bytes;count++;frame_count.store(count);byte_count.store(total);
    }
    if(!code && bounded && (stopping() || count!=90))code=24;
    if(!code && !hooks.drain(hooks.opaque))code=28;
    if(code) { failure_call.store(encoder.failed_call?encoder.failed_call:"media sink/deadline");failure_status.store(encoder.failed_call?encoder.raw_status:-1); }
    worker_result.store(code);active.store(false);done.store(true);
}
int MediaBackend::select_source(EncoderSource selected) {
    const int rc=encoder.rebind(selected);
    if(!rc)source=selected;
    return rc;
}
int MediaBackend::shutdown() {
    const int stopped=stop_video();if(stopped)return stopped;
    const int raw=rgb.close();if(raw && !close_error)close_error=raw;
    return close_error;
}
int MediaBackend::stop_video() {
    active.store(false);local_stop.store(true);
    // Failed RGB/IQ/encoder initialization can leave SDK callbacks or mutexes
    // alive after attempted unwind. Recovery belongs to the independent
    // watchdog; never continue RGB/SYS teardown or unload the SDK.
    if(unsafe_sdk_failure)return EOWNERDEAD;
    if(thread_started) {
        const int rc=pthread_join(thread,nullptr);
        if(rc) { if(!close_error)close_error=rc;return close_error; }
        thread_started=false;
    }
    const int enc=encoder.close();if(enc && !close_error)close_error=enc;
    return close_error;
}
} // namespace ca_zt30
