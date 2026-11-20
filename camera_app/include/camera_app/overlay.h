#ifndef CAMERA_APP_OVERLAY_H
#define CAMERA_APP_OVERLAY_H
#include <stdbool.h>
#include <stdint.h>

#define CA_OVERLAY_REGIONS 9
#define CA_OVERLAY_LINES 12
#ifdef __cplusplus
extern "C" {
#endif

struct ca_overlay_line { int x0, y0, x1, y1; unsigned region; bool dashed; };
struct ca_overlay_geometry {
    struct ca_overlay_line lines[CA_OVERLAY_LINES];
    unsigned count;
    float scale;
};
/* Coordinates refer to the final displayed image. Thermal FOV uses native
 * sensor aspect, not the aspect of a stretched thermal video stream. */
void ca_overlay_geometry(struct ca_overlay_geometry *g, unsigned width, unsigned height,
                         bool cross, bool thermal_box, float rgb_hfov);
void ca_overlay_geometry_fov(struct ca_overlay_geometry *,unsigned width,unsigned height,
                             bool cross,bool thermal_box,float rgb_hfov,float thermal_hfov,float thermal_aspect);
void ca_overlay_tracking(struct ca_overlay_geometry *g, unsigned width, unsigned height,
                         const float rect[4]);
struct ca_overlay_bitmap { unsigned x, y, width, height; uint16_t *pixels; };
/* Small ARGB1555 regions: one cross and four narrow box edges. */
int ca_overlay_bitmaps(struct ca_overlay_bitmap out[CA_OVERLAY_REGIONS],
                      unsigned width, unsigned height, const struct ca_overlay_geometry *g);
void ca_overlay_free(struct ca_overlay_bitmap out[CA_OVERLAY_REGIONS]);
struct ca_overlay_channel {
    unsigned width, height;
    bool cross, thermal_box;
    float hfov;
    bool tracking;
    float rect[4];
};
struct ca_overlay_hw;
int ca_overlay_hw_set(struct ca_overlay_hw **hw, const struct ca_overlay_channel *channels, unsigned count);
void ca_overlay_hw_close(struct ca_overlay_hw *hw);
#ifdef __cplusplus
}
#endif

#endif
