#ifndef CAMERA_APP_CAMERA_DEFINITION_H
#define CAMERA_APP_CAMERA_DEFINITION_H

#include "camera_app/config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define CA_CAMERA_DEFINITION_VERSION 5
#define CA_CAMERA_DEFINITION_PATH "/camera.xml"

enum ca_camera_operation {
    CA_CAMERA_CONFIG, CA_CAMERA_MODE, CA_CAMERA_ZOOM, CA_CAMERA_AUTOFOCUS,
    CA_CAMERA_LENS, CA_CAMERA_SOURCE, CA_CAMERA_PALETTE, CA_CAMERA_GAIN,
};

struct ca_camera_parameter {
    const char *name;
    const char *description;
    enum ca_camera_operation operation;
    /* MAV_PARAM_EXT_TYPE: INT32=6, REAL32=9. */
    unsigned type;
    float initial, minimum, maximum;
    const struct ca_config_option *options;
    size_t option_count;
    int config_index;
};

size_t ca_camera_param_count(void);
int ca_camera_param_find(const char *name);
bool ca_camera_param_info(size_t index, struct ca_camera_parameter *parameter);
bool ca_camera_param_valid(const struct ca_camera_parameter *parameter, float value);
/* Caller owns the returned, NUL-terminated XML. No configuration values or
 * credentials are included: only target capabilities and factory defaults. */
char *ca_camera_definition(size_t *length);
#endif
