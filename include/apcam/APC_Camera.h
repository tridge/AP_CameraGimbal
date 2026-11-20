#pragma once

#include "target.h"
#include "lens.h"
#include <cmath>
#include <cstddef>
#include <cstdint>

// Immutable lens capabilities. Zoom is relative to this lens, not to the
// vendor protocol's combined wide/tele zoom scale. Stream sizes are separate
// from sensor sizes and recording resolutions.
class APC_Lens {
public:
    enum class Type : uint8_t { RGB, Thermal };
    enum class FOV_Model : uint8_t { FocalLength, Endpoints };

    constexpr APC_Lens(const char *name, Type type, unsigned width, unsigned height,
                       float hfov, float optical_max, float digital_max,
                       FOV_Model model = FOV_Model::FocalLength, float tele_hfov = 0) :
        _name(name), _type(type), _width(width), _height(height), _hfov(hfov),
        _optical_max(optical_max), _digital_max(digital_max),
        _fov_model(model), _tele_hfov(tele_hfov) {}

    const char *name() const { return _name; }
    Type type() const { return _type; }
    unsigned width() const { return _width; }
    unsigned height() const { return _height; }
    bool has_zoom() const { return has_optical_zoom() || _digital_max > 1; }
    bool has_optical_zoom() const { return _optical_max > 1; }
    float optical_zoom_max() const { return _optical_max; }
    float digital_zoom_max() const { return _digital_max; }
    float get_FOV(float optical_zoom = 1, float digital_zoom = 1) const
    {
        float magnification = optical_zoom;
        if (_fov_model == FOV_Model::Endpoints) {
            if (!std::isfinite(optical_zoom) || optical_zoom < 1 ||
                optical_zoom > _optical_max) {
                return 0;
            }
            const float ratio = tanf(_hfov * DEG_TO_RAD * 0.5f) /
                                tanf(_tele_hfov * DEG_TO_RAD * 0.5f);
            magnification = 1 + (ratio - 1) * (optical_zoom - 1) / (_optical_max - 1);
        }
        return zoom_FOV(_hfov, magnification * digital_zoom);
    }

    static float zoom_FOV(float hfov, float magnification)
    {
        if (!std::isfinite(hfov) || hfov <= 0 || hfov >= 180 ||
            !std::isfinite(magnification) || magnification <= 0) {
            return 0;
        }
        return 2 * atanf(tanf(hfov * DEG_TO_RAD * 0.5f) / magnification) / DEG_TO_RAD;
    }

private:
    static constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    const char *_name;
    Type _type;
    unsigned _width, _height;
    float _hfov, _optical_max, _digital_max;
    FOV_Model _fov_model;
    float _tele_hfov;
};

class APC_Camera {
public:
    APC_Camera(const APC_Camera &) = delete;
    APC_Camera &operator=(const APC_Camera &) = delete;
    virtual ~APC_Camera() = default;

    static const APC_Camera &get_singleton();
    virtual const char *name() const = 0;
    virtual unsigned num_lenses() const = 0;
    // Invalid indices return nullptr; never silently substitute a different lens.
    virtual const APC_Lens *lens(unsigned index) const = 0;
    virtual unsigned num_streams() const = 0;
    virtual unsigned stream_lens_mask(unsigned index) const = 0;
    virtual unsigned streaming_resolutions(unsigned index) const = 0;
    virtual unsigned recording_resolutions() const = 0;
    virtual unsigned num_recording_channels() const = 0;
    virtual float zoom_control_max() const = 0;
    virtual bool has_independent_lens_zoom() const = 0;

    bool has_thermal() const
    {
        for (unsigned i = 0; i < num_lenses(); i++) {
            if (lens(i)->type() == APC_Lens::Type::Thermal) { return true; }
        }
        return false;
    }
    bool has_zoom() const
    {
        for (unsigned i = 0; i < num_lenses(); i++) {
            if (lens(i)->has_zoom()) { return true; }
        }
        return false;
    }

protected:
    APC_Camera() = default;
};

// Target headers remain the sole capability source for firmware, web and the
// Python simulator exporter. Hardware drivers stay behind their existing SDK
// boundaries; capability queries do not open a device or allocate memory.
class APC_Camera_Target final : public APC_Camera {
public:
    const char *name() const override { return APCAM_MODEL_NAME; }
    unsigned num_lenses() const override { return APCAM_NUM_LENSES; }
    const APC_Lens *lens(unsigned index) const override
    {
        static constexpr APC_Lens lenses[] = {
            {APCAM_LENS1_NAME, APCAM_LENS1_TYPE == APCAM_LENS_TYPE_THERMAL ?
                 APC_Lens::Type::Thermal : APC_Lens::Type::RGB, APCAM_LENS1_WIDTH, APCAM_LENS1_HEIGHT,
             APCAM_LENS1_FOV_H, APCAM_LENS1_OPTICAL_ZOOM_MAX,
             APCAM_LENS1_DIGITAL_ZOOM_MAX,
             APCAM_LENS1_FOV_MODEL == APCAM_FOV_ENDPOINTS ? APC_Lens::FOV_Model::Endpoints :
                 APC_Lens::FOV_Model::FocalLength, APCAM_LENS1_FOV_H_TELE},
#if APCAM_NUM_LENSES > 1
            {APCAM_LENS2_NAME, APCAM_LENS2_TYPE == APCAM_LENS_TYPE_THERMAL ?
                 APC_Lens::Type::Thermal : APC_Lens::Type::RGB, APCAM_LENS2_WIDTH, APCAM_LENS2_HEIGHT,
             APCAM_LENS2_FOV_H, APCAM_LENS2_OPTICAL_ZOOM_MAX, APCAM_LENS2_DIGITAL_ZOOM_MAX,
             APCAM_LENS2_FOV_MODEL == APCAM_FOV_ENDPOINTS ? APC_Lens::FOV_Model::Endpoints :
                 APC_Lens::FOV_Model::FocalLength, APCAM_LENS2_FOV_H_TELE},
#endif
#if APCAM_NUM_LENSES > 2
            {APCAM_LENS3_NAME, APCAM_LENS3_TYPE == APCAM_LENS_TYPE_THERMAL ?
                 APC_Lens::Type::Thermal : APC_Lens::Type::RGB, APCAM_LENS3_WIDTH, APCAM_LENS3_HEIGHT,
             APCAM_LENS3_FOV_H, APCAM_LENS3_OPTICAL_ZOOM_MAX, APCAM_LENS3_DIGITAL_ZOOM_MAX},
#endif
        };
        static_assert(sizeof(lenses) / sizeof(lenses[0]) == APCAM_NUM_LENSES, "lens count");
        return index < num_lenses() ? &lenses[index] : nullptr;
    }
    unsigned num_streams() const override { return APCAM_NUM_STREAMS; }
    unsigned stream_lens_mask(unsigned index) const override
    {
        const unsigned masks[] = {APCAM_STREAM1_LENS_MASK, APCAM_STREAM2_LENS_MASK};
        return index < num_streams() ? masks[index] : 0;
    }
    unsigned streaming_resolutions(unsigned index) const override
    {
        const unsigned resolutions[] = {APCAM_MAIN_RESOLUTIONS, APCAM_SUB_RESOLUTIONS};
        return index < num_streams() ? resolutions[index] : 0;
    }
    unsigned recording_resolutions() const override { return APCAM_RECORDING_RESOLUTIONS; }
    unsigned num_recording_channels() const override { return APCAM_NUM_RECORDING_CHANNELS; }
    float zoom_control_max() const override { return APCAM_ZOOM_CONTROL_MAX; }
    bool has_independent_lens_zoom() const override { return APCAM_HAVE_ZOOM_LENS; }
};

inline const APC_Camera &APC_Camera::get_singleton()
{
    static const APC_Camera_Target camera;
    return camera;
}
