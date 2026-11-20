#ifndef APCAM_LENS_H
#define APCAM_LENS_H
#include "target.h"
/* Targets may select RGB lenses from a wide-relative system zoom or retain
 * zoom-module state independently from the selected RGB/thermal scene. */
#if APCAM_NUM_LENSES > 1 && APCAM_LENS2_TYPE == APCAM_LENS_TYPE_RGB
#define APCAM_HAVE_ZOOM_LENS 1
#define APCAM_ZOOM_LENS_BASE APCAM_LENS2_BASE_MAGNIFICATION
#define APCAM_ZOOM_LENS_OPTICAL_MAX APCAM_LENS2_OPTICAL_ZOOM_MAX
#else
#define APCAM_HAVE_ZOOM_LENS 0
#define APCAM_ZOOM_LENS_BASE 1.0f
#define APCAM_ZOOM_LENS_OPTICAL_MAX 1.0f
#endif
static inline int apcam_uses_zoom_lens(float zoom)
{
#if APCAM_NUM_LENSES > 1 && APCAM_LENS2_TYPE == APCAM_LENS_TYPE_RGB
    return APCAM_ZOOM_SELECTS_LENS && zoom > APCAM_ZOOM_LENS_BASE;
#else
    (void)zoom;
    return 0;
#endif
}
static inline float apcam_zoom_lens_optical(float zoom)
{
    if (!APCAM_ZOOM_SELECTS_LENS)
        return zoom < APCAM_ZOOM_LENS_OPTICAL_MAX ? zoom : APCAM_ZOOM_LENS_OPTICAL_MAX;
    if (!apcam_uses_zoom_lens(zoom)) return 1.0f;
    unsigned tenths = (unsigned)(zoom * 10.0f / APCAM_ZOOM_LENS_BASE + 0.0001f);
    if (tenths < 10U) tenths = 10U;
    if (tenths > (unsigned)(APCAM_ZOOM_LENS_OPTICAL_MAX * 10))
        tenths = (unsigned)(APCAM_ZOOM_LENS_OPTICAL_MAX * 10);
    return tenths * 0.1f;
}
#endif
