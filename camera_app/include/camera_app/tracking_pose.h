#pragma once
#include "camera_app/image_tracker.h"
#include <cmath>

// Match delayed images to measured attitude, rather than integrating body
// gyro axes as Euler rates. Callers serialize access with the tracker lock.
class ca_tracking_pose_history {
public:
    void add(uint64_t ms, const ca_tracking_pose &pose) {
        if(!pose.valid || !ms) return;
        if(_count && ms<=_samples[_count-1].ms) return;
        if(_count==32) {
            for(unsigned i=1;i<_count;i++) _samples[i-1]=_samples[i];
            --_count;
        }
        _samples[_count++]={ms,pose};
    }
    ca_tracking_pose at(uint64_t ms) const {
        if(!_count) return {};
        if(_count==1) return ms==_samples[0].ms ? _samples[0].pose : ca_tracking_pose{};
        if(ms<_samples[0].ms || ms>_samples[_count-1].ms+250) return {};
        unsigned i=1;
        while(i+1<_count && _samples[i].ms<ms) ++i;
        const auto &a=_samples[i-1], &b=_samples[i];
        if(b.ms-a.ms>250) return {};
        const float f=float(int64_t(ms)-int64_t(a.ms))/float(b.ms-a.ms);
        auto lerp=[f](float x,float y) { return x+f*std::remainder(y-x,6.28318530718f); };
        return {lerp(a.pose.roll,b.pose.roll),lerp(a.pose.pitch,b.pose.pitch),
                lerp(a.pose.yaw,b.pose.yaw),b.pose.hfov,true};
    }
private:
    struct sample { uint64_t ms; ca_tracking_pose pose; };
    sample _samples[32] {};
    unsigned _count=0;
};
