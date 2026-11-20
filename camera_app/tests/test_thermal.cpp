#include "../src/backends/mt11/thermal.h"
#include "camera_app/config.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct callback_state {
    const uint8_t *expected;
    struct timespec expected_timestamp;
    unsigned count;
};

static void frame_callback(const uint8_t *display_yuyv,
                           const uint16_t *radiometric_y16,
                           const struct timespec *captured_at, void *opaque)
{
    struct callback_state *state = (callback_state*)(opaque);
    assert(memcmp(display_yuyv, state->expected,
                  CA_MT11_THERMAL_WIDTH * CA_MT11_THERMAL_HEIGHT * 2U) == 0);
    assert(memcmp(radiometric_y16,
                  state->expected + CA_MT11_THERMAL_WIDTH *
                                        CA_MT11_THERMAL_HEIGHT * 2U,
                  CA_MT11_THERMAL_WIDTH * CA_MT11_THERMAL_HEIGHT * 2U) == 0);
    assert(captured_at->tv_sec == state->expected_timestamp.tv_sec);
    assert(captured_at->tv_nsec == state->expected_timestamp.tv_nsec);
    state->count++;
}

static void test_conversion(void)
{
    const uint8_t yuyv[] = {
        10, 20, 11, 30, 12, 21, 13, 31,
        14, 22, 15, 32, 16, 23, 17, 33,
    };
    uint8_t y[12];
    uint8_t uv[6];
    const uint8_t expected_y[] = {10, 11, 12, 13, 0, 0,
                                  14, 15, 16, 17, 0, 0};
    const uint8_t expected_uv[] = {20, 30, 21, 31, 0, 0};
    memset(y, 0, sizeof(y));
    memset(uv, 0, sizeof(uv));
    ca_mt11_yuyv_to_nv12(yuyv, 4, 2, y, 6, uv, 6);
    assert(memcmp(y, expected_y, sizeof(y)) == 0);
    assert(memcmp(uv, expected_uv, sizeof(uv)) == 0);
    memset(y, 0, sizeof(y));
    memset(uv, 0, sizeof(uv));
    ca_mt11_yuyv_to_nv12_rotated_180(yuyv, 4, 2, y, 6, uv, 6);
    const uint8_t rotated_y[] = {17, 16, 15, 14, 0, 0,
                                 13, 12, 11, 10, 0, 0};
    const uint8_t rotated_uv[] = {21, 31, 20, 30, 0, 0};
    assert(memcmp(y, rotated_y, sizeof(y)) == 0);
    assert(memcmp(uv, rotated_uv, sizeof(uv)) == 0);
}

static void test_assembler(void)
{
    struct ca_mt11_uvc_assembler assembler;
    struct callback_state state = {};
    const struct timespec received_at = {
        .tv_sec = 1700000000,
        .tv_nsec = 123456789,
    };
    uint8_t *frame = (uint8_t*)(malloc(CA_MT11_THERMAL_FRAME_BYTES));
    uint8_t *payload = (uint8_t*)(malloc(100012U));
    size_t offset = 0;

    assert(frame != NULL && payload != NULL);
    for (size_t i = 0; i < CA_MT11_THERMAL_FRAME_BYTES; i++) {
        frame[i] = (uint8_t)(i * 17U + 3U);
    }
    state.expected = frame;
    state.expected_timestamp = received_at;
    assert(ca_mt11_uvc_assembler_init(&assembler, frame_callback, &state) == 0);
    while (offset < CA_MT11_THERMAL_FRAME_BYTES) {
        size_t bytes = CA_MT11_THERMAL_FRAME_BYTES - offset;
        if (bytes > 100000U) bytes = 100000U;
        payload[0] = 12;
        payload[1] = bytes == CA_MT11_THERMAL_FRAME_BYTES - offset
                         ? 0x82U
                         : 0x80U;
        memset(payload + 2, 0, 10);
        memcpy(payload + 12, frame + offset, bytes);
        assert(ca_mt11_uvc_assembler_consume(&assembler, payload,
                                              bytes + 12U,
                                              &received_at) == 0);
        offset += bytes;
    }
    assert(state.count == 1U);
    ca_mt11_uvc_assembler_destroy(&assembler);
    free(payload);
    free(frame);
}

static void test_temperature_range(void)
{
    uint16_t pixels[12];
    struct ca_thermal_range range = {};

    for (size_t i = 0; i < 12U; i++) pixels[i] = 19000U;
    pixels[6] = 20000U;
    pixels[8] = 18000U;
    assert(ca_mt11_thermal_range_from_y16(pixels, 4, 3, &range));
    assert(range.maximum_centi_c == 3935U);
    assert(range.minimum_centi_c == 810U);
    assert(range.maximum_x == 2U && range.maximum_y == 1U);
    assert(range.minimum_x == 0U && range.minimum_y == 2U);
    /* The sensor uses 1/64 Kelvin: retain negative and >655.35C values. */
    pixels[0] = 14922; /* -40C */
    pixels[11] = 62282; /* 700C */
    assert(ca_mt11_thermal_range_from_y16(pixels, 4, 3, &range));
    assert(range.minimum_centi_c == -3999 && range.maximum_centi_c == 70001);
    assert(ca_thermal_legacy_centi_c(range.minimum_centi_c) == 0);
    assert(ca_thermal_legacy_centi_c(range.maximum_centi_c) == UINT16_MAX);
    assert(ca_thermal_legacy_centi_c(3935) == 3935);
    pixels[0] = 0; pixels[11] = UINT16_MAX;
    assert(ca_mt11_thermal_range_from_y16(pixels, 4, 3, &range));
    assert(range.minimum_centi_c == -27315 && range.maximum_centi_c == 75083);
    /* Compare hotspot positions with the real thermal image converters. */
    uint8_t yuyv[4*2*2] = {}, y[8], uv[4];
    yuyv[0] = 255;
    for (unsigned rotated=0; rotated<2; rotated++) {
        if (rotated) ca_mt11_yuyv_to_nv12_rotated_180(yuyv,4,2,y,4,uv,4);
        else ca_mt11_yuyv_to_nv12(yuyv,4,2,y,4,uv,4);
        unsigned x=ca_thermal_display_pixel(0,4,rotated);
        unsigned row=ca_thermal_display_pixel(0,2,rotated);
        assert(y[row*4+x] == 255);
        assert(ca_thermal_display_pixel(4,4,rotated) == 4); /* invalid stays invalid */
    }
    range.sampled_us=1000000;
    assert(ca_thermal_range_fresh(&range,1000000));
    assert(ca_thermal_range_fresh(&range,1250000));
    assert(!ca_thermal_range_fresh(&range,1250001));
    assert(!ca_thermal_range_fresh(&range,999999));
    range.sampled_us=0;
    assert(!ca_thermal_range_fresh(&range,1));
    assert(!ca_mt11_thermal_range_from_y16(NULL, 4, 3, &range));
}

static void test_gain_commands(void)
{
    static const uint8_t expected_get[] = {
        0x14, 0x85, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
    };
    static const uint8_t expected_set_low[] = {
        0x14, 0xc5, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t command[16];

    ca_mt11_thermal_gain_get_command(command);
    assert(memcmp(command, expected_get, sizeof(command)) == 0);
    ca_mt11_thermal_gain_set_command(command, 0U);
    assert(memcmp(command, expected_set_low, sizeof(command)) == 0);
    ca_mt11_thermal_gain_set_command(command, 1U);
    assert(command[7] == 1U);
    command[7] = 0U;
    assert(memcmp(command, expected_set_low, sizeof(command)) == 0);
}

static void test_palette_commands(void)
{
    static const uint8_t expected_get[16] = {
        0x09, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    };
    uint8_t command[16];
    ca_mt11_thermal_palette_get_command(command);
    assert(memcmp(command, expected_get, sizeof(command)) == 0);
    ca_mt11_thermal_palette_set_command(command, CA_PALETTE_WHITE_HOT);
    assert(command[0] == 0x09 && command[1] == 0xc4 && command[7] == 1U &&
           command[8] == 1U);
    ca_mt11_thermal_palette_set_command(command, CA_PALETTE_IRONBOW);
    assert(command[7] == 1U && command[8] == 4U);
}

int main(void)
{
    test_conversion();
    test_assembler();
    test_temperature_range();
    test_gain_commands();
    test_palette_commands();
    puts("thermal tests passed");
    return 0;
}
