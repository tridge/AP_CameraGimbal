#define _GNU_SOURCE
#include "camera_app/media_impl.h"
#include "camera_app/log.h"
#include "camera_app/binlog.h"
#include "camera_app/metadata.h"
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include "apcam/target.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>

struct ca_media {
    struct ca_media_impl *impl;
    struct ca_media_config config;
    bool inverted;
    uint8_t cached_gain, cached_palette;
    bool cached_thermal;
    pthread_mutex_t controls_lock;
    pthread_cond_t controls_wake;
    pthread_t controls_thread;
    bool controls_running, controls_stop;
    unsigned controls_generation;
    pthread_t exposure_thread;
    bool exposure_running;
    pthread_cond_t exposure_wake;
};

static void apply_overlay_after_control(struct ca_media *media, const char *control)
{
    if (ca_media_impl_apply_overlay(media->impl, &media->config.settings) < 0) {
        ca_log("video overlay update failed after %s; control completed: %s",
               control, strerror(errno));
    }
}

/* Thermal USB transactions can take hundreds of milliseconds. Refresh their
 * diagnostic cache off the control loop, never holding the cache lock over I/O. */
static void *monitor_controls(void *opaque)
{
    struct ca_media *media=opaque;
    pthread_mutex_lock(&media->controls_lock);
    while (!media->controls_stop) {
        unsigned generation=media->controls_generation;
        pthread_mutex_unlock(&media->controls_lock);
        uint8_t gain,palette;
        bool valid=ca_media_impl_get_thermal_gain(media->impl,&gain)==0 &&
            ca_media_impl_get_thermal_palette(media->impl,&palette)==0;
        pthread_mutex_lock(&media->controls_lock);
        if (valid && generation==media->controls_generation) {
            media->cached_gain=gain; media->cached_palette=palette;
            media->cached_thermal=true;
        }
        if (!media->controls_stop) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME,&until);
            until.tv_sec++;
            pthread_cond_timedwait(&media->controls_wake,&media->controls_lock,&until);
        }
    }
    pthread_mutex_unlock(&media->controls_lock);
    return NULL;
}
/* Independent of slow thermal USB reads. The backend is joined before any
 * pipeline teardown, including reconfiguration and rollback. */
static void *monitor_exposure(void *opaque)
{
    struct ca_media *media=opaque;
    pthread_mutex_lock(&media->controls_lock);
    while (!media->controls_stop) {
        pthread_mutex_unlock(&media->controls_lock);
        if (ca_binlog_active()) {
            for (unsigned lens=0; lens<APCAM_NUM_LENSES; lens++) {
                struct ca_exposure sample=ca_exposure_empty(lens,ca_binlog_time_us());
                sample.result=ca_media_impl_exposure(media->impl,lens,&sample);
                ca_binlog_emit(CA_LOG_AE,&sample,sizeof(sample));
            }
        }
        pthread_mutex_lock(&media->controls_lock);
        if (!media->controls_stop) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME,&until);
            until.tv_nsec+=200000000;
            if (until.tv_nsec>=1000000000) { until.tv_sec++; until.tv_nsec-=1000000000; }
            pthread_cond_timedwait(&media->exposure_wake,&media->controls_lock,&until);
        }
    }
    pthread_mutex_unlock(&media->controls_lock);
    return NULL;
}
static void stop_controls_monitor(struct ca_media *media)
{
    if (!media->controls_running && !media->exposure_running) return;
    pthread_mutex_lock(&media->controls_lock);
    media->controls_stop=true;
    pthread_cond_signal(&media->controls_wake);
    pthread_cond_signal(&media->exposure_wake);
    pthread_mutex_unlock(&media->controls_lock);
    if (media->controls_running) pthread_join(media->controls_thread,NULL);
    if (media->exposure_running) pthread_join(media->exposure_thread,NULL);
    media->exposure_running=false;
    media->controls_running=false;
}
static void start_controls_monitor(struct ca_media *media)
{
    if (!media->impl || media->controls_running || media->exposure_running) return;
    media->controls_stop=false;
    media->cached_thermal=false;
    int error=pthread_create(&media->exposure_thread,NULL,monitor_exposure,media);
    if (error) ca_log("cannot start exposure monitor: %s",strerror(error));
    else media->exposure_running=true;
    if (!APCAM_HAVE_THERMAL) return;
    error=pthread_create(&media->controls_thread,NULL,monitor_controls,media);
    if (error) ca_log("cannot start thermal controls monitor: %s",strerror(error));
    else media->controls_running=true;
}

int ca_media_open(struct ca_media **result, const struct ca_media_config *config)
{
    if (!result || !config) { errno = EINVAL; return -1; }
    struct ca_media *media = calloc(1, sizeof(*media));
    if (!media) return -1;
    pthread_mutex_init(&media->controls_lock,NULL);
    pthread_cond_init(&media->controls_wake,NULL);
    pthread_cond_init(&media->exposure_wake,NULL);
    media->config = *config;
    media->inverted = config->settings.orientation == CA_MOUNT_INVERTED;
    if (ca_media_impl_open(&media->impl, config) < 0) {
        pthread_cond_destroy(&media->controls_wake);
        pthread_cond_destroy(&media->exposure_wake);
        pthread_mutex_destroy(&media->controls_lock);
        free(media); return -1;
    }
    if (ca_media_impl_apply_overlay(media->impl, &config->settings) < 0) {
        ca_log("initial video overlay could not be applied; camera remains available: %s",
               strerror(errno));
    }
    start_controls_monitor(media);
    *result = media;
    return 0;
}

const struct ca_config *ca_media_settings(const struct ca_media *media)
{
    return &media->config.settings;
}

struct live_controls {
    float zoom;
    enum ca_media_lens lens;
    bool thermal_main;
    int gain, palette;
};

static int restore_controls(struct ca_media *media, const struct live_controls *state)
{
    int result = 0;
    if (ca_media_impl_set_inverted(media->impl, media->inverted) < 0) result = -1;
    if (APCAM_NUM_LENSES > 1 && ca_media_impl_set_lens(media->impl, state->lens) < 0) result = -1;
    if (APCAM_HAVE_ZOOM && isfinite(state->zoom) &&
        ca_media_impl_set_zoom(media->impl, state->zoom) < 0) result = -1;
    if (APCAM_HAVE_THERMAL) {
        if (ca_media_impl_set_thermal_main(media->impl, state->thermal_main) < 0) result = -1;
        if (state->gain >= 0 && ca_media_impl_set_thermal_gain(media->impl, (uint8_t)state->gain) < 0) result = -1;
        if (state->palette >= 0 && ca_media_impl_set_thermal_palette(media->impl, (uint8_t)state->palette) < 0) result = -1;
    }
    return result;
}

int ca_media_configure(struct ca_media *media, const struct ca_config *settings)
{
    if (!media || !settings) { errno = EINVAL; return -1; }
    const struct ca_config *old = &media->config.settings;
    bool pipeline = !media->impl || old->main_resolution != settings->main_resolution ||
        old->sub_resolution != settings->sub_resolution ||
        old->recording_resolution != settings->recording_resolution ||
        old->main_codec != settings->main_codec || old->sub_codec != settings->sub_codec;
    if (pipeline) {
        if (media->impl && ca_media_impl_recording(media->impl)) { errno = EBUSY; return -1; }
        struct live_controls state = {.zoom = 1, .gain = -1, .palette = -1};
        if (media->impl) {
            state.zoom = ca_media_impl_zoom(media->impl);
            state.lens = ca_media_impl_lens(media->impl);
            state.thermal_main = ca_media_impl_thermal_main(media->impl);
            uint8_t value;
            if (APCAM_HAVE_THERMAL && ca_media_impl_get_thermal_gain(media->impl, &value) == 0) state.gain = value;
            if (APCAM_HAVE_THERMAL && ca_media_impl_get_thermal_palette(media->impl, &value) == 0) state.palette = value;
        }
        struct ca_media_config next = media->config;
        next.settings = *settings;
        stop_controls_monitor(media);
        ca_media_impl_close(media->impl);
        media->impl = NULL;
        ca_log("reconfiguring media pipeline without restarting camera app");
        if (ca_media_impl_open(&media->impl, &next) < 0 || restore_controls(media, &state) < 0 ||
            ca_media_impl_apply_overlay(media->impl, settings) < 0) {
            int saved_errno = errno;
            ca_media_impl_close(media->impl);
            media->impl = NULL;
            if (ca_media_impl_open(&media->impl, &media->config) < 0 || restore_controls(media, &state) < 0 ||
                ca_media_impl_apply_overlay(media->impl, old) < 0)
                ca_log("media configuration rollback failed; retry configuration");
            start_controls_monitor(media);
            errno = saved_errno ? saved_errno : EIO;
            return -1;
        }
        start_controls_monitor(media);
        media->config = next;
        return 0;
    }
    if (!ca_config_image_equal(old, settings)) {
        if (ca_media_impl_apply_image(media->impl, settings) < 0) {
            int saved_errno = errno;
            if (ca_media_impl_apply_image(media->impl, old) < 0)
                ca_log("image configuration rollback failed");
            errno = saved_errno;
            return -1;
        }
    }
    if (old->osd_cross != settings->osd_cross || old->osd_thermal_fov != settings->osd_thermal_fov ||
        old->osd_recording != settings->osd_recording) {
        if (ca_media_impl_apply_overlay(media->impl, settings) < 0) {
            int saved = errno;
            (void)ca_media_impl_apply_overlay(media->impl, old);
            if (!ca_config_image_equal(old, settings))
                (void)ca_media_impl_apply_image(media->impl, old);
            errno = saved;
            return -1;
        }
    }
    media->config.settings = *settings;
    return 0;
}

void ca_media_close(struct ca_media *media)
{
    if (!media) return;
    stop_controls_monitor(media);
    if (ca_media_recording(media)) (void)ca_media_set_recording(media, false);
    ca_media_impl_close(media->impl);
    pthread_cond_destroy(&media->controls_wake);
    pthread_cond_destroy(&media->exposure_wake);
    pthread_mutex_destroy(&media->controls_lock);
    free(media);
}

/* The stable handle is retained by gimbal callbacks and the MAVLink server;
 * only the private implementation changes during pipeline reconfiguration. */
#define IMPL (media ? media->impl : NULL)
#define REQUIRE_IMPL do { if (!IMPL) { errno = ENODEV; return -1; } } while (0)
bool ca_media_ready(const struct ca_media *media)
{ return IMPL && ca_media_impl_ready(IMPL); }
int ca_media_set_recording(struct ca_media *media, bool active)
{
    REQUIRE_IMPL;
    bool previous=ca_media_impl_recording(IMPL);
    struct ca_log_vid r={.time_us=ca_binlog_time_us(),.active=active};
    const char *path=ca_media_impl_recording_path(IMPL);
    if (path) snprintf(r.path,sizeof(r.path),"%s",path);
    r.result=ca_media_impl_set_recording(IMPL,active);
    int saved_errno=errno;
    if (active) {
        path=ca_media_impl_recording_path(IMPL);
        if (path) snprintf(r.path,sizeof(r.path),"%s",path);
    }
    if (previous!=active || r.result<0) ca_binlog_emit(CA_LOG_VID,&r,sizeof(r));
    errno=saved_errno;
    return r.result;
}
bool ca_media_recording(const struct ca_media *media)
{ return IMPL && ca_media_impl_recording(IMPL); }
const char *ca_media_recording_path(const struct ca_media *media)
{ return IMPL ? ca_media_impl_recording_path(IMPL) : NULL; }
int ca_media_set_zoom(struct ca_media *media, float zoom)
{
    REQUIRE_IMPL;
    int result=ca_media_impl_set_zoom(IMPL, zoom);
    int saved=errno;
    if (result<0) { errno=saved; return result; }
    apply_overlay_after_control(media, "zoom");
    return 0;
}
float ca_media_zoom(const struct ca_media *media)
{ return IMPL ? ca_media_impl_zoom(IMPL) : 1; }
float ca_media_hfov(const struct ca_media *media, bool thermal)
{ return IMPL ? ca_media_impl_hfov(IMPL, thermal) : NAN; }
unsigned ca_media_frame_rate(const struct ca_media *media, bool thermal)
{ return IMPL ? ca_media_impl_frame_rate(IMPL, thermal) : 0; }
int ca_media_set_lens(struct ca_media *media, enum ca_media_lens lens)
{
    REQUIRE_IMPL;
    int result=ca_media_impl_set_lens(IMPL, lens);
    int saved=errno;
    if (result<0) { errno=saved; return result; }
    apply_overlay_after_control(media, "lens change");
    return 0;
}
enum ca_media_lens ca_media_lens(const struct ca_media *media)
{ return IMPL ? ca_media_impl_lens(IMPL) : CA_MEDIA_LENS_WIDE; }
int ca_media_set_thermal_main(struct ca_media *media, bool thermal_main)
{
    REQUIRE_IMPL;
    int result=ca_media_impl_set_thermal_main(IMPL, thermal_main);
    int saved=errno;
    if (result<0) { errno=saved; return result; }
    apply_overlay_after_control(media, "video source change");
    return 0;
}
bool ca_media_thermal_main(const struct ca_media *media)
{ return IMPL && ca_media_impl_thermal_main(IMPL); }
int ca_media_autofocus(struct ca_media *media, uint16_t x, uint16_t y)
{ REQUIRE_IMPL; return ca_media_impl_autofocus(IMPL, x, y); }
int ca_media_manual_focus(struct ca_media *media, int direction)
{ REQUIRE_IMPL; return ca_media_impl_manual_focus(IMPL, direction); }
int ca_media_set_focus_percent(struct ca_media *media, float percent)
{ REQUIRE_IMPL; return ca_media_impl_set_focus_percent(IMPL, percent); }
bool ca_media_thermal_range(struct ca_media *media, struct ca_thermal_range *range)
{ return IMPL && ca_media_impl_thermal_range(IMPL, range); }
int ca_media_capture_photo(struct ca_media *media, enum ca_photo_scope scope)
{
    REQUIRE_IMPL;
    struct ca_metadata snapshot;
    ca_metadata_snapshot(&snapshot);
    struct ca_log_cam r={.time_us=ca_binlog_time_us(),.scope=scope,
        .lat=snapshot.lat_e7,.lon=snapshot.lon_e7,.alt=snapshot.alt_amsl_m,
        .roll=snapshot.gimbal_roll_rad*57.295779513f,
        .pitch=snapshot.gimbal_pitch_rad*57.295779513f,
        .yaw=snapshot.gimbal_yaw_rad*57.295779513f};
    r.result=ca_media_impl_capture_photo(IMPL,scope);
    int saved_errno=errno;
    ca_binlog_emit(CA_LOG_CAM,&r,sizeof(r));
    errno=saved_errno;
    return r.result;
}
bool ca_media_cached_thermal_controls(struct ca_media *media, uint8_t *gain, uint8_t *palette)
{
    if (!media) return false;
    pthread_mutex_lock(&media->controls_lock);
    bool valid=media->cached_thermal;
    *gain=media->cached_gain; *palette=media->cached_palette;
    pthread_mutex_unlock(&media->controls_lock);
    return valid;
}
int ca_media_get_thermal_gain(struct ca_media *media, uint8_t *gain)
{ REQUIRE_IMPL; int result=ca_media_impl_get_thermal_gain(IMPL, gain);
  if (!result) {
      pthread_mutex_lock(&media->controls_lock);
      media->cached_gain=*gain;
      media->controls_generation++;
      pthread_mutex_unlock(&media->controls_lock);
  }
  return result; }
int ca_media_set_thermal_gain(struct ca_media *media, uint8_t gain)
{ REQUIRE_IMPL; int result=ca_media_impl_set_thermal_gain(IMPL, gain);
  if (!result) {
      pthread_mutex_lock(&media->controls_lock);
      media->cached_gain=gain;
      media->controls_generation++;
      pthread_mutex_unlock(&media->controls_lock);
  }
  return result; }
int ca_media_get_thermal_palette(struct ca_media *media, uint8_t *palette)
{ REQUIRE_IMPL; int result=ca_media_impl_get_thermal_palette(IMPL, palette);
  if (!result) {
      pthread_mutex_lock(&media->controls_lock);
      media->cached_palette=*palette;
      media->controls_generation++;
      pthread_mutex_unlock(&media->controls_lock);
  }
  return result; }
int ca_media_set_thermal_palette(struct ca_media *media, uint8_t palette)
{ REQUIRE_IMPL; int result=ca_media_impl_set_thermal_palette(IMPL, palette);
  if (!result) {
      pthread_mutex_lock(&media->controls_lock);
      media->cached_palette=palette;
      media->controls_generation++;
      pthread_mutex_unlock(&media->controls_lock);
  }
  return result; }
int ca_media_set_inverted(struct ca_media *media, bool inverted)
{
    REQUIRE_IMPL;
    if (ca_media_impl_set_inverted(IMPL, inverted) < 0) return -1;
    media->inverted = inverted;
    return 0;
}
