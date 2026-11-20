#pragma once
#include "camera_app/APC_Media_Backend.h"
#include "rgb_owner.h"
#include "encoder_owner.h"
#include <atomic>
#include <csignal>
#include <pthread.h>
namespace ca_zt30 {
// Qualification wiring. The caller owns the SDK, copied buffer and callbacks
// until shutdown joins the sole consumer and releases all hardware resources.
struct MediaHooks {
    void *opaque=nullptr;
    const volatile sig_atomic_t *stop=nullptr;
    bool (*gate)(void*,bool after_iq)=nullptr;
    bool (*open_sink)(void*)=nullptr;
    bool (*vui)(void*,const char*,const H264Vui&)=nullptr;
    bool (*frame)(void*,const EncodedFrame&,uint64_t before_ns,uint64_t after_ns)=nullptr;
    bool (*drain)(void*)=nullptr;
};
// The same resource owners support continuous app operation and bounded
// qualification. The default preserves the existing diagnostic contract.
enum class MediaRun { Qualification, Continuous };
class MediaBackend : public APC_Media_Backend {
public:
    MediaBackend(Api &api,EncoderApi &enc,PacketMemory &memory):rgb(api),encoder(api,enc,memory) {}
    ~MediaBackend() override;
    int initialize(const MediaHooks&,uint8_t *buffer,size_t capacity,EncoderSource source=EncoderSource::Zoom,MediaRun run=MediaRun::Qualification,unsigned scaler_mask=3);
    int stop_video();
    int select_source(EncoderSource selected);
    int shutdown(); // join first, then encoder/RGB; caller must not unload SDK on error
    bool finished() const { return done.load(); }
    int result() const { return worker_result.load(); }
    const char *failed_call() const { return failure_call.load(); }
    int raw_status() const { return failure_status.load(); }
    unsigned frames() const { return frame_count.load(); }
    uint64_t bytes() const { return byte_count.load(); }
    bool ready() const override { return active.load(); }
    int set_recording(bool) override { return unsupported(); }
    bool recording() const override { return false; }
    const char *recording_path() const override { return nullptr; }
    int set_zoom(float) override { return unsupported(); }
    float zoom() const override { return NAN; }
    float lens_zoom(enum ca_media_lens) const override { return NAN; }
    int set_lens_zoom(enum ca_media_lens,float) override { return unsupported(); }
    float hfov(bool) const override { return 0; } // physical zoom not known
    unsigned frame_rate(bool thermal) const override { return thermal?0:30; }
    int set_lens(enum ca_media_lens value) override { return value==lens()?0:unsupported(); }
    enum ca_media_lens lens() const override { return source==EncoderSource::Wide?CA_MEDIA_LENS_WIDE:CA_MEDIA_LENS_ZOOM; }
    int set_thermal_main(bool) override { return unsupported(); }
    bool thermal_main() const override { return false; }
    int set_side_by_side(bool) override { return unsupported(); }
    int autofocus(uint16_t,uint16_t) override { return unsupported(); }
    int manual_focus(int) override { return unsupported(); }
    int set_focus_percent(float) override { return unsupported(); }
    bool thermal_range(ca_thermal_range*) override { return false; }
    int capture_photo(ca_photo_scope) override { return unsupported(); }
    int get_thermal_gain(uint8_t*) override { return unsupported(); }
    int set_thermal_gain(uint8_t) override { return unsupported(); }
    int get_thermal_palette(uint8_t*) override { return unsupported(); }
    int set_thermal_palette(uint8_t) override { return unsupported(); }
    int set_inverted(bool) override { return unsupported(); }
    int exposure(unsigned,ca_exposure*) override { return unsupported(); }
    int apply_overlay(const ca_config*) override { return unsupported(); }
    int apply_image(const ca_config*) override { return unsupported(); }
    int configure_raw_thermal(const ca_config*) override { return unsupported(); }
private:
    static int unsupported() { errno=ENOTSUP;return -1; }
    static uint64_t now();
    bool stopping() const { return local_stop.load() || (hooks.stop && *hooks.stop); }
    void consume();
    int init_failed(int code);
    RgbOwner rgb;
    EncoderOwner encoder;
    MediaHooks hooks{};
    EncoderSource source=EncoderSource::Zoom;
    MediaRun run=MediaRun::Qualification;
    EncodedFrame copied{};
    pthread_t thread{};
    bool used=false,thread_started=false,unsafe_sdk_failure=false;
    int close_error=0;
    std::atomic<bool> active{false},done{false},local_stop{false};
    std::atomic<int> worker_result{0},failure_status{0};
    std::atomic<const char*> failure_call{nullptr};
    std::atomic<unsigned> frame_count{0};
    std::atomic<uint64_t> byte_count{0};
};
} // namespace ca_zt30
