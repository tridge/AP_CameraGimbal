/* rpi_libcam_caddx stills: Rec.709 limited-range frames become JPEGs whose
 * decoded colours match the scene; needs libjpeg but not libcamera. */
#include "../src/backends/rpi_libcam/jpeg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

struct ycbcr { uint8_t y, cb, cr; };

/* Limited-range Rec.709 encoding of linear-free (gamma-encoded) RGB in 0..1. */
static struct ycbcr rec709(double r, double g, double b)
{
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return {(uint8_t)lround(16 + 219 * y), (uint8_t)lround(128 + 224 * (b - y) / 1.8556),
            (uint8_t)lround(128 + 224 * (r - y) / 1.5748)};
}

/* Left half colour a, right half colour b; height deliberately not a multiple of 16. */
static void fill(struct ca_rpi_still_frame *f, struct ycbcr a, struct ycbcr b)
{
    const unsigned cs = f->stride / 2, ch = (f->height + 1) / 2;
    uint8_t *y = f->data, *u = y + f->stride * f->height, *v = u + cs * ch;
    for (unsigned r = 0; r < f->height; r++)
        for (unsigned c = 0; c < f->width; c++) y[r * f->stride + c] = c < f->width / 2 ? a.y : b.y;
    for (unsigned r = 0; r < ch; r++)
        for (unsigned c = 0; c < f->width / 2; c++) {
            u[r * cs + c] = c < f->width / 4 ? a.cb : b.cb;
            v[r * cs + c] = c < f->width / 4 ? a.cr : b.cr;
        }
}

static void decode_pixel(const uint8_t *jpeg, size_t length, unsigned width, unsigned height,
                         unsigned x, unsigned y, int rgb[3])
{
    struct jpeg_decompress_struct d;
    struct jpeg_error_mgr e;
    d.err = jpeg_std_error(&e);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, jpeg, length);
    assert(jpeg_read_header(&d, TRUE) == JPEG_HEADER_OK);
    d.out_color_space = JCS_RGB;
    jpeg_start_decompress(&d);
    assert(d.output_width == width && d.output_height == height && d.output_components == 3);
    unsigned char *row = (unsigned char *)malloc(width * 3);
    while (d.output_scanline < d.output_height) {
        const unsigned line = d.output_scanline;
        jpeg_read_scanlines(&d, &row, 1);
        if (line == y) for (unsigned i = 0; i < 3; i++) rgb[i] = row[x * 3 + i];
    }
    free(row);
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
}

static void check(const int rgb[3], int r, int g, int b)
{
    const int tolerance = 8;
    if (abs(rgb[0] - r) > tolerance || abs(rgb[1] - g) > tolerance || abs(rgb[2] - b) > tolerance) {
        fprintf(stderr, "got %d,%d,%d expected %d,%d,%d\n", rgb[0], rgb[1], rgb[2], r, g, b);
        assert(false);
    }
}

int main(void)
{
    struct ca_rpi_still_frame frame = {};
    frame.width = 64; frame.height = 40; frame.stride = 64;
    frame.data = (uint8_t *)calloc(1, frame.stride * frame.height * 3 / 2 + 64);

    const struct { double r, g, b; int er, eg, eb; } pairs[][2] = {
        {{1, 0, 0, 255, 0, 0}, {0.5, 0.5, 0.5, 128, 128, 128}},
        {{1, 1, 1, 255, 255, 255}, {0, 0, 1, 0, 0, 255}},
        {{0, 1, 0, 0, 255, 0}, {0, 0, 0, 0, 0, 0}},
    };
    for (const auto &pair : pairs) {
        fill(&frame, rec709(pair[0].r, pair[0].g, pair[0].b), rec709(pair[1].r, pair[1].g, pair[1].b));
        ca_rpi_jpeg_convert(&frame);
        uint8_t *jpeg = NULL;
        size_t length = 0;
        assert(ca_rpi_jpeg_encode(&frame, 90, &jpeg, &length) == 0);
        assert(length > 4 && jpeg[0] == 0xff && jpeg[1] == 0xd8 && jpeg[length - 2] == 0xff && jpeg[length - 1] == 0xd9);
        int rgb[3];
        decode_pixel(jpeg, length, frame.width, frame.height, 8, 36, rgb); /* last, partial MCU row */
        check(rgb, pair[0].er, pair[0].eg, pair[0].eb);
        decode_pixel(jpeg, length, frame.width, frame.height, 56, 4, rgb);
        check(rgb, pair[1].er, pair[1].eg, pair[1].eb);
        free(jpeg);
    }
    struct ca_rpi_still_frame bad = frame;
    bad.stride = 32;
    uint8_t *jpeg = NULL;
    size_t length = 0;
    assert(ca_rpi_jpeg_encode(&bad, 90, &jpeg, &length) < 0);
    free(frame.data);
    puts("rpi_libcam_caddx JPEG colour conversion and encoding passed");
    return 0;
}
