#pragma once
#include "camera_app/APC_Media_Backend.h"

// Minimal backend for frontend lifetime and failure-injection tests.
class TestMediaBackend : public APC_Media_Backend {
public:
    bool ready() const override { return true; }
    int set_recording(bool ) override { return 0; }
    bool recording() const override { return false; }
    const char * recording_path() const override { return ""; }
    int set_zoom(float ) override { return 0; }
    float zoom() const override { return 0; }
    float hfov(bool ) const override { return 50; }
    unsigned frame_rate(bool ) const override { return 0; }
    int set_lens(enum ca_media_lens ) override { return 0; }
    enum ca_media_lens lens() const override { return CA_MEDIA_LENS_WIDE; }
    int set_thermal_main(bool ) override { return 0; }
    bool thermal_main() const override { return false; }
    int autofocus(uint16_t , uint16_t ) override { return 0; }
    int manual_focus(int ) override { return 0; }
    int set_focus_percent(float ) override { return 0; }
    bool thermal_range(struct ca_thermal_range *) override { return false; }
    int capture_photo(enum ca_photo_scope ) override { return 0; }
    int get_thermal_gain(uint8_t *value) override { *value=0; return 0; }
    int set_thermal_gain(uint8_t ) override { return 0; }
    int get_thermal_palette(uint8_t *value) override { *value=0; return 0; }
    int set_thermal_palette(uint8_t ) override { return 0; }
    int set_inverted(bool ) override { return 0; }
    int exposure(unsigned , struct ca_exposure *) override { return 0; }
    int apply_overlay(const struct ca_config *) override { return 0; }
    int apply_image(const struct ca_config *) override { return 0; }
};
