#ifndef CAMERA_APP_MEDIA_H
#define CAMERA_APP_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_app/config.h"
#include "camera_app/thermal.h"
#include "camera_app/image_tracker.h"

struct ca_media;
struct ca_backend;
struct ca_survey_request;
struct ca_survey_result;
float ca_media_survey_hfov(const ca_media *, unsigned lens);
bool ca_media_survey_available(struct ca_media *, unsigned lens);
bool ca_media_survey_start(struct ca_media *, const ca_survey_request &);
bool ca_media_survey_poll(struct ca_media *, ca_survey_result &);
void ca_media_survey_cancel(struct ca_media *);
bool ca_media_tracking_available(struct ca_media *media);
int ca_media_tracking_start(struct ca_media *media, ca_tracking_rect rect, ca_tracking_owner owner);
void ca_media_tracking_stop(struct ca_media *media, ca_tracking_owner owner=CA_TRACK_OWNER_NONE);
ca_tracking_status ca_media_tracking_status(struct ca_media *media);
void ca_media_tracking_update(struct ca_media *media, struct ca_backend *backend, bool manual);

enum ca_media_lens {
    CA_MEDIA_LENS_WIDE = 0,
    CA_MEDIA_LENS_ZOOM = 1,
};

struct ca_media_config {
    const char *backend;
    const char *record_root;
    const char *capture_root;
    unsigned rtsp_port;
    unsigned width;
    unsigned height;
    unsigned frame_rate;
    unsigned bit_rate_kbps;
    struct ca_config settings;
};

/* These calls run on the app/control thread. The handle stays valid across
 * pipeline rebuilds, including all backend callback opaque pointers. */
int ca_media_configure(struct ca_media *media, const struct ca_config *settings);
const struct ca_config *ca_media_settings(const struct ca_media *media);
bool ca_media_ready(const struct ca_media *media);
void ca_media_defer_autorecord(struct ca_media *media);
void ca_media_periodic(struct ca_media *media);
int ca_media_open(struct ca_media **media, const struct ca_media_config *config);
int ca_media_set_recording(struct ca_media *media, bool active);
bool ca_media_recording(const struct ca_media *media);
const char *ca_media_recording_path(const struct ca_media *media);
int ca_media_set_zoom(struct ca_media *media, float zoom);
float ca_media_zoom(const struct ca_media *media);
/* Lens-relative zoom, independent of vendor hybrid lens selection. */
int ca_media_set_lens_zoom(struct ca_media *media, enum ca_media_lens lens, float zoom);
float ca_media_lens_zoom(const struct ca_media *media, enum ca_media_lens lens);
/* Effective FOV of the visible/thermal sensor, including current zoom. */
float ca_media_hfov(const struct ca_media *media, bool thermal);
unsigned ca_media_frame_rate(const struct ca_media *media, bool thermal);
int ca_media_set_lens(struct ca_media *media, enum ca_media_lens lens);
enum ca_media_lens ca_media_lens(const struct ca_media *media);
int ca_media_set_thermal_main(struct ca_media *media, bool thermal_main);
bool ca_media_thermal_main(const struct ca_media *media);
int ca_media_set_side_by_side(struct ca_media *media, bool enabled);
bool ca_media_side_by_side(const struct ca_media *media);
int ca_media_autofocus(struct ca_media *media, uint16_t x, uint16_t y);
int ca_media_manual_focus(struct ca_media *media, int direction);
int ca_media_set_focus_percent(struct ca_media *media, float percent);
bool ca_media_thermal_range(struct ca_media *media,
                            struct ca_thermal_range *range);
bool ca_media_photo_available(const struct ca_media *);
void ca_media_set_photo_prepare(struct ca_media *,int (*fn)(void*),void *opaque);
int ca_media_capture_photo(struct ca_media *media, enum ca_photo_scope scope);
/* Cached values for diagnostic sampling; never performs sensor I/O. */
bool ca_media_cached_thermal_controls(struct ca_media *media, uint8_t *gain, uint8_t *palette);
int ca_media_get_thermal_gain(struct ca_media *media, uint8_t *gain);
int ca_media_set_thermal_gain(struct ca_media *media, uint8_t gain);
int ca_media_get_thermal_palette(struct ca_media *media, uint8_t *palette);
int ca_media_set_thermal_palette(struct ca_media *media, uint8_t palette);
int ca_media_set_inverted(struct ca_media *media, bool inverted);
void ca_media_close(struct ca_media *media);

#endif
