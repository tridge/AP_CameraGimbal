/* SigmaStar hardware regions; one small encoder/VPE overlay per stream.
 * Region ABI declarations are the same pinned OpenIPC sources as each backend. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "camera_app/overlay.h"
#include "camera_app/log.h"
#include "apcam/target.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#if APCAM_TARGET == APCAM_TARGET_A8
#include "pipeline.h"
#include "mi/star/m6_rgn.h"
#define MI_TYPE(name) m6_##name
#define MI_CALL(hw, name, ...) (hw)->mi.name(0, __VA_ARGS__)
#define MI_DEINIT(hw) (hw)->mi.fnDeinit(0)
/* Attach OSD to the same scaler device used by the A8 video pipeline. */
#define MI_DEST(c) (m6_sys_bind){.module=M6_SYS_MOD_SCL,.device=CA_A8_SCL_DEV,.channel=0,.port=(c)}
#else
#include "mi/star/i6_rgn.h"
#define MI_TYPE(name) i6_##name
#define MI_CALL(hw, name, ...) (hw)->mi.name(__VA_ARGS__)
#define MI_DEINIT(hw) (hw)->mi.fnDeinit()
/* RGN's VPE destination enum is 0, distinct from the SYS module enum. */
#define MI_DEST(c) (i6_sys_bind){.module=(i6_sys_mod)0,.device=0,.channel=0,.port=(c)}
#endif
struct ca_overlay_hw {
    MI_TYPE(rgn_impl) mi;
    bool initialized, created[3][CA_OVERLAY_REGIONS], attached[3][CA_OVERLAY_REGIONS];
    unsigned width[3][CA_OVERLAY_REGIONS], height[3][CA_OVERLAY_REGIONS];
    ca_overlay_channel previous[3];
};
static unsigned handle(unsigned c,unsigned r) { return c*CA_OVERLAY_REGIONS+r; }
static void clear(struct ca_overlay_hw *hw,unsigned c,unsigned r)
{
    MI_TYPE(sys_bind) dest=MI_DEST(c);
    const unsigned h=handle(c,r);
    if(hw->attached[c][r]) (void)MI_CALL(hw,fnDetachChannel,h,&dest);
    if(hw->created[c][r]) (void)MI_CALL(hw,fnDestroyRegion,h);
    hw->attached[c][r]=hw->created[c][r]=false;
}
void ca_overlay_hw_close(struct ca_overlay_hw *hw)
{
    if(!hw) return;
    for(unsigned c=0;c<3;c++) for(unsigned r=0;r<CA_OVERLAY_REGIONS;r++) clear(hw,c,r);
    if(hw->initialized) (void)MI_DEINIT(hw);
    MI_TYPE(rgn_unload)(&hw->mi);
    free(hw);
}
int ca_overlay_hw_set(struct ca_overlay_hw **out,const struct ca_overlay_channel *channels,unsigned count)
{
    if(count>3) { errno=EINVAL; return -1; }
    bool enabled=false;
    for(unsigned c=0;c<count;c++) enabled |= channels[c].cross || channels[c].tracking;
    if(!enabled) {
        // Keep the SDK loaded until pipeline shutdown. On A8, repeated
        // Init/DeInit/dlclose cycles leak /dev/mi_rgn descriptors and eventually
        // assert in the vendor wrapper. Stale tracking frames can hide the box
        // several times a second, so removing its regions must not unload RGN.
        if(*out) {
            for(unsigned c=0;c<3;c++) for(unsigned r=0;r<CA_OVERLAY_REGIONS;r++) clear(*out,c,r);
            memset((*out)->previous,0,sizeof((*out)->previous));
        }
        return 0;
    }
    int error=0;
    ca_overlay_hw *hw;
    if(!*out) {
        *out=static_cast<ca_overlay_hw *>(calloc(1,sizeof(ca_overlay_hw)));
        if(!*out) return -1;
        hw=*out;
        if((error=MI_TYPE(rgn_load)(&hw->mi))) goto fail;
        MI_TYPE(rgn_pal) palette {};
        if((error=MI_CALL(hw,fnInit,&palette))) goto fail;
        hw->initialized=true;
    }
    hw=*out;
    for(unsigned c=0;c<count;c++) {
        const auto &s=channels[c];
        if(!memcmp(&s,&hw->previous[c],sizeof(s))) continue;
        struct ca_overlay_geometry g;
        ca_overlay_bitmap bits[CA_OVERLAY_REGIONS];
        ca_overlay_geometry(&g,s.width,s.height,s.cross,false,0);
        if(s.tracking) ca_overlay_tracking(&g,s.width,s.height,s.rect);
        if(ca_overlay_bitmaps(bits,s.width,s.height,&g)<0) { error=-1; goto fail; }
        for(unsigned r=0;r<CA_OVERLAY_REGIONS;r++) {
            auto &b=bits[r]; const unsigned h=handle(c,r);
            if(!b.pixels) { clear(hw,c,r); continue; }
            if(hw->created[c][r] && (hw->width[c][r]!=b.width || hw->height[c][r]!=b.height)) clear(hw,c,r);
            if(!hw->created[c][r]) {
                MI_TYPE(rgn_cnf) config={.type=(MI_TYPE(rgn_type))0,.pixFmt=(MI_TYPE(rgn_pixfmt))0,.size={b.width,b.height}};
                error=MI_CALL(hw,fnCreateRegion,h,&config);
                if(error) break;
                hw->created[c][r]=true; hw->width[c][r]=b.width; hw->height[c][r]=b.height;
            }
            MI_TYPE(rgn_bmp) bitmap={.pixFmt=(MI_TYPE(rgn_pixfmt))0,.size={b.width,b.height},.data=b.pixels};
            error=MI_CALL(hw,fnSetBitmap,h,&bitmap);
            if(error) break;
            MI_TYPE(sys_bind) dest=MI_DEST(c);
            MI_TYPE(rgn_chn) display={.show=1,.point={b.x,b.y}};
            display.osd.bgFgAlpha[0]=0; display.osd.bgFgAlpha[1]=255;
            error=hw->attached[c][r]?MI_CALL(hw,fnSetChannelConfig,h,&dest,&display):
                MI_CALL(hw,fnAttachChannel,h,&dest,&display);
            if(error) break;
            hw->attached[c][r]=true;
        }
        ca_overlay_free(bits);
        if(error) goto fail;
        hw->previous[c]=s;
    }
    return 0;
fail:
    ca_log("SigmaStar overlay failed: 0x%x",unsigned(error?error:errno));
    ca_overlay_hw_close(*out); *out=nullptr; errno=EIO; return -1;
}
