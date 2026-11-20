#pragma once
#include "camera_app/video_fov.h"
#include <cstdint>
#include <mutex>
namespace ca_zt30 {
// Optical module telemetry only. Observing a reply never sends a lens command.
// No Linux digital zoom is applied by the current fixed native RGB pipeline.
class ZoomFeedback {
public:
    void reset() { std::lock_guard<std::mutex> guard(lock);ratio=NAN;updated=0; }
    bool observe(uint8_t integer,uint8_t decimal,uint64_t now) {
        if(integer<1 || integer>30 || decimal>9 || (integer==30 && decimal))return false;
        std::lock_guard<std::mutex> guard(lock);
        ratio=integer+decimal*0.1f;updated=now;return true;
    }
    float get(uint64_t now) const {
        std::lock_guard<std::mutex> guard(lock);
        return now>=updated && now-updated<=2500 ? ratio : NAN;
    }
    float hfov(bool wide,uint64_t now) const {
        if(wide)return ca_lens1_hfov(1);
        const float optical=get(now);
        return std::isfinite(optical) ? ca_zoom_lens_hfov(optical,1) : 0;
    }
private:
    mutable std::mutex lock;
    float ratio=NAN;
    uint64_t updated=0;
};
inline ZoomFeedback &zoom_feedback() { static ZoomFeedback value;return value; }
} // namespace ca_zt30
