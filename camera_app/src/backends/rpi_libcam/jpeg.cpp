/* JPEG stills from Raspberry Pi camera frames, encoded with libjpeg(-turbo)
 * from planar 4:2:0 data without an RGB round trip. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "jpeg.h"
#include "camera_app/log.h"

#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>

/* Limited-range Rec.709 to full-range BT.601, as fixed point (16 bits):
 *   Y' = (Y - 16) * ky + (Cb - 128) * kyb + (Cr - 128) * kyr
 *   Cb' = 128 + (Cb - 128) * kbb + (Cr - 128) * kbr
 *   Cr' = 128 + (Cb - 128) * krb + (Cr - 128) * krr
 * derived from the Kr/Kb of both standards and the 219/224 code ranges. */
struct conversion { int32_t ky, kyb, kyr, kbb, kbr, krb, krr; };

static struct conversion conversion_709_to_601(void)
{
    const double kr709 = 0.2126, kb709 = 0.0722, kr601 = 0.299, kb601 = 0.114;
    double out[3][3]; /* rows Y', Cb', Cr'; columns y, cb, cr (normalised) */
    for (unsigned column = 0; column < 3; column++) {
        const double y = column == 0, cb = column == 1, cr = column == 2;
        const double r = y + 2.0 * (1.0 - kr709) * cr;
        const double b = y + 2.0 * (1.0 - kb709) * cb;
        const double g = (y - kr709 * r - kb709 * b) / (1.0 - kr709 - kb709);
        const double y601 = kr601 * r + (1.0 - kr601 - kb601) * g + kb601 * b;
        out[0][column] = y601;
        out[1][column] = (b - y601) / (2.0 * (1.0 - kb601));
        out[2][column] = (r - y601) / (2.0 * (1.0 - kr601));
    }
    const double one = 65536.0, luma = 255.0 / 219.0, chroma = 255.0 / 224.0;
    return {(int32_t)(out[0][0] * luma * one + 0.5), (int32_t)(out[0][1] * chroma * one),
            (int32_t)(out[0][2] * chroma * one), (int32_t)(out[1][1] * chroma * one),
            (int32_t)(out[1][2] * chroma * one), (int32_t)(out[2][1] * chroma * one),
            (int32_t)(out[2][2] * chroma * one)};
}

static uint8_t clamp8(int32_t fixed)
{
    const int32_t value = (fixed + 32768) >> 16;
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

void ca_rpi_jpeg_convert(struct ca_rpi_still_frame *frame)
{
    static const struct conversion k = conversion_709_to_601();
    const unsigned chroma_stride = frame->stride / 2U, chroma_height = (frame->height + 1U) / 2U;
    uint8_t *luma = frame->data;
    uint8_t *cb = luma + (size_t)frame->stride * frame->height;
    uint8_t *cr = cb + (size_t)chroma_stride * chroma_height;
    for (unsigned row = 0; row < frame->height; row++) {
        uint8_t *y = luma + (size_t)row * frame->stride;
        const uint8_t *u = cb + (size_t)(row / 2U) * chroma_stride;
        const uint8_t *v = cr + (size_t)(row / 2U) * chroma_stride;
        for (unsigned column = 0; column < frame->width; column++) {
            const int32_t b = u[column / 2U] - 128, r = v[column / 2U] - 128;
            y[column] = clamp8((y[column] - 16) * k.ky + b * k.kyb + r * k.kyr);
        }
    }
    for (unsigned row = 0; row < chroma_height; row++) {
        uint8_t *u = cb + (size_t)row * chroma_stride;
        uint8_t *v = cr + (size_t)row * chroma_stride;
        for (unsigned column = 0; column < (frame->width + 1U) / 2U; column++) {
            const int32_t b = u[column] - 128, r = v[column] - 128;
            u[column] = clamp8((128 << 16) + b * k.kbb + r * k.kbr);
            v[column] = clamp8((128 << 16) + b * k.krb + r * k.krr);
        }
    }
}

/* libjpeg's default error handler calls exit(); return to the caller instead. */
struct jpeg_failure {
    struct jpeg_error_mgr manager;
    jmp_buf jump;
};

static void jpeg_failed(j_common_ptr info)
{
    char message[JMSG_LENGTH_MAX];
    (*info->err->format_message)(info, message);
    ca_log("RPi JPEG encode failed: %s", message);
    longjmp(reinterpret_cast<struct jpeg_failure *>(info->err)->jump, 1);
}

int ca_rpi_jpeg_encode(const struct ca_rpi_still_frame *frame, int quality,
                       uint8_t **jpeg, size_t *length)
{
    if (!frame || !frame->data || !jpeg || !length || frame->width < 2U || frame->height < 2U ||
        frame->stride < frame->width) {
        errno = EINVAL;
        return -1;
    }
    const unsigned chroma_stride = frame->stride / 2U, chroma_height = (frame->height + 1U) / 2U;
    uint8_t *planes[3];
    planes[0] = frame->data;
    planes[1] = planes[0] + (size_t)frame->stride * frame->height;
    planes[2] = planes[1] + (size_t)chroma_stride * chroma_height;

    struct jpeg_compress_struct info;
    struct jpeg_failure failure;
    unsigned char *buffer = NULL;
    unsigned long size = 0;
    info.err = jpeg_std_error(&failure.manager);
    failure.manager.error_exit = jpeg_failed;
    if (setjmp(failure.jump)) {
        jpeg_destroy_compress(&info);
        free(buffer);
        errno = EIO;
        return -1;
    }
    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, &buffer, &size);
    info.image_width = frame->width;
    info.image_height = frame->height;
    info.input_components = 3;
    info.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&info);
    jpeg_set_colorspace(&info, JCS_YCbCr);
    jpeg_set_quality(&info, quality, TRUE);
    info.raw_data_in = TRUE;
    info.comp_info[0].h_samp_factor = info.comp_info[0].v_samp_factor = 2;
    info.comp_info[1].h_samp_factor = info.comp_info[1].v_samp_factor = 1;
    info.comp_info[2].h_samp_factor = info.comp_info[2].v_samp_factor = 1;
    jpeg_start_compress(&info, TRUE);
    /* One MCU row: 16 luma and 8 chroma rows. Repeat the last row past the
     * bottom edge, as libjpeg pads partial MCU rows itself. */
    JSAMPROW luma_rows[16], cb_rows[8], cr_rows[8];
    JSAMPARRAY rows[3] = {luma_rows, cb_rows, cr_rows};
    while (info.next_scanline < info.image_height) {
        for (unsigned i = 0; i < 16U; i++) {
            unsigned row = info.next_scanline + i;
            if (row >= frame->height) row = frame->height - 1U;
            luma_rows[i] = planes[0] + (size_t)row * frame->stride;
        }
        for (unsigned i = 0; i < 8U; i++) {
            unsigned row = info.next_scanline / 2U + i;
            if (row >= chroma_height) row = chroma_height - 1U;
            cb_rows[i] = planes[1] + (size_t)row * chroma_stride;
            cr_rows[i] = planes[2] + (size_t)row * chroma_stride;
        }
        jpeg_write_raw_data(&info, rows, 16);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    *jpeg = buffer;
    *length = size;
    return 0;
}
