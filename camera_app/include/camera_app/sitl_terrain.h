#ifndef CAMERA_APP_SITL_TERRAIN_H
#define CAMERA_APP_SITL_TERRAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "camera_app/config.h"
#include "camera_app/exposure.h"

struct ca_sitl_image {
    struct ca_config settings;
    unsigned thermal_gain, thermal_palette;
    float defocus;
    unsigned capture_mask;
    unsigned capture_generation;
    float capture_fov[3];
};

#define CA_SITL_STREAMS 5U
struct ca_sitl_terrain;
int ca_sitl_terrain_open(struct ca_sitl_terrain **out, const char *script,
                         const unsigned width[CA_SITL_STREAMS], const unsigned height[CA_SITL_STREAMS], unsigned fps,
                         const enum ca_video_codec codecs[CA_SITL_STREAMS]);
/* Outputs: visible main, thermal/A8 sub, optional MT11 visible sub,
 * RGB recording, thermal recording.
 * One request in flight: bounded memory and no accumulating video latency. */
int ca_sitl_terrain_frame(struct ca_sitl_terrain *terrain, uint64_t pts, uint64_t presentation_ms,
                          const float hfov[2], bool thermal_main, bool has_thermal, bool separate_recording,
                          const struct ca_sitl_image *image,
                          uint8_t *data[CA_SITL_STREAMS], size_t length[CA_SITL_STREAMS], bool key[CA_SITL_STREAMS],
                          uint8_t *photos[3], size_t photo_length[3], struct ca_exposure *exposure);
void ca_sitl_terrain_interrupt(struct ca_sitl_terrain *terrain);
void ca_sitl_terrain_close(struct ca_sitl_terrain *terrain);

#endif
