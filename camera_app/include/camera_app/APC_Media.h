#pragma once
#include "camera_app/APC_Media_Backend.h"
#include "apcam/APC_Camera.h"
#include "apcam/APC_Resource.h"
#include "camera_app/APC_Tracking.h"

// Stable owner used by protocol callbacks while the underlying image pipeline
// changes. Diagnostic workers are joined before replacement and destruction.
class APC_Media {
public:
    explicit APC_Media(const ca_media_config &config) : _config(config),
        _inverted(config.settings.orientation == CA_MOUNT_INVERTED) {}
    ~APC_Media();
    APC_Media(const APC_Media &) = delete;
    APC_Media &operator=(const APC_Media &) = delete;
    int initialize();
    int configure(const struct ca_config *settings);
    const struct ca_config * settings() const;
    bool ready() const;
    void defer_autorecord() { _autorecord_pending=true; _autorecord_next=0; _autorecord_deadline=0; }
    void periodic(uint64_t now_ms);
    int set_recording(bool active);
    bool recording() const;
    const char * recording_path() const;
    int set_zoom(float zoom);
    float zoom() const;
    int set_lens_zoom(enum ca_media_lens lens, float zoom);
    float lens_zoom(enum ca_media_lens lens) const;
    float hfov(bool thermal) const;
    unsigned frame_rate(bool thermal) const;
    int set_lens(enum ca_media_lens lens);
    enum ca_media_lens lens() const;
    int set_thermal_main(bool thermal_main);
    bool thermal_main() const;
    bool side_by_side() const { return _backend && _backend->side_by_side(); }
    int set_side_by_side(bool enabled);
    int autofocus(uint16_t x, uint16_t y);
    int manual_focus(int direction);
    int set_focus_percent(float percent);
    bool thermal_range(struct ca_thermal_range *range);
    bool photo_available() const;
    int capture_photo(enum ca_photo_scope scope);
    bool survey_available(unsigned lens) const;
    float survey_hfov(unsigned lens) const;
    bool survey_start(const ca_survey_request &request);
    bool survey_poll(ca_survey_result &result);
    void survey_cancel();
    void set_photo_prepare(int (*fn)(void*),void *opaque) { _photo_prepare=fn;_photo_opaque=opaque; }
    bool cached_thermal_controls(uint8_t *gain, uint8_t *palette);
    int get_thermal_gain(uint8_t *gain);
    int set_thermal_gain(uint8_t gain);
    int get_thermal_palette(uint8_t *palette);
    int set_thermal_palette(uint8_t palette);
    int set_inverted(bool inverted);
    APC_Tracking &tracking() { return _tracking; }
private:
    bool _autorecord_pending=false;
    uint64_t _autorecord_next=0, _autorecord_deadline=0;
    int (*_photo_prepare)(void*)=nullptr;void *_photo_opaque=nullptr;
    struct LiveControls;
    struct SurveyWorker;
    SurveyWorker *_survey = nullptr;
    ca_survey_result _survey_completion{};
    bool _survey_completed = false;
    void _stop_survey();
    int _open_backend(const ca_media_config *config);
    void _apply_overlay_after_control(const char *control);
    int _restore_controls(const LiveControls *state);
    void _start_controls_monitor();
    void _stop_controls_monitor();
    void _monitor_controls();
    void _monitor_exposure();
    const APC_Camera &_camera = APC_Camera::get_singleton();
    std::unique_ptr<APC_Media_Backend> _backend;
    APC_Tracking _tracking;
    ca_media_config _config {};
    bool _inverted = false;
    uint8_t _cached_gain = 0, _cached_palette = 0;
    bool _cached_thermal = false;
    APC_Mutex _controls_lock;
    APC_Condition _controls_wake, _exposure_wake;
    pthread_t _controls_thread {}, _exposure_thread {};
    bool _controls_running = false, _controls_stop = false, _exposure_running = false;
    unsigned _controls_generation = 0;
};
