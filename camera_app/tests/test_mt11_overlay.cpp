// Model the SDK's unique layer ownership during thermal/tracking transitions.
#include "camera_app/overlay.h"
#include "ss_mpi_region.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
static bool created[36], attached[36];
static unsigned layers[36];
void ca_log(const char *, ...) {}
extern "C" {
td_s32 ss_mpi_rgn_create(ot_rgn_handle h, const ot_rgn_attr *)
{ assert(!created[h]); created[h]=true; return 0; }
td_s32 ss_mpi_rgn_destroy(ot_rgn_handle h)
{ assert(created[h] && !attached[h]); created[h]=false; return 0; }
td_s32 ss_mpi_rgn_set_bmp(ot_rgn_handle h, const ot_bmp *)
{ assert(created[h]); return 0; }
td_s32 ss_mpi_rgn_attach_to_chn(ot_rgn_handle h, const ot_mpp_chn *, const ot_rgn_chn_attr *a)
{
    assert(created[h] && !attached[h]);
    const auto layer=a->attr.overlay_chn.layer;
    for (unsigned i=0;i<36;i++) if (attached[i] && i/CA_OVERLAY_REGIONS==h/CA_OVERLAY_REGIONS)
        assert(layers[i]!=layer);
    layers[h]=layer; attached[h]=true; return 0;
}
td_s32 ss_mpi_rgn_detach_from_chn(ot_rgn_handle h, const ot_mpp_chn *)
{ assert(attached[h]); attached[h]=false; return 0; }
td_s32 ss_mpi_rgn_set_display_attr(ot_rgn_handle h, const ot_mpp_chn *, const ot_rgn_chn_attr *a)
{ assert(attached[h] && layers[h]==a->attr.overlay_chn.layer); return 0; }
}
int main()
{
    ca_overlay_hw *hw=nullptr;
    ca_overlay_channel c {};
    c.width=1280; c.height=720; c.cross=true; c.thermal_box=true; c.hfov=88;
    c.rect[0]=.1f; c.rect[1]=.1f; c.rect[2]=.3f; c.rect[3]=.3f;
    for (bool tracking : {false,true,false,true,false}) {
        c.tracking=tracking;
        assert(ca_overlay_hw_set(&hw,&c,1)==0);
        assert(attached[0]);
        for (unsigned i=1;i<9;i++) assert(attached[i]==(tracking ? i>=5 : i<5));
    }
    ca_overlay_hw_close(hw);
    for (unsigned i=0;i<36;i++) assert(!created[i] && !attached[i]);
    puts("PASS MT11 thermal/tracking layer transitions in both directions");
}
