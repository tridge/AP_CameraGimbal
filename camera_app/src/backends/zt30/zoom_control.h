#pragma once
#include "zoom_feedback.h"
#include "mi/api.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
namespace ca_zt30 {
// Requests cross from media controls to the sole MCU UART owner. Run the lens
// continuously during a long move; pulse near the target to let feedback
// settle. Stop on stale telemetry, crossing, timeout, cancellation or shutdown.
class OpticalZoom {
public:
    void reset() { std::lock_guard<std::mutex> g(lock);target=last_observed=NAN;wire=previous=blocked=0;deadline=pulse_end=cooldown=progress=last_sent=0; }
    bool request(float value,uint64_t now) {
        if(!std::isfinite(value) || value<1 || value>30)return false;
        std::lock_guard<std::mutex> g(lock);
        if(blocked && last_observed==blocked_observed && (value-last_observed)*blocked>0)return false;
        // Repeated absolute/rate requests must not renew a stalled motor's
        // progress timer or lifetime. Reversing direction starts a new move.
        if(!std::isfinite(target) || (std::isfinite(last_observed) && (value-last_observed)*(target-last_observed)<0)) {
            deadline=now+45000;progress=now;previous=0;
        }
        target=value;return true;
    }
    void cancel() { std::lock_guard<std::mutex> g(lock);target=NAN; }
    int current() const { std::lock_guard<std::mutex> g(lock);return wire; }
    // The module stops a single direction command after a short move. Refresh
    // it while driving, rather than alternating explicit stop/start pulses.
    bool refresh_due(uint64_t now) const { std::lock_guard<std::mutex> g(lock);return wire && now-last_sent>=80; }
    bool active() const { std::lock_guard<std::mutex> g(lock);return std::isfinite(target) || wire; }
    int wanted(float observed,uint64_t now) {
        std::lock_guard<std::mutex> g(lock);
        if(!std::isfinite(observed)) {
            if(wire || now>=deadline)target=NAN;
            return 0; // A queued request waits for fresh initial telemetry.
        }
        if(!std::isfinite(last_observed) || observed!=last_observed) {
            last_observed=observed;progress=now;blocked=0;
        }
        if(std::isfinite(target) && (now>=deadline || now-progress>=2000)) {
            blocked=target>observed?1:-1;blocked_observed=observed;target=NAN;
        }
        const float error=target-observed;
        const float tolerance=target==1 || target==30 ? .01f:.05f;
        if(std::isfinite(target) && (std::fabs(error)<=tolerance || (previous && error*previous<0)))target=NAN;
        if(wire) {
            if(!std::isfinite(target) || error*wire<=0)return 0;
            // Pausing every 120 ms makes a long optical sweep unnecessarily
            // slow and visibly jerky. Scale the approach distance with zoom:
            // a fixed 1x margin makes the entire 1x -> 2x move pulse slowly.
            if(std::fabs(error)>std::max(.1f,target*.1f))pulse_end=now+120;
            return now<pulse_end ? wire:0;
        }
        if(!std::isfinite(target) || now<cooldown)return 0;
        pulse_end=now+120;
        previous=error>0?1:-1;return previous;
    }
    void sent(int value,uint64_t now) {
        std::lock_guard<std::mutex> g(lock);
        wire=value;last_sent=now;if(!wire)cooldown=now+140;
    }
private:
    mutable std::mutex lock;
    float target=NAN;
    float last_observed=NAN;
    int wire=0,previous=0;
    int blocked=0;
    float blocked_observed=NAN;
    uint64_t deadline=0,pulse_end=0,cooldown=0;
    uint64_t progress=0,last_sent=0;
};
inline OpticalZoom &optical_zoom() { static OpticalZoom value;return value; }

inline bool zoom_crop(float factor,Rect &crop) {
    if(!std::isfinite(factor) || factor<1 || factor>6)return false;
    // Keep the 16:9 window and NV12 alignment, including at 6x (640x360).
    unsigned width=std::max(640U,unsigned(3840/factor)/32*32);
    unsigned height=width*9/16;
    crop={uint16_t((3840-width)/2),uint16_t((2160-height)/4*2),uint16_t(width),uint16_t(height)};
    return true;
}
// Realtime ISP-to-SCL binding does not support input cropping. Update the
// enabled zoom output ports together, preserving every output format.
class DigitalZoom {
public:
    explicit DigitalZoom(Api &a):api(a) {}
    int set(float factor,unsigned ports=0x0b) {
        Rect crop{};SclPort before[5]{},after[5]{},actual{};
        if(!zoom_crop(factor,crop)) { errno=EINVAL;return -1; }
        if(!ports || (ports&~31U)) { errno=EINVAL;return -1; }
        if(!api.scl_get_port || !api.scl_set_port) { errno=ENOTSUP;return -1; }
        // Port configuration belongs to this media context. Cache only a
        // fully verified transaction; a changed port mask must be checked too.
        if(ports==verified_ports && !std::memcmp(&crop,&verified_crop,sizeof(crop)))return 0;
        for(unsigned p=0;p<5;p++)if(ports&(1U<<p)) {
            if(api.scl_get_port(0,0,p,&before[p])) { errno=EIO;return -1; }
            after[p]=before[p];after[p].crop=crop;
        }
        unsigned touched=0;
        for(unsigned p=0;p<5;p++)if(ports&(1U<<p)) {
            touched|=1U<<p;
            if(api.scl_set_port(0,0,p,&after[p]) || api.scl_get_port(0,0,p,&actual) ||
               std::memcmp(&after[p],&actual,sizeof(actual))) {
                bool restored=true;
                for(unsigned q=0;q<5;q++)if(touched&(1U<<q)) {
                    const int rc=api.scl_set_port(0,0,q,&before[q]);
                    if(rc || api.scl_get_port(0,0,q,&actual) || std::memcmp(&before[q],&actual,sizeof(actual)))restored=false;
                }
                if(!restored)verified_ports=0;
                errno=restored?EIO:EOWNERDEAD;return -1;
            }
        }
        verified_crop=crop;verified_ports=ports;
        ratio=3840.0f/crop.width;return 0;
    }
    float get() const { return ratio; }
private:
    Api &api;
    float ratio=1;
    Rect verified_crop{};
    unsigned verified_ports=0;
};
}
