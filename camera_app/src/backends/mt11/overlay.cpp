#include "camera_app/overlay.h"
#include "camera_app/log.h"
#include "ss_mpi_region.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct ca_overlay_hw {
    bool created[4][CA_OVERLAY_REGIONS], attached[4][CA_OVERLAY_REGIONS];
    struct ca_overlay_channel previous[4];
    unsigned width[4][CA_OVERLAY_REGIONS], height[4][CA_OVERLAY_REGIONS];
};
static unsigned handle(unsigned channel, unsigned region) { return channel*CA_OVERLAY_REGIONS+region; }
static void clear(struct ca_overlay_hw *hw, unsigned c, unsigned r)
{
    ot_mpp_chn chn={.mod_id=OT_ID_VENC,.dev_id=0,.chn_id=(td_s32)(c)};
    if (hw->attached[c][r]) (void)ss_mpi_rgn_detach_from_chn(handle(c,r),&chn);
    if (hw->created[c][r]) (void)ss_mpi_rgn_destroy(handle(c,r));
    hw->attached[c][r]=hw->created[c][r]=false;
}
void ca_overlay_hw_close(struct ca_overlay_hw *hw)
{
    if (!hw) return;
    for (unsigned c=0;c<4;c++) for (unsigned r=0;r<CA_OVERLAY_REGIONS;r++) clear(hw,c,r);
    free(hw);
}
int ca_overlay_hw_set(struct ca_overlay_hw **out, const struct ca_overlay_channel *channels, unsigned count)
{
    if (count>4) { errno=EINVAL; return -1; }
    bool enabled=false;
    for (unsigned c=0;c<count;c++) enabled |= channels[c].cross || channels[c].thermal_box || channels[c].tracking;
    if (!enabled) { ca_overlay_hw_close(*out); *out=NULL; return 0; }
    if (!*out && !(*out=(struct ca_overlay_hw*)(calloc(1,sizeof(**out))))) return -1;
    struct ca_overlay_hw *hw=*out;
    for (unsigned c=0;c<count;c++) {
        const struct ca_overlay_channel *s=&channels[c], *old=&hw->previous[c];
        if (s->width==old->width && s->height==old->height && s->cross==old->cross &&
            s->thermal_box==old->thermal_box && (!s->thermal_box || s->hfov==old->hfov) &&
            s->tracking==old->tracking && (!s->tracking || !memcmp(s->rect,old->rect,sizeof(s->rect)))) continue;
        struct ca_overlay_geometry g;
        struct ca_overlay_bitmap bits[CA_OVERLAY_REGIONS];
        ca_overlay_geometry(&g,s->width,s->height,s->cross,s->thermal_box && !s->tracking,s->hfov);
        if(s->tracking) ca_overlay_tracking(&g,s->width,s->height,s->rect);
        if (ca_overlay_bitmaps(bits,s->width,s->height,&g)<0) return -1;
        int error=0;
        // Tracking and thermal edges share layer slots. Release the outgoing
        // set before attaching replacements in either direction.
        for (unsigned r=0;r<CA_OVERLAY_REGIONS;r++)
            if (!bits[r].pixels) clear(hw,c,r);
        for (unsigned r=0;r<CA_OVERLAY_REGIONS;r++) {
            struct ca_overlay_bitmap *b=&bits[r];
            if (!b->pixels) continue;
            /* The centre cross does not change when the RGB lens zooms. */
            if (r==0 && hw->attached[c][r] && old->cross==s->cross &&
                old->width==s->width && old->height==s->height) continue;
            if (hw->created[c][r] && (hw->width[c][r]!=b->width || hw->height[c][r]!=b->height))
                clear(hw,c,r);
            ot_rgn_attr attr={.type=OT_RGN_OVERLAY};
            attr.attr.overlay.pixel_format=OT_PIXEL_FORMAT_ARGB_1555;
            attr.attr.overlay.size=(ot_size){b->width,b->height};
            attr.attr.overlay.canvas_num=2;
            if (!hw->created[c][r]) {
                error=ss_mpi_rgn_create(handle(c,r),&attr);
                if (error) break;
                hw->created[c][r]=true;
                hw->width[c][r]=b->width; hw->height[c][r]=b->height;
            }
            ot_bmp bmp={.pixel_format=OT_PIXEL_FORMAT_ARGB_1555,.width=b->width,.height=b->height,.data=b->pixels};
            error=ss_mpi_rgn_set_bmp(handle(c,r),&bmp);
            if (error) break;
            ot_mpp_chn chn={.mod_id=OT_ID_VENC,.dev_id=0,.chn_id=(td_s32)(c)};
            ot_rgn_chn_attr display={.is_show=TD_TRUE,.type=OT_RGN_OVERLAY};
            display.attr.overlay_chn.point=(ot_point){(td_s32)(b->x),(td_s32)(b->y)};
            display.attr.overlay_chn.fg_alpha=128;
            display.attr.overlay_chn.bg_alpha=0;
            // Tracking replaces the four thermal-FOV edges, so reuse their
            // layer slots instead of exceeding the SDK's 0..7 layer range.
            display.attr.overlay_chn.layer=r>=5 ? r-4 : r;
            error=hw->attached[c][r] ? ss_mpi_rgn_set_display_attr(handle(c,r),&chn,&display) :
                ss_mpi_rgn_attach_to_chn(handle(c,r),&chn,&display);
            if (error) break;
            hw->attached[c][r]=true;
        }
        ca_overlay_free(bits);
        if (error) {
            ca_log("MT11 overlay channel %u failed: 0x%x",c,(unsigned)error);
            /* Invalidate all cached state so caller rollback rebuilds it. */
            ca_overlay_hw_close(hw); *out=NULL; errno=EIO; return -1;
        }
        hw->previous[c]=*s;
    }
    return 0;
}
