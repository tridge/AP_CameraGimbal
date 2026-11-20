/* Fault injection through the public media API: overlay errors must not mask
 * control errors, and a failed pipeline replacement must restore its owner. */
#include "../src/media/media.cpp"
#include "test_media_backend.h"
#include <cassert>
#include <initializer_list>
static bool overlay_live, recording_active, fail_open;
static unsigned closed, opened, log_count, recording_calls;
static int recording_error;
static int control_result, overlay_result;
static bool combined, thermal;
static float active_hfov=50;
static enum ca_media_lens selected=CA_MEDIA_LENS_WIDE;
void ca_log(const char *, ...) { log_count++; }
bool ca_binlog_active() { return false; }
uint64_t ca_binlog_time_us() { return 0; }
void ca_binlog_emit(uint8_t, const void *, size_t) {}
static int control() { overlay_live=false; errno=EIO; return control_result; }
bool ca_config_image_equal(const ca_config *a, const ca_config *b)
{ return a->brightness == b->brightness; }
class OverlayBackend final : public TestMediaBackend {
public:
    ~OverlayBackend() override { closed++; }
    bool recording() const override { return recording_active; }
    int set_recording(bool active) override {
        recording_calls++;
        if(active && recording_error) { errno=recording_error;return -1; }
        recording_active=active;return 0;
    }
    int apply_overlay(const ca_config *) override
    { overlay_live=overlay_result==0; errno=ENOSPC; return overlay_result; }
    bool tracking_available() const override { return true; }
    float hfov(bool) const override { return active_hfov; }
    bool side_by_side() const override { return combined; }
    int set_side_by_side(bool enable) override { combined=enable; return 0; }
    enum ca_media_lens lens() const override { return selected; }
    bool thermal_main() const override { return thermal; }
    int set_zoom(float z) override {
        assert(!combined); // VENC must not have two producers during rebinding.
        if(control_result==0) { selected=apcam_uses_zoom_lens(z)?CA_MEDIA_LENS_ZOOM:CA_MEDIA_LENS_WIDE; thermal=false; }
        return control();
    }
    int set_lens(enum ca_media_lens lens) override {
        assert(!combined);
        if(control_result==0) { selected=lens; thermal=false; }
        return control();
    }
    int set_thermal_main(bool) override { return control(); }
};
std::unique_ptr<APC_Media_Backend> APC_Media_Backend::create(const ca_media_config &)
{
    opened++;
    if (fail_open) { fail_open=false; errno=ENOMEM; return nullptr; }
    return std::unique_ptr<APC_Media_Backend>(new OverlayBackend);
}
int main()
{
    ca_media_config config {};
    ca_media *media=nullptr;
    overlay_result=-1;
    assert(ca_media_open(&media,&config)==0);
    assert(media && !overlay_live && log_count==1 && closed==0);
    ca_media_close(media);
    assert(closed==1);
    overlay_result=0;
    assert(ca_media_open(&media,&config)==0);
    for (control_result=-1;control_result<=0;control_result++) {
        for (overlay_result=-1;overlay_result<=0;overlay_result++) {
            for (unsigned op=0;op<3;op++) {
                int result=op==0 ? ca_media_set_zoom(media,2) :
                    op==1 ? ca_media_set_lens(media,CA_MEDIA_LENS_WIDE) :
                            ca_media_set_thermal_main(media,true);
                assert(result==control_result);
                assert(overlay_live==(overlay_result==0));
                if (control_result<0) assert(errno==EIO);
            }
        }
    }
    control_result=overlay_result=0;
    const ca_tracking_rect rect {.1f,.1f,.3f,.3f};
    for (float invalid : {0.0f, NAN, INFINITY, -1.0f}) {
        active_hfov=invalid;
        assert(!ca_media_tracking_available(media));
        assert(ca_media_tracking_start(media,rect,CA_TRACK_OWNER_MAVLINK)<0 && errno==ENOTSUP);
        assert(ca_media_tracking_status(media).state==CA_TRACK_IDLE);
    }
    active_hfov=50;
    assert(ca_media_tracking_available(media));
    assert(ca_media_tracking_start(media,rect,CA_TRACK_OWNER_MAVLINK)==0);
    assert(ca_media_set_zoom(media,2)==0);
    assert(ca_media_tracking_status(media).state==CA_TRACK_ACQUIRING);
    assert(ca_media_set_zoom(media,5)==0);
    assert(ca_media_tracking_status(media).state==CA_TRACK_IDLE);
    assert(ca_media_tracking_start(media,rect,CA_TRACK_OWNER_MAVLINK)==0);
    assert(ca_media_set_zoom(media,1)==0);
    assert(ca_media_tracking_status(media).state==CA_TRACK_IDLE);
    thermal=true;
    assert(ca_media_tracking_start(media,rect,CA_TRACK_OWNER_MAVLINK)==0);
    assert(ca_media_set_zoom(media,1)==0);
    assert(!thermal && ca_media_tracking_status(media).state==CA_TRACK_IDLE);
    combined=true;
    assert(ca_media_set_zoom(media,5)==0 && !combined);
    for (auto lens : {CA_MEDIA_LENS_WIDE,CA_MEDIA_LENS_ZOOM}) {
        assert(ca_media_set_lens(media,lens)==0);
        assert(ca_media_tracking_start(media,rect,CA_TRACK_OWNER_MAVLINK)==0);
        assert(ca_media_set_lens(media,lens)==0);
        assert(ca_media_tracking_status(media).state==CA_TRACK_ACQUIRING);
        for (bool side_by_side : {false,true}) {
            thermal=true; combined=side_by_side;
            if(!side_by_side)
                assert(ca_media_tracking_start(media,rect,CA_TRACK_OWNER_MAVLINK)==0);
            assert(ca_media_set_lens(media,lens)==0);
            assert(!thermal && !combined && selected==lens);
            assert(ca_media_tracking_status(media).state==CA_TRACK_IDLE);
        }
        // Combined RGB view must also stop before reselecting its RGB lens.
        combined=true;
        assert(ca_media_set_lens(media,lens)==0 && !combined);
    }
    ca_config next=config.settings;
    next.main_resolution=CA_VIDEO_1080P;
    assert(next.main_resolution!=config.settings.main_resolution);
    const unsigned initial_opens=opened;
    assert(ca_media_set_recording(media,true)==0);
    assert(ca_media_configure(media,&next)<0 && errno==EBUSY);
    assert(opened==initial_opens);
    assert(ca_media_set_recording(media,false)==0);
    fail_open=true;
    assert(ca_media_configure(media,&next)<0 && errno==ENOMEM);
    assert(opened==initial_opens+2 && ca_media_ready(media));
    assert(ca_media_settings(media)->main_resolution==config.settings.main_resolution);
    assert(ca_media_configure(media,&next)==0);
    assert(ca_media_settings(media)->main_resolution==next.main_resolution);
    ca_media_close(media);
    assert(closed==opened-1); // The failed open never acquired a backend.
    config.settings.autorecord=CA_AUTORECORD_ENABLED;
    assert(ca_media_open(&media,&config)==0);
    recording_error=ENODATA;
    const auto calls=recording_calls;
    ca_media_defer_autorecord(media);
    media->periodic(1000);media->periodic(1200);
    assert(recording_calls==calls+1 && !recording_active);
    media->periodic(1500);
    assert(recording_calls==calls+2 && !recording_active);
    recording_error=0;media->periodic(2000);
    assert(recording_calls==calls+3 && recording_active);
    ca_media_set_recording(media,false);media->periodic(2500);
    assert(recording_calls==calls+4 && !recording_active);
    // A stop before readiness cancels startup intent too.
    recording_error=ENODATA;ca_media_defer_autorecord(media);media->periodic(3000);
    ca_media_set_recording(media,false);
    recording_error=0;media->periodic(3500);
    assert(recording_calls==calls+6 && !recording_active);
    // Storage and other permanent errors stop retries.
    recording_error=EIO;ca_media_defer_autorecord(media);media->periodic(4000);
    recording_error=0;media->periodic(4500);
    assert(recording_calls==calls+7 && !recording_active);
    recording_error=ENODATA;ca_media_defer_autorecord(media);media->periodic(5000);
    const auto attempts=recording_calls;
    media->periodic(65000);recording_error=0;media->periodic(65500);
    assert(recording_calls==attempts && !recording_active);
    ca_media_close(media);
    puts("PASS media controls, overlay errors, recording guard, rollback, lifetime and deferred autorecord");
}

// These media lifetime tests do not open a gimbal transport.
int ca_backend_set_gimbal_rates(ca_backend *, float, float) { return 0; }
