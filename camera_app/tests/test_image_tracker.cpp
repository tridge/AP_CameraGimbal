#include "camera_app/image_tracker.h"
#include "camera_app/tracking_pose.h"
#include "camera_app/tracking_rate.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <initializer_list>

static ca_tracking_frame frame(unsigned n, int dx=0, bool occluded=false)
{
    ca_tracking_frame f;
    f.width=320; f.height=180; f.timestamp_ms=1000+n*66;
    for(unsigned y=0;y<f.height;y++) for(unsigned x=0;x<f.width;x++) {
        unsigned v=48+(x*13+y*7)%9;
        const int tx=int(x)-dx-120,ty=int(y)-60;
        if(!occluded && tx>=0 && tx<48 && ty>=0 && ty<40)
            v=30+(tx*71+ty*23+(tx*ty*13))%220;
        f.pixels[y*f.width+x]=v;
    }
    return f;
}
static void point_rect_tests()
{
    auto near=[](float a,float b) { return std::fabs(a-b)<1e-5f; };
    auto accepted=[](const ca_tracking_rect &r) {
        // APC_Tracking::start() limits
        return ca_tracking_rect_valid(r) && (r.right-r.left)*CA_TRACK_WIDTH>=8 && (r.bottom-r.top)*180>=8;
    };
    // square in pixels on 16:9, centred on the point
    ca_tracking_rect r=ca_tracking_point_rect(.5f,.5f,.1f,16.f/9);
    assert(near(r.left,.4f) && near(r.right,.6f) && near(r.top,.5f-.1f*16/9) && near(r.bottom,.5f+.1f*16/9));
    // near an edge it shrinks around the point instead of moving off it
    r=ca_tracking_point_rect(.05f,.97f,.1f,16.f/9);
    assert(near((r.left+r.right)/2,.05f) && near((r.top+r.bottom)/2,.97f) && accepted(r));
    // at the very edge, radius 0 or NaN: the smallest accepted box
    for(float x : {0.f,1.f,.5f}) for(float radius : {0.f,NAN,-1.f}) {
        r=ca_tracking_point_rect(x,x,radius,16.f/9);
        assert(accepted(r));
    }
    // a huge radius is the whole image
    r=ca_tracking_point_rect(.3f,.3f,5.f,5.f/4);
    assert(near(r.left,0) && near(r.right,.6f) && accepted(r));
}

int main()
{
    point_rect_tests();
    // The MT11 holds still below 6 deg/s. The integrated physical motion must
    // follow a sub-threshold request without accumulating an integral bias.
    for(float requested : {-.25f,-1.f,-2.f,-5.9f,.25f,1.f,2.f,5.9f,6.f,10.f}) {
        ca_tracking_rate_pulses pulses;
        float wanted=0, moved=0;
        float held=pulses.apply(requested,6,.05f);
        for(unsigned i=0;i<400;i++) {
            const float dt=i%3 ? .05f : .08f;
            wanted+=requested*dt;
            moved+=held*dt;
            const float output=pulses.apply(requested,6,dt);
            assert(output==0 || std::fabs(output)>=6);
            assert(output*requested>=0);
            assert(std::fabs(moved-wanted)<.72f);
            held=output;
        }
        assert(pulses.apply(0,6,.05f)==0);
        assert(pulses.apply(-requested,6,.05f)*requested<=0);
        pulses.reset();
        assert(pulses.apply(.1f,6,.05f)==0);
        assert(pulses.apply(NAN,6,.05f)==0);
        assert(pulses.apply(1,6,1)==0);
        assert(pulses.apply(.1f,0,.05f)==.1f);
    }

    // A delayed image must use the old measured pose, including across yaw
    // wrap. Missing/stale history must never invent a usable orientation.
    ca_tracking_pose_history history;
    ca_tracking_pose a{0,-.4f,3.10f,1.1f,true}, b{0,-.3f,-3.10f,1.1f,true};
    history.add(1000,a); history.add(1100,b);
    auto middle=history.at(1050);
    assert(middle.valid && std::fabs(middle.pitch+.35f)<1e-5f);
    assert(std::fabs(std::fabs(middle.yaw)-3.14159265f)<1e-5f);
    assert(!history.at(999).valid && !history.at(1351).valid);
    assert(std::fabs(history.at(1150).pitch+.25f)<1e-5f);
    history.add(1500,b); assert(!history.at(1300).valid);

    // A stationary object was 0.2 rad right in a 190-ms-old observation.
    // If the camera has since rotated onto it, the control error and drawn
    // box must be centered, while the confirmation time remains unchanged.
    ca_tracking_status delayed;
    delayed.pose={0,0,0,1.1f,true}; delayed.aspect=16.f/9;
    const float old_x=.5f+std::tan(.2f)/(2*std::tan(.55f));
    delayed.rect={old_x-.05f,.45f,old_x+.05f,.55f};
    delayed.timestamp_ms=delayed.confirmed_ms=1000;
    assert(ca_tracking_project(delayed,{0,0,.2f,1.1f,true},1190));
    assert(std::fabs(delayed.yaw_error)<1e-5f);
    assert(std::fabs((delayed.rect.left+delayed.rect.right)*.5f-.5f)<1e-5f);
    assert(delayed.confirmed_ms==1000);
    assert(!ca_tracking_project(delayed,{0,0,.2f,1.1f,true},1800));

    auto *t=ca_image_tracker_create(); assert(t);
    ca_tracking_pose pose; pose.hfov=1.1f; pose.valid=true;
    const ca_tracking_rect rect={120.f/320,60.f/180,168.f/320,100.f/180};
    assert(!ca_image_tracker_start(t,frame(0),{NAN,0,1,1},pose));
    assert(!ca_image_tracker_start(t,frame(0),{0,0,.1f,.1f},pose));
    assert(ca_image_tracker_start(t,frame(0),rect,pose));
    auto start=std::chrono::steady_clock::now();
    for(unsigned n=1;n<=12;n++) {
        auto s=ca_image_tracker_update(t,frame(n,n*2),pose);
        printf("frame %u state %u PSR %.2f center %.1f\n",n,s.state,s.quality,(s.rect.left+s.rect.right)*160);
        assert(s.state==CA_TRACK_ACTIVE);
        assert(std::fabs((s.rect.left+s.rect.right)*160-(144+n*2))<5);
    }
    auto s=ca_image_tracker_update(t,frame(13,26,true),pose);
    assert(s.state==CA_TRACK_COASTING);
    s=ca_image_tracker_update(t,frame(14,28,true),pose); assert(s.state==CA_TRACK_COASTING);
    s=ca_image_tracker_update(t,frame(15,30),pose);
    printf("recovery state %u PSR %.2f center %.1f\n",s.state,s.quality,(s.rect.left+s.rect.right)*160);
    assert(s.state==CA_TRACK_ACTIVE);
    for(unsigned n=16;n<=24;n++) s=ca_image_tracker_update(t,frame(n,n*2,true),pose);
    assert(s.state==CA_TRACK_LOST);
    s=ca_image_tracker_update(t,frame(25,50),pose); assert(s.state==CA_TRACK_LOST);
    assert(ca_image_tracker_start(t,frame(30),rect,pose));
    pose.valid=false;
    s=ca_image_tracker_update(t,frame(31,0,true),pose); assert(s.state==CA_TRACK_LOST);
    // A fixed world target moves left as the camera turns right. Occlude it
    // while the camera continues turning, then recover at the predicted LOS.
    pose.valid=true; pose.yaw=0;
    assert(ca_image_tracker_start(t,frame(40),rect,pose));
    const float focal=160/std::tan(pose.hfov/2);
    const float target_yaw=std::atan((144-160)/focal);
    for(unsigned n=1;n<=8;n++) {
        pose.yaw=n*.02f;
        const int dx=std::lround(focal*std::tan(target_yaw-pose.yaw)+160-144);
        s=ca_image_tracker_update(t,frame(40+n,dx,n==5 || n==6),pose);
        assert(s.state==(n==5 || n==6 ? CA_TRACK_COASTING : CA_TRACK_ACTIVE));
        assert(std::fabs((s.rect.left+s.rect.right)*160-(144+dx))<5);
        if(n==8) assert(std::fabs(s.yaw_rate)<.08f);
    }
    auto stale=frame(48); // duplicate frame timestamp is never a new observation
    s=ca_image_tracker_update(t,stale,pose); assert(s.state==CA_TRACK_LOST);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    printf("PASS translation, occlusion recovery with camera rotation, long loss, stale pose/frame: %.1f ms\n",ms);
    ca_image_tracker_destroy(t);
}
