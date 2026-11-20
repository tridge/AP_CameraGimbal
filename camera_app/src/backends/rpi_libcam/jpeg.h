#ifndef CAMERA_APP_RPI_JPEG_H
#define CAMERA_APP_RPI_JPEG_H

#include "pipeline.h"

#include <stddef.h>
#include <stdint.h>

/* Convert a camera frame in place from limited-range Rec.709 to the
 * full-range BT.601 YCbCr that JPEG (JFIF) viewers expect. */
void ca_rpi_jpeg_convert(struct ca_rpi_still_frame *frame);

/* Encode a converted frame with libjpeg. *jpeg is malloc()ed. */
int ca_rpi_jpeg_encode(const struct ca_rpi_still_frame *frame, int quality,
                       uint8_t **jpeg, size_t *length);

#endif
