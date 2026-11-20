// Only this translation unit enables exceptions and includes dlib. Keeping the
// minimal FFT implementation here avoids a second, differently configured dlib
// library in cross builds. dlib: Boost license; bundled KISS FFT: BSD-3-Clause.
#include "camera_app/image_tracker.h"
#include <dlib/image_transforms/fhog.h>
#include <dlib/image_processing/correlation_tracker.h>
#include <dlib/fft/fft.cpp>
#include <dlib/test_for_odr_violations.cpp>
#include <algorithm>
#include <cmath>

namespace {
constexpr float pi = 3.14159265358979323846f;
float wrap(float a) { return std::remainder(a, 2*pi); }
float clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
struct ray { float x, y, z; };
ray rotate(ray v, const ca_tracking_pose &p, bool inverse)
{
    const float cr=std::cos(p.roll), sr=std::sin(p.roll),
        cp=std::cos(p.pitch), sp=std::sin(p.pitch), cy=std::cos(p.yaw), sy=std::sin(p.yaw);
    const float m[3][3]={{cp*cy, sr*sp*cy-cr*sy, cr*sp*cy+sr*sy},
        {cp*sy, sr*sp*sy+cr*cy, cr*sp*sy-sr*cy}, {-sp,sr*cp,cr*cp}};
    const float a[3]={v.x,v.y,v.z}; float b[3] {};
    for(unsigned i=0;i<3;i++) for(unsigned j=0;j<3;j++) b[i]+=(inverse?m[j][i]:m[i][j])*a[j];
    return {b[0],b[1],b[2]};
}
bool image_valid(const ca_tracking_frame &f)
{
    return f.width>=16 && f.width<=CA_TRACK_WIDTH && f.height>=16 && f.height<=CA_TRACK_HEIGHT && f.timestamp_ms;
}
void copy_image(const ca_tracking_frame &f, dlib::array2d<unsigned char> &image)
{
    image.set_size(f.height,f.width);
    for(unsigned y=0;y<f.height;y++) std::copy_n(f.pixels+y*f.width,f.width,&image[y][0]);
}
}

struct ca_image_tracker {
    // 64x64 spatial filter and 16 scale levels bound CPU/RAM on ARMv7.
    dlib::correlation_tracker model {6,4};
    dlib::array2d<unsigned char> image;
    ca_tracking_status status;
    ca_tracking_pose previous_pose;
    unsigned width=0,height=0;
    float target_yaw=0,target_pitch=0;
    bool have_motion=false;
};

ca_tracking_rect ca_tracking_point_rect(float x, float y, float radius, float aspect)
{
    const float r=std::clamp(std::isfinite(radius) ? radius : 0.f, 0.f, .5f);
    auto span=[](float c,float half,float minimum,float &lo,float &hi) {
        half=std::max(std::min({half,c,1-c}),minimum);
        c=std::clamp(c,half,1-half);
        lo=c-half; hi=c+half;
    };
    ca_tracking_rect rect{};
    span(x,r,.015f,rect.left,rect.right);
    span(y,r*aspect,.025f,rect.top,rect.bottom);
    return rect;
}

bool ca_tracking_rect_valid(const ca_tracking_rect &r)
{
    return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) &&
        std::isfinite(r.bottom) && r.left>=0 && r.top>=0 && r.right<=1 && r.bottom<=1 &&
        r.right>r.left && r.bottom>r.top;
}
ca_image_tracker *ca_image_tracker_create()
{
    try { return new ca_image_tracker; } catch (...) { return nullptr; }
}
void ca_image_tracker_destroy(ca_image_tracker *t) { delete t; }

static void angles(ca_image_tracker &t, const ca_tracking_pose &p, float dt, bool confirmed)
{
    t.status.pose=p;
    t.status.aspect=float(t.width)/t.height;
    const auto &r=t.status.rect;
    const float scale=2*std::tan(p.hfov*.5f);
    const ray camera={1,((r.left+r.right)*.5f-.5f)*scale,
        ((r.top+r.bottom)*.5f-.5f)*scale*t.height/t.width};
    t.status.yaw_error=std::atan2(camera.y,camera.x);
    t.status.pitch_error=-std::atan2(camera.z,std::hypot(camera.x,camera.y));
    if (!p.valid || !confirmed) return;
    const ray earth=rotate(camera,p,false);
    const float yaw=std::atan2(earth.y,earth.x), pitch=-std::atan2(earth.z,std::hypot(earth.x,earth.y));
    if(t.have_motion && dt>0 && dt<.5f) {
        const float alpha=dt/(.2f+dt);
        t.status.yaw_rate+=alpha*(clamp(wrap(yaw-t.target_yaw)/dt,-1.f,1.f)-t.status.yaw_rate);
        t.status.pitch_rate+=alpha*(clamp((pitch-t.target_pitch)/dt,-1.f,1.f)-t.status.pitch_rate);
    }
    t.target_yaw=yaw; t.target_pitch=pitch; t.have_motion=true;
}

bool ca_tracking_project(ca_tracking_status &s, const ca_tracking_pose &p, uint64_t ms)
{
    // SITL renders ahead of presentation; permit a bounded backward projection
    // as well as compensating a delayed physical camera observation.
    if(!s.pose.valid || !p.valid || ms+250<s.timestamp_ms || ms>s.timestamp_ms+500 ||
       !(p.hfov>0 && p.hfov<3.1f) || !(s.aspect>0)) return false;
    const float scale=2*std::tan(s.pose.hfov*.5f);
    const ray camera={1,((s.rect.left+s.rect.right)*.5f-.5f)*scale,
        ((s.rect.top+s.rect.bottom)*.5f-.5f)*scale/s.aspect};
    const ray earth=rotate(camera,s.pose,false);
    const float dt=(int64_t(ms)-int64_t(s.timestamp_ms))*.001f;
    const float yaw=std::atan2(earth.y,earth.x)+s.yaw_rate*dt;
    const float pitch=-std::atan2(earth.z,std::hypot(earth.x,earth.y))+s.pitch_rate*dt;
    const ray projected=rotate({std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)},p,true);
    if(projected.x<=.1f) return false;
    const float new_scale=2*std::tan(p.hfov*.5f);
    const float cx=.5f+projected.y/projected.x/new_scale;
    const float cy=.5f+projected.z/projected.x/new_scale*s.aspect;
    const float w=(s.rect.right-s.rect.left)*scale/new_scale;
    const float h=(s.rect.bottom-s.rect.top)*scale/new_scale;
    s.rect={cx-w*.5f,cy-h*.5f,cx+w*.5f,cy+h*.5f};
    s.yaw_error=std::atan2(projected.y,projected.x);
    s.pitch_error=-std::atan2(projected.z,std::hypot(projected.x,projected.y));
    s.timestamp_ms=ms; s.pose=p;
    return true;
}
bool ca_image_tracker_start(ca_image_tracker *t, const ca_tracking_frame &f,
                            const ca_tracking_rect &r, const ca_tracking_pose &p)
{
    if(!t || !image_valid(f) || !ca_tracking_rect_valid(r) ||
       (r.right-r.left)*f.width<8 || (r.bottom-r.top)*f.height<8) return false;
    try {
        copy_image(f,t->image);
        // Reject a textureless initial target before advertising acquisition.
        double sum=0,squared=0; unsigned n=0;
        for(unsigned y=r.top*f.height;y<r.bottom*f.height;y++)
            for(unsigned x=r.left*f.width;x<r.right*f.width;x++) {
                const float v=t->image[y][x]; sum+=v; squared+=v*v; n++;
            }
        if(!n || squared/n-(sum/n)*(sum/n)<16) return false;
        t->model.start_track(t->image,dlib::drectangle(r.left*f.width,r.top*f.height,
            r.right*f.width-1,r.bottom*f.height-1));
        t->status={}; t->status.rect=r; t->status.state=CA_TRACK_ACTIVE;
        t->status.timestamp_ms=t->status.confirmed_ms=f.timestamp_ms;
        t->width=f.width; t->height=f.height; t->have_motion=false;
        t->previous_pose=p; angles(*t,p,0,true);
        return true;
    } catch (...) { t->status.state=CA_TRACK_LOST; return false; }
}
ca_tracking_status ca_image_tracker_update(ca_image_tracker *t, const ca_tracking_frame &f,
                                           const ca_tracking_pose &p)
{
    if(!t) return {CA_TRACK_LOST};
    auto &s=t->status;
    if(s.state!=CA_TRACK_ACTIVE && s.state!=CA_TRACK_COASTING) return s;
    if(!image_valid(f) || f.width!=t->width || f.height!=t->height ||
       f.timestamp_ms<=s.timestamp_ms || f.timestamp_ms-s.confirmed_ms>600) {
        s.state=CA_TRACK_LOST; return s;
    }
    try {
        const float dt=(f.timestamp_ms-s.timestamp_ms)*.001f;
        const float since_good=(f.timestamp_ms-s.confirmed_ms)*.001f;
        ca_tracking_rect predicted=s.rect;
        if(p.valid && t->have_motion && t->previous_pose.valid) {
            const float yaw=t->target_yaw+s.yaw_rate*since_good;
            const float pitch=t->target_pitch+s.pitch_rate*since_good;
            const ray camera=rotate({std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)},p,true);
            if(camera.x<=.1f) { s.state=CA_TRACK_LOST; return s; }
            const float scale=2*std::tan(p.hfov*.5f);
            const float dx=.5f+camera.y/camera.x/scale-(s.rect.left+s.rect.right)*.5f;
            const float dy=.5f+camera.z/camera.x/scale*t->width/t->height-(s.rect.top+s.rect.bottom)*.5f;
            predicted.left+=dx; predicted.right+=dx; predicted.top+=dy; predicted.bottom+=dy;
        }
        if(predicted.right<=0 || predicted.left>=1 || predicted.bottom<=0 || predicted.top>=1) {
            s.state=CA_TRACK_LOST; return s;
        }
        copy_image(f,t->image);
        // update() learns on every call. Only commit this copy after passing
        // confidence and displacement gates, preserving appearance in occlusion.
        auto candidate=t->model;
        const dlib::drectangle guess(predicted.left*f.width,predicted.top*f.height,
                                    predicted.right*f.width-1,predicted.bottom*f.height-1);
        s.quality=candidate.update(t->image,guess);
        const auto box=candidate.get_position();
        ca_tracking_rect found={float(box.left()/f.width),float(box.top()/f.height),
            float((box.right()+1)/f.width),float((box.bottom()+1)/f.height)};
        const double displacement=dlib::length(dlib::center(box)-dlib::center(guess));
        const bool good=std::isfinite(s.quality) && s.quality>=(s.state==CA_TRACK_COASTING?9.f:7.f) &&
            ca_tracking_rect_valid(found) && displacement<std::max(12.,.75*std::max(guess.width(),guess.height()));
        s.timestamp_ms=f.timestamp_ms;
        if(good) {
            t->model=std::move(candidate); s.rect=found;
            s.state=CA_TRACK_ACTIVE; angles(*t,p,since_good,true);
            s.confirmed_ms=f.timestamp_ms;
        } else {
            s.rect=predicted;
            // Without fresh pose, coasting cannot compensate camera motion.
            s.state=p.valid && t->previous_pose.valid && since_good<=.5f ? CA_TRACK_COASTING : CA_TRACK_LOST;
            angles(*t,p,dt,false);
        }
        t->previous_pose=p;
    } catch (...) { s.state=CA_TRACK_LOST; }
    return s;
}
