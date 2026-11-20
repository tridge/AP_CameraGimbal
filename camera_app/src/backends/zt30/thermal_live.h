#pragma once
#include "thermal_encoder.h"
#include "thermal_controls.h"
#include "thermal_start_budget.h"
#include "mi/native_sdk.h"
#include "usb_iso.h"
#include "uvc_frame.h"
#include "pts_clock.h"
#include "camera_app/overlay.h"
#include "camera_app/live_video_server.h"
#include "camera_app/rtsp.h"
#include "camera_app/raw_thermal_server.h"
#include "camera_app/thermal_stream.h"
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <pthread.h>
namespace ca_zt30 {
// USB only assembles/copies into a one-frame mailbox;
// the separate encoder worker converts/submits/publishes and drops stale input.
// RGB owns SYS/device0 for the entire lifetime. join() precedes RGB teardown.
class ThermalLive {
public:
    ThermalLive(NativeSdk&,PacketMemory&,ca_rtsp*,ca_live_video_server*,ca_raw_thermal_server* =nullptr,ca_thermal_stream* =nullptr);
    ~ThermalLive() { join(); }
    // nullptr selects ordinary startup; a name enables one-shot diagnostic capture.
    bool start(const char *run_name=nullptr);
    using FrameCallback=void(*)(void*,const EncodedFrame&,uint64_t,bool);
    void set_frame_callback(FrameCallback fn,void *p) { callback=fn;callback_opaque=p; } // Before start().
    uint8_t cached_palette() const { return controls.cached_palette(); }
    int palette(uint8_t *value) { return controls.palette(value); }
    int set_palette(uint8_t value) { return controls.set_palette(value); }
    void initial_palette(uint8_t value) { wanted_palette=value; } // Before start().
    void overlay(bool cross,bool recording) { overlay_options.store((cross?1U:0U)|(recording?2U:0U)); }
    using VideoCallback=void(*)(void*,EncoderCodec,const EncodedFrame&,uint64_t,bool);
    void set_video_callback(VideoCallback fn,void *p,bool hevc) { video_callback=fn;video_opaque=p;need_hevc=hevc; }
    bool ready() const;
    void join();
    // Same USB frame supplies display and raw plane; refuse stale/degraded data.
    bool snapshot(uint8_t *frame,size_t capacity,timespec &captured_at);
private:
    static uint64_t now(void* =nullptr);
    static void pause(void*,unsigned);
    static int ioctl_call(void*,int,unsigned long,void*);
    static int close_call(void*,int);
    static bool frame(void*,const uint8_t*,size_t,const UvcFrameInfo&);
    static bool packet(void*,const IsoPacket&);
    static int open_device();
    bool initialize(const char *name);
    void capture();
    void encode();
    [[noreturn]] void encoder_fatal(const char *call,int status=0);
    NativeSdk &sdk;ThermalEncoder encoder,clean_encoder,hevc_encoder;
    VideoCallback video_callback=nullptr;void *video_opaque=nullptr;bool need_hevc=false;
    ThermalControls controls{{nullptr,ioctl_call,close_call,now,pause}};
    uint8_t wanted_palette=0;
    std::atomic<unsigned> overlay_options{0};
    ca_overlay_bitmap cross_bits[CA_OVERLAY_REGIONS]{};
    FrameCallback callback=nullptr;void *callback_opaque=nullptr;
    ca_raw_thermal_server *raw_server;ca_thermal_stream *raw_stream;
    ca_rtsp *rtsp;ca_live_video_server *live;unsigned stream_id=0;
    std::unique_ptr<UsbIso> usb;
    UvcFrame assembler;
    std::unique_ptr<uint8_t[]> latest,owned,nv12,encoded,clean_encoded,hevc_encoded;
    std::unique_ptr<uint16_t[]> radiometric;
    std::mutex lock;uint64_t receipt=0,generation=0;timespec captured_at{};
    pthread_t start_thread{},usb_thread{},encode_thread{};
    bool start_started=false;
    char run_name[49]{};
    std::atomic<bool> usb_started{false};bool encode_started=false;
    std::atomic<bool> stop{false},usb_failed{false},degraded{false},usb_finished{false};
    PtsClock clock,clean_clock,hevc_clock;
    ThermalStartBudget budget;
    bool normal=false;
    std::atomic<uint64_t> published_ns{0};
    char raw_sample[128]{};
};
}
