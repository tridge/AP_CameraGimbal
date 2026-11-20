#include "camera_app/src/backends/zt30/overlay.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>
#include <ctime>
#include "camera_app/src/backends/zt30/zoom_feedback.h"
using namespace ca_zt30;
static std::map<unsigned,RgnConfig> regions;
static std::map<unsigned,std::pair<unsigned,unsigned>> attached;
static bool initialized=false,fail_bitmap=false,fail_detach=false;
static unsigned bitmaps;
extern "C" void ca_video_resolution_size(ca_video_resolution value,unsigned *w,unsigned *h) {
    switch(value) {
    case CA_VIDEO_2160P:*w=3840;*h=2160;break;
    case CA_VIDEO_1440P:*w=2560;*h=1440;break;
    case CA_VIDEO_1080P:*w=1920;*h=1080;break;
    default:*w=1280;*h=720;
    }
}
int main() {
    struct ca_overlay_geometry geometry{};
    ca_overlay_geometry_fov(&geometry,1280,720,false,true,84.5f,24.2f,640.0f/512);
    assert(geometry.count==4 && geometry.lines[0].x0==489 && geometry.lines[0].x1==791);
    assert(geometry.lines[0].y0==239 && geometry.lines[1].y0==481);
    ca_overlay_geometry_fov(&geometry,1280,720,false,true,84.5f,30,640.0f/512);
    assert(geometry.count==4 && geometry.lines[0].x0==451 && geometry.lines[1].y0==511);
    ca_overlay_geometry_fov(&geometry,1280,720,false,true,84.5f,0,640.0f/512);
    assert(!geometry.count);
    NativeSdk sdk;
    sdk.rgn.init=[](uint16_t chip,void *){assert(!chip && !initialized);initialized=true;return 0;};
    sdk.rgn.deinit=[](uint16_t chip){assert(!chip && regions.empty() && attached.empty());initialized=false;return 0;};
    sdk.rgn.create=[](uint16_t chip,uint32_t id,RgnConfig *a) {
        assert(!chip && initialized && a->type==0 && a->format==0 && a->width && a->height && !regions.count(id));
        regions[id]=*a;return 0;
    };
    sdk.rgn.destroy=[](uint16_t chip,uint32_t id){assert(!chip && !attached.count(id) && regions.count(id));regions.erase(id);return 0;};
    sdk.rgn.bitmap=[](uint16_t chip,uint32_t id,RgnBitmap *b) {
        assert(!chip && regions.count(id) && b->width==regions[id].width && b->height==regions[id].height && b->format==0 && b->address);
        bitmaps++;
        if(fail_bitmap)return -77;
        return 0;
    };
    sdk.rgn.attach=[](uint16_t chip,uint32_t id,Port *p,RgnDisplay *a) {
        assert(!chip && p->module==Scl && p->device==0 && p->channel<=1 && p->port>=1 && p->port<=4 && !attached.count(id));
        assert(a->show && a->foreground_alpha==255 && !a->background_alpha);
        attached[id]={p->channel,p->port};return 0;
    };
    sdk.rgn.display=[](uint16_t chip,uint32_t id,Port *p,RgnDisplay *a) {
        assert(!chip && attached.at(id)==std::make_pair(p->channel,p->port) && a->show);return 0;
    };
    sdk.rgn.detach=[](uint16_t chip,uint32_t id,Port *p) {
        assert(!chip && attached.at(id)==std::make_pair(p->channel,p->port));
        if(fail_detach)return -88;
        attached.erase(id);return 0;
    };
    Overlay owner(sdk);ca_config config{};
    config.main_resolution=CA_VIDEO_2160P;config.sub_resolution=CA_VIDEO_1440P;config.sub_codec=CA_VIDEO_H265;
    config.recording_resolution=CA_VIDEO_1080P;config.osd_cross=true;config.osd_thermal_fov=true;config.thermal_hfov_cd=2420;
    assert(owner.configure(1,true,config)==0);
    assert(attached.size()==15);
    for(auto &entry:attached)assert(entry.second.second!=3);
    config.osd_recording=true;assert(owner.apply(config)==0 && attached.size()==20);
    auto invalid=config;invalid.thermal_hfov_cd=0;
    assert(owner.apply(invalid)<0 && attached.size()==20);
    fail_bitmap=true;assert(owner.apply(config)<0);fail_bitmap=false;
    assert(owner.apply(config)==0 && attached.size()==20);
    assert(owner.select_source(0)==0);
    for(auto &entry:attached)assert(entry.second.first==0);
    // Unknown zoom geometry removes the FOV box instead of inventing a size.
    assert(attached.size()==4);
    timespec at{};clock_gettime(CLOCK_MONOTONIC,&at);
    const uint64_t now=uint64_t(at.tv_sec)*1000+at.tv_nsec/1000000;
    assert(zoom_feedback().observe(1,0,now));
    assert(owner.refresh(now)==0 && attached.size()==20);
    const auto updates=bitmaps;
    assert(owner.refresh(now+500)==0 && bitmaps==updates);
    assert(zoom_feedback().observe(30,0,now+1000));
    assert(owner.refresh(now+1000)==0 && attached.size()==4);
    assert(zoom_feedback().observe(1,0,now+1500));
    assert(owner.refresh(now+1500)==0 && attached.size()==20);
    assert(owner.refresh(now+4500)==0 && attached.size()==4); // stale telemetry

    fail_detach=true;assert(owner.close()==-88 && initialized && !regions.empty());fail_detach=false;
    assert(owner.close()==0 && !initialized && regions.empty() && attached.empty());
    assert(owner.close()==0);
    puts("PASS ZT30 overlays: four output geometries, clean recordings, FOV validity, source switch and detach-failure lifetime");
}
