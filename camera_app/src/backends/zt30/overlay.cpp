#include "overlay.h"
#include "camera_app/log.h"
#include "zoom_feedback.h"
#include <ctime>
#include <cerrno>
namespace ca_zt30 {
int Overlay::clear(unsigned index) {
    auto &r=regions[index];Port port{Scl,0,source,index/CA_OVERLAY_REGIONS+1};
    if(r.attached) {
        const int rc=s.rgn.detach(0,index,&port);if(rc)return rc;
        r.attached=false;
    }
    if(r.created) {
        const int rc=s.rgn.destroy(0,index);if(rc)return rc;
        r.created=false;
    }
    return 0;
}
int Overlay::configure(unsigned ch,bool aux,const ca_config &config) {
    if(configured || ch>1) { errno=EINVAL;return -1; }
    source=ch;external=aux;settings=config;configured=true;
    external_sub=config.sub_resolution!=CA_VIDEO_720P || config.sub_codec!=CA_VIDEO_H264;
    // Initialize once, before USB input starts. Never repeatedly dlopen RGN.
    uint32_t palette[256]{};
    if(s.rgn.init(0,palette)) { errno=EIO;return -1; }
    initialized=true;
    return apply(config);
}
int Overlay::apply(const ca_config &config) {
    timespec at{};clock_gettime(CLOCK_MONOTONIC,&at);
    return apply_with_fov(config,zoom_feedback().hfov(source==1,uint64_t(at.tv_sec)*1000+at.tv_nsec/1000000));
}
int Overlay::apply_with_fov(const ca_config &config,float rgb_fov) {
    if(!initialized) { errno=ENODEV;return -1; }
    if(config.osd_thermal_fov && config.thermal_hfov_cd<100) { errno=EINVAL;return -1; }
    for(unsigned c=0;c<4;c++) {
        unsigned width=1280,height=720;
        if(c==1)ca_video_resolution_size(config.main_resolution,&width,&height);
        if(c==2)ca_video_resolution_size(config.recording_resolution,&width,&height);
        if(c==3)ca_video_resolution_size(config.sub_resolution,&width,&height);
        const bool enabled=config.osd_cross && (c!=1 || external) && (c!=2 || config.osd_recording) && (c!=3 || external_sub);
        struct ca_overlay_geometry geometry{};
        const bool thermal_box=config.osd_thermal_fov && (c!=1 || external) && (c!=2 || config.osd_recording) && (c!=3 || external_sub);
        ca_overlay_geometry_fov(&geometry,width,height,enabled,thermal_box,rgb_fov,config.thermal_hfov_cd*.01f,640.0f/512);
        ca_overlay_bitmap bits[CA_OVERLAY_REGIONS]{};
        if(ca_overlay_bitmaps(bits,width,height,&geometry))return -1;
        int error=0;
        for(unsigned n=0;n<CA_OVERLAY_REGIONS;n++) {
            const unsigned index=c*CA_OVERLAY_REGIONS+n;
            auto &r=regions[index];const auto &b=bits[n];
            if(!b.pixels) { error=clear(index);if(error)break;continue; }
            if(r.created && (r.width!=b.width || r.height!=b.height)) {
                error=clear(index);if(error)break;
            }
            if(!r.created) {
                RgnConfig attr{0,0,b.width,b.height};error=s.rgn.create(0,index,&attr);if(error)break;
                r.created=true;r.width=b.width;r.height=b.height;
            }
            const uintptr_t address=reinterpret_cast<uintptr_t>(b.pixels);
            if(address>UINT32_MAX) { error=EOVERFLOW;break; }
            RgnBitmap bitmap{0,b.width,b.height,uint32_t(address)};
            error=s.rgn.bitmap(0,index,&bitmap);if(error)break;
            Port port{Scl,0,source,c+1};RgnDisplay display{};
            display.show=1;display.x=b.x;display.y=b.y;display.foreground_alpha=255;
            error=r.attached?s.rgn.display(0,index,&port,&display):s.rgn.attach(0,index,&port,&display);
            if(error)break;
            r.attached=true;
        }
        ca_overlay_free(bits);
        if(error) { ca_log("ZT30 RGB overlay failed: raw=%d",error);errno=EIO;return -1; }
    }
    settings=config;applied_hfov=rgb_fov;return 0;
}
int Overlay::refresh(uint64_t now_ms) {
    if(!initialized || !settings.osd_thermal_fov || now_ms<refresh_after)return 0;
    refresh_after=now_ms+500;
    const float hfov=zoom_feedback().hfov(source==1,now_ms);
    if(std::fabs(hfov-applied_hfov)<.001f)return 0;
    const int rc=apply_with_fov(settings,hfov);
    if(rc)refresh_after=now_ms+5000; // Bound diagnostics and SDK retries on failure.
    return rc;
}
int Overlay::select_source(unsigned ch) {
    if(ch>1)return EINVAL;
    if(ch==source)return 0;
    for(unsigned i=0;i<regions.size();i++) { const int rc=clear(i);if(rc)return rc; }
    source=ch;
    return apply(settings);
}
int Overlay::close() {
    if(!initialized)return 0;
    for(unsigned i=0;i<regions.size();i++) { const int rc=clear(i);if(rc)return rc; }
    const int rc=s.rgn.deinit(0);if(!rc)initialized=false;return rc;
}
}
