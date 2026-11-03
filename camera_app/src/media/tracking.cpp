#include "camera_app/APC_Tracking.h"
#include "camera_app/APC_Media_Backend.h"
#include "camera_app/backend.h"
#include "camera_app/metadata.h"
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "apcam/target.h"
#include <cmath>
#include <algorithm>
#include <unistd.h>

static uint64_t tracking_ms()
{
    timespec t {}; clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;
}
bool APC_Tracking::open(APC_Media_Backend *media)
{
    close();
    if(!media || !media->tracking_available() || _lock.error() || _wake.error()) return false;
    _engine=ca_image_tracker_create();
    if(!_engine) return false;
    _media=media; _quit=false; _poses={};
    const int error=pthread_create(&_thread,nullptr,[](void *p)->void* {
        static_cast<APC_Tracking *>(p)->run(); return nullptr;
    },this);
    if(error) { close(); return false; }
    _running=true;
    return true;
}
void APC_Tracking::close()
{
    if(_running) {
        { APC_LockGuard guard(_lock); _quit=true; _status={}; ++_generation; _wake.signal(); }
        pthread_join(_thread,nullptr); _running=false;
    }
    ca_image_tracker_destroy(_engine); _engine=nullptr; _media=nullptr;
}
int APC_Tracking::start(ca_tracking_rect r, ca_tracking_owner owner)
{
    if(!_running) { errno=ENOSYS; return -1; }
    if(!ca_tracking_rect_valid(r) || owner==CA_TRACK_OWNER_NONE ||
        (r.right-r.left)*CA_TRACK_WIDTH<8 || (r.bottom-r.top)*180<8) { errno=EINVAL; return -1; }
    APC_LockGuard guard(_lock);
    _status={}; _status.rect=r; _status.owner=owner;
    _integral[0]=_integral[1]=0;
    for(auto &rate : _rate_pulses) rate.reset();
    _status.state=CA_TRACK_ACQUIRING; _status.timestamp_ms=tracking_ms();
    ++_generation; _wake.signal();
    return 0;
}
void APC_Tracking::stop(ca_tracking_owner owner)
{
    APC_LockGuard guard(_lock);
    if(owner!=CA_TRACK_OWNER_NONE && _status.owner!=owner) return;
    if(_driving && _gimbal) (void)ca_backend_set_gimbal_rates(_gimbal,0,0);
    _driving=false;
    _integral[0]=_integral[1]=0;
    for(auto &rate : _rate_pulses) rate.reset();
    _status={}; ++_generation; _wake.signal();
}
ca_tracking_status APC_Tracking::status()
{
    APC_LockGuard guard(_lock); return _status;
}
void APC_Tracking::run()
{
    ca_tracking_frame frame;
    unsigned generation=0;
    uint64_t last_frame=0;
    for(;;) {
        _lock.lock();
        if(_quit) { _lock.unlock(); return; }
        ca_tracking_status s=_status;
        const unsigned current=_generation;
        if(s.state==CA_TRACK_IDLE || s.state==CA_TRACK_LOST) {
            timespec until {}; clock_gettime(CLOCK_REALTIME,&until); until.tv_sec++;
            _wake.wait_until(_lock,until); _lock.unlock(); continue;
        }
        _lock.unlock();
        const uint64_t began=tracking_ms();
        if(_media->tracking_frame(frame) && frame.timestamp_ms!=last_frame &&
           frame.timestamp_ms<=began+250 && began<=frame.timestamp_ms+250) {
            ca_tracking_pose pose;
            { APC_LockGuard guard(_lock); pose=_poses.at(frame.timestamp_ms); }
            const uint64_t retrieved=tracking_ms();
            last_frame=frame.timestamp_ms;
            if(current!=generation) {
                generation=current;
                const bool ok=ca_image_tracker_start(_engine,frame,s.rect,pose);
                s.state=ok?CA_TRACK_ACTIVE:CA_TRACK_LOST;
                s.timestamp_ms=s.confirmed_ms=frame.timestamp_ms;
                s.pose=pose; s.aspect=float(frame.width)/frame.height;
                (void)ca_tracking_project(s,pose,frame.timestamp_ms);
            } else {
                const auto owner=s.owner;
                s=ca_image_tracker_update(_engine,frame,pose); s.owner=owner;
            }
            s.capture_delay_ms=retrieved>frame.timestamp_ms ? retrieved-frame.timestamp_ms : 0;
            constexpr float deg=57.295779513f;
            CA_BINLOG(CA_LOG_TRKF,ca_log_trkf,.frame_ms=frame.timestamp_ms,
                .state=uint8_t(s.state),.quality=s.quality,
                .pitch_error=s.pitch_error*deg,.yaw_error=s.yaw_error*deg,
                .pitch_rate=s.pitch_rate*deg,.yaw_rate=s.yaw_rate*deg,
                .roll=pose.roll*deg,.pitch=pose.pitch*deg,.yaw=pose.yaw*deg,.hfov=pose.hfov*deg);
        } else if(began>s.timestamp_ms+500) s.state=CA_TRACK_LOST;
        {
            APC_LockGuard guard(_lock);
            if(current==_generation) _status=s;
        }
        // Bound CPU usage and never accumulate a backlog of capture buffers.
        const uint64_t elapsed=tracking_ms()-began;
        if(elapsed<66) usleep((66-elapsed)*1000);
    }
}
void APC_Tracking::update(ca_backend *gimbal, bool manual, float hfov)
{
    _gimbal=gimbal;
    if(manual) _driving=false; // The manual controller already owns the motor.
    if(manual) stop();
    const uint64_t now=tracking_ms();
    if(now-_command_ms<50) return;
    const float elapsed=(now-_command_ms)*.001f;
    const float dt=std::min(.1f,elapsed);
    _command_ms=now;
    ca_gimbal_attitude attitude {};
    const bool fresh=ca_backend_gimbal_attitude(gimbal,&attitude) &&
        now>=attitude.timestamp_ms && now-attitude.timestamp_ms<250;
    ca_metadata m {}; ca_metadata_snapshot(&m);
    const bool aircraft=m.have_vehicle_attitude && m.vehicle_attitude_age_ms<250 &&
        std::isfinite(m.vehicle_yaw_rad) && std::isfinite(m.vehicle_yaw_rate_rad_s);
    ca_tracking_pose pose;
    pose.roll=attitude.roll_rad; pose.pitch=attitude.pitch_rad; pose.yaw=attitude.yaw_rad;
    pose.hfov=hfov*.01745329252f;
    if(aircraft) pose.yaw+=m.vehicle_yaw_rad+m.vehicle_yaw_rate_rad_s*
        (int64_t(attitude.timestamp_ms)-int64_t(now)+m.vehicle_attitude_age_ms)*.001f;
    pose.valid=fresh && std::isfinite(pose.roll) && std::isfinite(pose.pitch) && std::isfinite(pose.yaw) &&
        std::isfinite(pose.hfov) && pose.hfov>0 && pose.hfov<3.1f;
    { APC_LockGuard guard(_lock); _poses.add(attitude.timestamp_ms,pose); pose=_poses.at(now); }
    const auto s=status();
    auto control=s;
    const bool active=(s.state==CA_TRACK_ACTIVE || s.state==CA_TRACK_COASTING) &&
        now<=s.timestamp_ms+350 && now<=s.confirmed_ms+500 && fresh && !manual &&
        ca_tracking_project(control,pose,now);
    if(!active) {
        if(_driving && !manual) (void)ca_backend_set_gimbal_rates(gimbal,0,0);
        _driving=false;
        _integral[0]=_integral[1]=0;
        for(auto &rate : _rate_pulses) rate.reset();
    } else {
        const float limit=std::min(30.f,float(APCAM_GIMBAL_RATE_MAX))*.01745329252f;
        const float error[2]={control.pitch_error,control.yaw_error};
        const float minimum[2]={APCAM_TRACKING_MIN_PITCH_RATE*.01745329252f,
                                APCAM_TRACKING_MIN_YAW_RATE*.01745329252f};
        for(unsigned i=0;i<2;i++) {
            // Calibrated pulses already realize sub-minimum average rates;
            // do not retain the dead-zone integral that drives past center.
            if(minimum[i]>0) { _integral[i]=0; continue; }
            // Uncalibrated targets retain their existing dead-zone correction.
            // Never learn an integral from an unconfirmed/coasting rectangle.
            if(s.state==CA_TRACK_ACTIVE && fabsf(error[i])<.15f)
                _integral[i]=std::clamp(_integral[i]+.8f*error[i]*dt,-.14f,.14f);
        }
        float pitch=std::clamp(s.pitch_rate+1.5f*control.pitch_error+_integral[0],-limit,limit);
        float yaw=std::clamp(s.yaw_rate+1.5f*control.yaw_error+_integral[1]-(aircraft?m.vehicle_yaw_rate_rad_s:0),-limit,limit);
        pitch=_rate_pulses[0].apply(pitch,minimum[0],elapsed);
        yaw=_rate_pulses[1].apply(yaw,minimum[1],elapsed);
        if((attitude.pitch_rad<=APCAM_GIMBAL_PITCH_MIN*.01745329252f && pitch<0) ||
           (attitude.pitch_rad>=APCAM_GIMBAL_PITCH_MAX*.01745329252f && pitch>0)) {
            pitch=0; _integral[0]=0; _rate_pulses[0].reset();
        }
        if(!APCAM_GIMBAL_YAW_CONTINUOUS &&
           ((attitude.yaw_rad<=APCAM_GIMBAL_YAW_MIN*.01745329252f && yaw<0) ||
            (attitude.yaw_rad>=APCAM_GIMBAL_YAW_MAX*.01745329252f && yaw>0))) {
            yaw=0; _integral[1]=0; _rate_pulses[1].reset();
        }
        if(ca_backend_set_gimbal_rates(gimbal,pitch,yaw)<0) { stop(); (void)ca_backend_set_gimbal_rates(gimbal,0,0); }
        else _driving=true;
    }
    if(s.state!=CA_TRACK_IDLE && s.state!=CA_TRACK_LOST) (void)ca_backend_request_gimbal_attitude(gimbal);
    // The encoder is also displaying delayed sensor images. Advance the box
    // through tracker processing time, not all the way to the current pose.
    auto overlay=s;
    const uint64_t overlay_ms=now-std::min(now,s.capture_delay_ms);
    ca_tracking_pose overlay_pose;
    { APC_LockGuard guard(_lock); overlay_pose=_poses.at(overlay_ms); }
    const bool show=active && ca_tracking_project(overlay,overlay_pose,overlay_ms);
    if(_media) _media->tracking_overlay(show?&overlay:nullptr);
}
