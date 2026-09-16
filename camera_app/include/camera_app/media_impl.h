#ifndef CAMERA_APP_MEDIA_IMPL_H
#define CAMERA_APP_MEDIA_IMPL_H
#include "camera_app/media.h"
#include "camera_app/exposure.h"

/* Hardware/SITL implementation behind the stable public media handle. */
struct ca_media_impl;
bool ca_media_impl_ready(const struct ca_media_impl *media);
int ca_media_impl_open(struct ca_media_impl **media, const struct ca_media_config *config);
int ca_media_impl_set_recording(struct ca_media_impl *media, bool active);
bool ca_media_impl_recording(const struct ca_media_impl *media);
const char *ca_media_impl_recording_path(const struct ca_media_impl *media);
int ca_media_impl_set_zoom(struct ca_media_impl *media, float zoom);
float ca_media_impl_zoom(const struct ca_media_impl *media);
/* Effective FOV of the visible/thermal sensor, including current zoom. */
float ca_media_impl_hfov(const struct ca_media_impl *media, bool thermal);
unsigned ca_media_impl_frame_rate(const struct ca_media_impl *media, bool thermal);
int ca_media_impl_set_lens(struct ca_media_impl *media, enum ca_media_lens lens);
enum ca_media_lens ca_media_impl_lens(const struct ca_media_impl *media);
int ca_media_impl_set_thermal_main(struct ca_media_impl *media, bool thermal_main);
bool ca_media_impl_thermal_main(const struct ca_media_impl *media);
int ca_media_impl_autofocus(struct ca_media_impl *media, uint16_t x, uint16_t y);
int ca_media_impl_manual_focus(struct ca_media_impl *media, int direction);
int ca_media_impl_set_focus_percent(struct ca_media_impl *media, float percent);
bool ca_media_impl_thermal_range(struct ca_media_impl *media,
                            struct ca_thermal_range *range);
int ca_media_impl_capture_photo(struct ca_media_impl *media, enum ca_photo_scope scope);
int ca_media_impl_get_thermal_gain(struct ca_media_impl *media, uint8_t *gain);
int ca_media_impl_set_thermal_gain(struct ca_media_impl *media, uint8_t gain);
int ca_media_impl_get_thermal_palette(struct ca_media_impl *media, uint8_t *palette);
int ca_media_impl_set_thermal_palette(struct ca_media_impl *media, uint8_t palette);
int ca_media_impl_set_inverted(struct ca_media_impl *media, bool inverted);
void ca_media_impl_close(struct ca_media_impl *media);

/* Read-only feedback; called on a background thread, only while logging. */
int ca_media_impl_exposure(struct ca_media_impl *media, unsigned lens, struct ca_exposure *sample);

int ca_media_impl_apply_overlay(struct ca_media_impl *media, const struct ca_config *settings);

int ca_media_impl_apply_image(struct ca_media_impl *media, const struct ca_config *settings);
#endif
