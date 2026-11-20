#ifndef CAMERA_APP_VIDEO_FOV_H
#define CAMERA_APP_VIDEO_FOV_H

#include <math.h>
#include "apcam/APC_Camera.h"
#include "apcam/target.h"

/* Nominal horizontal lens calibration. Zoom scales focal length, not angle. */
#define CA_VISIBLE_HFOV_DEG APCAM_LENS1_FOV_H
#if APCAM_HAVE_THERMAL
#define CA_THERMAL_HFOV_DEG APCAM_LENS3_FOV_H
#else
#define CA_THERMAL_HFOV_DEG 0.0f
#endif

static inline float ca_video_hfov(float native_hfov_deg, float magnification)
{
    return APC_Lens::zoom_FOV(native_hfov_deg, magnification);
}

/* Active image geometry, before encoder stretching or padding. */
static inline float ca_video_vfov(float hfov_deg, unsigned width, unsigned height)
{
    return width && height ? APC_Lens::zoom_FOV(hfov_deg, float(width) / height) : 0.0f;
}

static inline float ca_lens1_hfov(float zoom)
{
    return APC_Camera::get_singleton().lens(0)->get_FOV(zoom);
}

/* The secondary RGB lens has its own calibration, independent of the
 * system-zoom ratio used to select it. */
static inline float ca_zoom_lens_hfov(float optical_zoom, float digital_zoom)
{
#if APCAM_NUM_LENSES > 1 && APCAM_LENS2_TYPE == APCAM_LENS_TYPE_RGB
    return APC_Camera::get_singleton().lens(1)->get_FOV(optical_zoom, digital_zoom);
#else
    (void)optical_zoom; (void)digital_zoom;
    return 0.0f;
#endif
}

#endif
