#include "apcam/APC_Camera.h"
#include "apcam/APC_Config.h"
#include "apcam/gimbal_transform.h"
#include "camera_app/video_fov.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void near(float actual, float expected) { assert(fabsf(actual - expected) < 0.001f); }
int main(void)
{
    const auto *telemetry = APC_Config::find("video", "telemetry");
    assert(telemetry && !strcmp(telemetry->param_name, "VIDEO_META_FMT"));
    assert(telemetry->option_count == 2);
    ca_config settings{};
    ca_config_defaults(&settings);
    assert(settings.video_telemetry_format == CA_VIDEO_TELEMETRY_MISB);
    assert(CA_VIDEO_TELEMETRY_APCG == 0 && CA_VIDEO_TELEMETRY_MISB == 1);
    assert(APC_Config::assign(settings, *telemetry, "apcg"));
    assert(settings.video_telemetry_format == CA_VIDEO_TELEMETRY_APCG);
    assert(APC_Config::assign(settings, *telemetry, "misb"));
    assert(settings.video_telemetry_format == CA_VIDEO_TELEMETRY_MISB);
    assert(!APC_Config::assign(settings, *telemetry, "unknown"));
    const APC_Camera &camera = APC_Camera::get_singleton();
    assert(camera.num_lenses() == APCAM_NUM_LENSES);
    assert(camera.num_streams() == APCAM_NUM_STREAMS);
    assert(camera.has_thermal() == bool(APCAM_HAVE_THERMAL));
    assert(camera.has_zoom() == bool(APCAM_HAVE_ZOOM));
    assert(camera.lens(camera.num_lenses()) == nullptr);
    assert(camera.stream_lens_mask(camera.num_streams()) == 0);
    assert(camera.recording_resolutions() == APCAM_RECORDING_RESOLUTIONS);
    assert(camera.streaming_resolutions(0) == APCAM_MAIN_RESOLUTIONS);
    assert(fabsf(camera.lens(0)->get_FOV() - APCAM_LENS1_FOV_H) < 0.0001f);
    assert(camera.lens(0)->get_FOV(NAN) == 0);
    assert(camera.lens(0)->get_FOV(0) == 0);

    float pose[3] = {5, -20, 30}, out[3];
    apcam_transform(&apcam_feedback[0], pose, out, false);
    near(out[0], 5); near(out[1], -20);
#if APCAM_TARGET == APCAM_TARGET_MT11 || APCAM_TARGET == APCAM_TARGET_A8 || APCAM_TARGET == APCAM_TARGET_ZR10 || APCAM_TARGET == APCAM_TARGET_ZT30
    near(out[2], -30);
#else
    near(out[2], 30);
#endif
#if APCAM_TARGET == APCAM_TARGET_A8 || APCAM_TARGET == APCAM_TARGET_ZR10
    pose[1] = -160;
#endif
    apcam_transform(&apcam_feedback[1], pose, out, false);
    near(out[1], -20);
#if APCAM_TARGET == APCAM_TARGET_MT11 || APCAM_TARGET == APCAM_TARGET_ZR10
    near(out[2], -30);
#else
    near(out[2], 30);
#endif
    float rates[3] = {1, 2, 3};
    apcam_transform(&apcam_feedback[1], rates, out, true);
#if APCAM_TARGET == APCAM_TARGET_A8 || APCAM_TARGET == APCAM_TARGET_ZR10
    near(out[1], -2);
#else
    near(out[1], 2);
#endif
#if APCAM_TARGET == APCAM_TARGET_ZR10
    near(ca_lens1_hfov(1), 71.5f);
    near(ca_lens1_hfov(10), 6.7f);
    for (unsigned i = 11; i <= 100; i++)
        assert(ca_lens1_hfov(i * 0.1f) < ca_lens1_hfov((i - 1) * 0.1f));
    assert(ca_lens1_hfov(NAN) == 0);
    assert(ca_lens1_hfov(INFINITY) == 0);
    assert(ca_lens1_hfov(0) == 0);
    assert(ca_lens1_hfov(30) == 0);
#elif APCAM_TARGET == APCAM_TARGET_ZT30
    near(ca_lens1_hfov(1), 84.5f);
    near(ca_video_vfov(ca_lens1_hfov(1), 2560,1440), 54.128524f);
    near(ca_zoom_lens_hfov(1,1),58.1f);
    near(ca_zoom_lens_hfov(30,1),2.14f);
    near(ca_video_vfov(ca_zoom_lens_hfov(30,1),3840,2160),1.203846f);
    near(ca_zoom_lens_hfov(30,6),ca_video_hfov(2.14f,6));
    assert(ca_zoom_lens_hfov(31,1)==0);
    assert(camera.lens(0)->digital_zoom_max()==1);
    assert(camera.lens(1)->optical_zoom_max()==30 && camera.lens(1)->digital_zoom_max()==6);
    near(camera.lens(2)->get_FOV(),24.2f); // Assumed MT11 thermal FOV.
    assert(!apcam_uses_zoom_lens(1) && !apcam_uses_zoom_lens(90));
    near(apcam_zoom_lens_optical(90),30);
#elif APCAM_TARGET == APCAM_TARGET_Z1_MINI
    near(ca_lens1_hfov(1), 54.7f);
    assert(APCAM_MAIN_RESOLUTIONS == APCAM_RES_MASK_1080P);
    assert(APCAM_RECORDING_RESOLUTIONS & APCAM_RES_MASK_2160P);
#elif APCAM_TARGET == APCAM_TARGET_RPI_LIBCAM_CADDX
    near(ca_lens1_hfov(1), 70.0f);
    /* Up to 1080p, the hardware encoder's limit; smaller sizes save bandwidth. */
    assert(APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_1080P);
    assert(APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_720P);
    assert(APCAM_MAIN_RESOLUTIONS & APCAM_RES_MASK_180P);
    assert(!(APCAM_MAIN_RESOLUTIONS & (APCAM_RES_MASK_1440P | APCAM_RES_MASK_2160P)));
    assert(APCAM_RECORDING_RESOLUTIONS == APCAM_RES_MASK_1080P);
    assert(!APCAM_HAVE_NETWORK_CONFIG && !APCAM_HAVE_SITL);
    assert(APCAM_VENDOR_PROTOCOL == APCAM_PROTOCOL_NONE && !APCAM_HAVE_SIYI && !APCAM_HAVE_XFROBOT);
#elif APCAM_TARGET == APCAM_TARGET_MT11
    near(ca_lens1_hfov(1), 71.532165f);
    near(ca_video_vfov(ca_lens1_hfov(1), 3840, 2160), 44.113793f);
    near(ca_video_vfov(ca_lens1_hfov(2), 1920, 1080),
         ca_video_hfov(44.113793f, 2));
    assert(ca_video_vfov(NAN, 1920, 1080) == 0);
    assert(ca_video_vfov(71.532165f, 0, 1080) == 0);
    assert(ca_video_vfov(71.532165f, 1920, 0) == 0);
#else
    near(ca_lens1_hfov(1), 88);
    assert(ca_lens1_hfov(2) > 51 && ca_lens1_hfov(2) < 52);
#endif
#if APCAM_TARGET == APCAM_TARGET_MT11
    near(APCAM_GIMBAL_RATE_MAX, 100);
    near(ca_zoom_lens_hfov(1, 1), 31.3613561f);
    near(ca_zoom_lens_hfov(2, 1), ca_zoom_lens_hfov(1, 2));
    assert(ca_zoom_lens_hfov(2, 1) < ca_zoom_lens_hfov(1, 1));
#endif
    puts(APCAM_NAME " target transforms and lens calibration passed");
    return 0;
}
