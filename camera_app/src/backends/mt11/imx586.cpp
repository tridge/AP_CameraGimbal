#include "apcam/compiler.h"
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "imx586.h"
#include <array>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ot_common_ae.h"
#include "ot_common_awb.h"
#include "ot_common_sns.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"
#include "ss_mpi_isp.h"

#ifndef OT_ISP_MAX_PIPE_NUM
#define OT_ISP_MAX_PIPE_NUM 16
#endif

#define IMX586_I2C_SLAVE_FORCE 0x0706
#define IMX586_REG_COUNT 8
#define IMX586_ADDR_BYTES 2
#define IMX586_DATA_BYTES 1
#define IMX586_ERR_NULL OT_ERR_ISP_NULL_PTR

enum imx586_reg_index {
    IMX586_EXPOSURE_H,
    IMX586_EXPOSURE_L,
    IMX586_AGAIN_H,
    IMX586_AGAIN_L,
    IMX586_DGAIN_H,
    IMX586_DGAIN_L,
    IMX586_VMAX_H,
    IMX586_VMAX_L,
};

struct mt11_imx586_reg {
    td_u16 address;
    td_u8 value;
};

#include "imx586_mode0_registers.inc"

static const td_u32 imx586_dynamic_registers[IMX586_REG_COUNT] = {
    0x0202, 0x0203, 0x0204, 0x0205, 0x020e, 0x020f, 0x0340, 0x0341,
};

/* The values of the same registers in the exact mode-0 startup table. */
static const td_u8 imx586_dynamic_defaults[IMX586_REG_COUNT] = {
    0x0b, 0xc4, 0x00, 0x00, 0x01, 0x00, 0x0b, 0xf8,
};

static ot_isp_sns_state *imx586_state[OT_ISP_MAX_PIPE_NUM];
static auto imx586_bus = [] {
    std::array<ot_isp_sns_commbus, OT_ISP_MAX_PIPE_NUM> bus {};
    for (unsigned i=1; i<bus.size(); i++) bus[i].i2c_dev = -1;
    return bus;
}();
static auto imx586_i2c_fd = [] {
    std::array<int, OT_ISP_MAX_PIPE_NUM> fds {};
    for (auto &fd : fds) fd = -1;
    return fds;
}();
static td_u32 imx586_init_exposure[OT_ISP_MAX_PIPE_NUM];
static td_u32 imx586_lines_per_500ms[OT_ISP_MAX_PIPE_NUM];
static td_u16 imx586_init_wb[OT_ISP_MAX_PIPE_NUM][OT_ISP_BAYER_CHN_NUM];
static td_u16 imx586_sample_r_gain[OT_ISP_MAX_PIPE_NUM];
static td_u16 imx586_sample_b_gain[OT_ISP_MAX_PIPE_NUM];

/* IMX586 mode-0 noise calibration. */
static const td_u8 imx586_noise_calibration[128] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb9, 0x40, 0x0d, 0x9b, 0x54, 0x76, 0xf2, 0xd0, 0x24, 0x3f,
    0x62, 0x7d, 0x5b, 0x26, 0x3e, 0x94, 0x43, 0x3f, 0xeb, 0xa3, 0xcc, 0xf8, 0x49, 0x3d, 0x6c, 0x3e,
    0xa1, 0xfd, 0x66, 0x67, 0x73, 0xca, 0xd3, 0xbe, 0x23, 0x43, 0x02, 0x96, 0xb9, 0x11, 0x94, 0x3f,
    0x0d, 0x9b, 0x54, 0x76, 0xf2, 0xd0, 0x24, 0x3f, 0x62, 0x7d, 0x5b, 0x26, 0x3e, 0x94, 0x43, 0x3f,
    0xeb, 0xa3, 0xcc, 0xf8, 0x49, 0x3d, 0x6c, 0x3e, 0xa1, 0xfd, 0x66, 0x67, 0x73, 0xca, 0xd3, 0xbe,
    0x23, 0x43, 0x02, 0x96, 0xb9, 0x11, 0x94, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* IMX586 linear black-level calibration. */
static const td_u8 imx586_linear_black_level[216] = {
    0x00, 0x00, 0x00, 0x00, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04,
    0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04, 0xb0, 0x04,
    0xb0, 0x04, 0xb0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04,
    0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04,
    0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x0f,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x03, 0x2b, 0x03, 0x32, 0x03, 0x44, 0x03,
    0x68, 0x03, 0xb0, 0x03, 0x38, 0x04, 0x38, 0x04, 0x38, 0x04, 0x38, 0x04, 0x38, 0x04, 0x38, 0x04,
    0x38, 0x04, 0x38, 0x04, 0x38, 0x04, 0x38, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04,
    0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x04,
    0x00, 0x0f, 0x00, 0x00, 0x84, 0x08, 0x00, 0x00,
};

/* IMX586 mode-0 AWB and DNG calibration blocks. */
static const td_u8 imx586_awb_agc[20] = {
    0x01, 0x00, 0x00, 0x00, 0x80, 0x80, 0x80, 0x80, 0x7c, 0x78, 0x74, 0x70, 0x6c, 0x68, 0x64, 0x5e,
    0x5a, 0x5a, 0x5a, 0x5a,
};

static const td_u8 imx586_awb_ccm[142] = {
    0x04, 0x00, 0x14, 0x19, 0x41, 0x02, 0x28, 0x81, 0x19, 0x80, 0x68, 0x80, 0xa9, 0x01, 0x41, 0x80,
    0x05, 0x00, 0x27, 0x81, 0x22, 0x02, 0xf2, 0x12, 0x35, 0x02, 0x13, 0x81, 0x22, 0x80, 0x76, 0x80,
    0x9d, 0x01, 0x27, 0x80, 0x0b, 0x00, 0x33, 0x81, 0x28, 0x02, 0x2e, 0x0e, 0x25, 0x02, 0x0d, 0x81,
    0x18, 0x80, 0x7a, 0x80, 0x89, 0x01, 0x0f, 0x80, 0x22, 0x00, 0x33, 0x81, 0x11, 0x02, 0xdd, 0x09,
    0xc1, 0x01, 0x6e, 0x80, 0x53, 0x80, 0x73, 0x80, 0x8e, 0x01, 0x1b, 0x80, 0x0f, 0x00, 0xfc, 0x81,
    0xed, 0x02, 0x34, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x40, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x78, 0x05, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

static const td_u8 imx586_dng_color[12] = {
    0x7a, 0x01, 0x00, 0x01, 0xae, 0x01, 0xb7, 0x01, 0x00, 0x01, 0xb7, 0x01,
};
static td_bool imx586_valid_pipe(ot_vi_pipe vi_pipe)
{
    return vi_pipe >= 0 && vi_pipe < OT_ISP_MAX_PIPE_NUM ? TD_TRUE : TD_FALSE;
}

static ot_isp_sns_state *imx586_get_state(ot_vi_pipe vi_pipe)
{
    return imx586_valid_pipe(vi_pipe) ? imx586_state[vi_pipe] : TD_NULL;
}

static td_s32 imx586_i2c_init(ot_vi_pipe vi_pipe)
{
    char path[32];
    int fd;

    if (!imx586_valid_pipe(vi_pipe)) {
        return TD_FAILURE;
    }
    if (imx586_i2c_fd[vi_pipe] >= 0) {
        return TD_SUCCESS;
    }
    if (imx586_bus[vi_pipe].i2c_dev < 0) {
        return TD_FAILURE;
    }
    (void)snprintf(path, sizeof(path), "/dev/i2c-%u", (unsigned)(td_u8)imx586_bus[vi_pipe].i2c_dev);
    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return TD_FAILURE;
    }
    if (ioctl(fd, IMX586_I2C_SLAVE_FORCE, MT11_IMX586_I2C_ADDR) < 0) {
        (void)close(fd);
        return TD_FAILURE;
    }
    imx586_i2c_fd[vi_pipe] = fd;
    return TD_SUCCESS;
}

static td_s32 imx586_i2c_exit(ot_vi_pipe vi_pipe)
{
    if (!imx586_valid_pipe(vi_pipe) || imx586_i2c_fd[vi_pipe] < 0) {
        return TD_FAILURE;
    }
    (void)close(imx586_i2c_fd[vi_pipe]);
    imx586_i2c_fd[vi_pipe] = -1;
    return TD_SUCCESS;
}

td_s32 mt11_imx586_write_register(ot_vi_pipe vi_pipe, td_u32 addr, td_u32 data)
{
    td_u8 payload[3];

    if (!imx586_valid_pipe(vi_pipe) || imx586_i2c_fd[vi_pipe] < 0) {
        return TD_FAILURE;
    }
    payload[0] = (td_u8)(addr >> 8);
    payload[1] = (td_u8)addr;
    payload[2] = (td_u8)data;
    return write(imx586_i2c_fd[vi_pipe], payload, sizeof(payload)) == (ssize_t)sizeof(payload)
        ? TD_SUCCESS : TD_FAILURE;
}

td_s32 mt11_imx586_read_register(ot_vi_pipe vi_pipe, td_u32 addr)
{
    (void)vi_pipe;
    (void)addr;
    /* This fixed-mode adapter does not require register reads. */
    return 0;
}

td_void mt11_imx586_standby(ot_vi_pipe vi_pipe)
{
    (void)mt11_imx586_write_register(vi_pipe, 0x0100, 0x00);
}

td_void mt11_imx586_restart(ot_vi_pipe vi_pipe)
{
    (void)mt11_imx586_write_register(vi_pipe, 0x0100, 0x01);
}

static td_void imx586_sensor_init(ot_vi_pipe vi_pipe)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    size_t i;

    if (state == TD_NULL || imx586_i2c_init(vi_pipe) != TD_SUCCESS) {
        return;
    }
    for (i = 0; i < sizeof(mt11_imx586_mode0_registers) / sizeof(mt11_imx586_mode0_registers[0]); ++i) {
        (void)mt11_imx586_write_register(vi_pipe, mt11_imx586_mode0_registers[i].address,
            mt11_imx586_mode0_registers[i].value);
    }
    if (state->regs_info[0].reg_num != 0) {
        for (i = 0; i < state->regs_info[0].reg_num; ++i) {
            const ot_isp_i2c_data *reg = &state->regs_info[0].i2c_data[i];
            (void)mt11_imx586_write_register(vi_pipe, reg->reg_addr, reg->data);
        }
    }
    (void)usleep(10000);
    mt11_imx586_restart(vi_pipe);
    state->init = TD_TRUE;
}

static td_void imx586_sensor_exit(ot_vi_pipe vi_pipe)
{
    (void)imx586_i2c_exit(vi_pipe);
}

static td_void imx586_sensor_global_init(ot_vi_pipe vi_pipe)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL) {
        return;
    }
    state->init = TD_FALSE;
    state->sync_init = TD_FALSE;
    state->img_mode = 0;
    state->wdr_mode = OT_WDR_MODE_NONE;
    state->fl_std = MT11_IMX586_VMAX;
    state->fl[0] = MT11_IMX586_VMAX;
    state->fl[1] = MT11_IMX586_VMAX;
    (void)memset(&state->regs_info, 0, sizeof(state->regs_info));
}

static td_s32 imx586_set_image_mode(ot_vi_pipe vi_pipe, const ot_isp_cmos_sensor_image_mode *mode)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL || mode == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    if (state->wdr_mode != OT_WDR_MODE_NONE || mode->width > MT11_IMX586_WIDTH ||
        mode->height > MT11_IMX586_HEIGHT || mode->fps > MT11_IMX586_MAX_FPS ||
        mode->fps < MT11_IMX586_MIN_FPS || mode->sns_mode != 0) {
        return TD_FAILURE;
    }
    if (state->init && state->img_mode == 0) {
        return OT_ISP_DO_NOT_NEED_SWITCH_IMAGEMODE;
    }
    state->sync_init = TD_FALSE;
    state->img_mode = 0;
    state->fl_std = MT11_IMX586_VMAX;
    state->fl[0] = MT11_IMX586_VMAX;
    state->fl[1] = MT11_IMX586_VMAX;
    return TD_SUCCESS;
}

static td_s32 imx586_set_wdr_mode(ot_vi_pipe vi_pipe, td_u8 mode)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    if (mode != OT_WDR_MODE_NONE) {
        return TD_FAILURE;
    }
    state->sync_init = TD_FALSE;
    state->wdr_mode = OT_WDR_MODE_NONE;
    (void)memset(state->wdr_int_time, 0, sizeof(state->wdr_int_time));
    return TD_SUCCESS;
}

static td_s32 imx586_get_isp_default(ot_vi_pipe vi_pipe, ot_isp_cmos_default *def)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL || def == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    (void)memset(def, 0, sizeof(*def));
    memcpy(&def->noise_calibration, imx586_noise_calibration, sizeof(imx586_noise_calibration));
    def->sensor_max_resolution.max_width = MT11_IMX586_WIDTH;
    def->sensor_max_resolution.max_height = MT11_IMX586_HEIGHT;
    def->sensor_mode.sensor_id = MT11_IMX586_SENSOR_ID;
    def->sensor_mode.sensor_mode = state->img_mode;
    def->sensor_mode.valid_dng_raw_format = TD_TRUE;
    def->sensor_mode.dng_raw_format.bits_per_sample = 12;
    def->sensor_mode.dng_raw_format.cfa_plane_color[0] = 0;
    def->sensor_mode.dng_raw_format.cfa_plane_color[1] = 1;
    def->sensor_mode.dng_raw_format.cfa_plane_color[2] = 2;
    def->sensor_mode.dng_raw_format.cfa_layout = OT_ISP_CFALAYOUT_TYPE_RECTANGULAR;
    def->sensor_mode.dng_raw_format.black_level_repeat_dim.repeat_row = 2;
    def->sensor_mode.dng_raw_format.black_level_repeat_dim.repeat_col = 2;
    def->sensor_mode.dng_raw_format.white_level = 0x0fff;
    def->sensor_mode.dng_raw_format.default_scale.default_scale_hor.numerator = 1;
    def->sensor_mode.dng_raw_format.default_scale.default_scale_hor.denominator = 1;
    def->sensor_mode.dng_raw_format.default_scale.default_scale_ver.numerator = 1;
    def->sensor_mode.dng_raw_format.default_scale.default_scale_ver.denominator = 1;
    def->sensor_mode.dng_raw_format.cfa_repeat_pattern_dim.repeat_pattern_dim_row = 2;
    def->sensor_mode.dng_raw_format.cfa_repeat_pattern_dim.repeat_pattern_dim_col = 2;
    def->sensor_mode.dng_raw_format.cfa_pattern[0] = 0;
    def->sensor_mode.dng_raw_format.cfa_pattern[1] = 1;
    def->sensor_mode.dng_raw_format.cfa_pattern[2] = 1;
    def->sensor_mode.dng_raw_format.cfa_pattern[3] = 2;
    APC_STATIC_ASSERT(sizeof(def->dng_color_param) == sizeof(imx586_dng_color), "B090 DNG color ABI drift");
    memcpy(&def->dng_color_param, imx586_dng_color, sizeof(imx586_dng_color));
    /* Optional opaque ISP algorithm tables deliberately remain disabled. */
    return TD_SUCCESS;
}

static td_s32 imx586_get_black_level(ot_vi_pipe vi_pipe, ot_isp_cmos_black_level *black_level)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL || black_level == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    APC_STATIC_ASSERT(sizeof(*black_level) == sizeof(imx586_linear_black_level), "B090 black-level ABI drift");
    memcpy(black_level, imx586_linear_black_level, sizeof(*black_level));
    return TD_SUCCESS;
}

static td_s32 imx586_get_sns_regs_info(ot_vi_pipe vi_pipe, ot_isp_sns_regs_info *out)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    ot_isp_sns_regs_info *current;
    td_u32 i;

    if (state == TD_NULL || out == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    current = &state->regs_info[0];
    if (!state->sync_init || !out->config) {
        (void)memset(current, 0, sizeof(*current));
        current->sns_type = OT_ISP_SNS_I2C_TYPE;
        current->reg_num = IMX586_REG_COUNT;
        current->cfg2_valid_delay_max = 2;
        current->com_bus = imx586_bus[vi_pipe];
        for (i = 0; i < IMX586_REG_COUNT; ++i) {
            current->i2c_data[i].update = TD_TRUE;
            current->i2c_data[i].dev_addr = MT11_IMX586_I2C_ADDR << 1;
            current->i2c_data[i].reg_addr = imx586_dynamic_registers[i];
            current->i2c_data[i].addr_byte_num = IMX586_ADDR_BYTES;
            current->i2c_data[i].data_byte_num = IMX586_DATA_BYTES;
            current->i2c_data[i].data = imx586_dynamic_defaults[i];
        }
        state->sync_init = TD_TRUE;
    } else {
        for (i = 0; i < current->reg_num; ++i) {
            current->i2c_data[i].update = current->i2c_data[i].data !=
                state->regs_info[1].i2c_data[i].data ? TD_TRUE : TD_FALSE;
        }
    }
    out->config = TD_FALSE;
    memcpy(out, current, sizeof(*out));
    memcpy(&state->regs_info[1], current, sizeof(*current));
    state->fl[1] = state->fl[0];
    return TD_SUCCESS;
}

static td_void imx586_set_pixel_detect(ot_vi_pipe vi_pipe, td_bool enable)
{
    (void)vi_pipe;
    (void)enable;
}

static td_void imx586_again_calc(ot_vi_pipe vi_pipe, td_u32 *again_lin, td_u32 *again_db)
{
    td_u32 code;
    (void)vi_pipe;
    if (again_lin == TD_NULL || again_db == TD_NULL || *again_lin == 0) {
        return;
    }
    code = 1024U - 1048576U / *again_lin;
    if (code < 112U) {
        code = 112U;
    } else if (code > 1008U) {
        code = 1008U;
    }
    *again_db = code;
    *again_lin = 1048576U / (1024U - code);
}

static td_void imx586_gains_update(ot_vi_pipe vi_pipe, td_u32 again, td_u32 dgain)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL) {
        return;
    }
    state->regs_info[0].i2c_data[IMX586_AGAIN_H].data = (again >> 8) & 0x3;
    state->regs_info[0].i2c_data[IMX586_AGAIN_L].data = again & 0xff;
    state->regs_info[0].i2c_data[IMX586_DGAIN_H].data = (dgain >> 8) & 0xf;
    state->regs_info[0].i2c_data[IMX586_DGAIN_L].data = dgain & 0xff;
}

static td_void imx586_inttime_update(ot_vi_pipe vi_pipe, td_u32 int_time)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL) {
        return;
    }
    if (int_time < 16U) {
        int_time = 16U;
    } else if (int_time > 65535U) {
        int_time = 65535U;
    }
    state->regs_info[0].i2c_data[IMX586_EXPOSURE_H].data = int_time >> 8;
    state->regs_info[0].i2c_data[IMX586_EXPOSURE_L].data = int_time & 0xff;
}

static td_void imx586_slow_framerate_set(ot_vi_pipe vi_pipe, td_u32 full_lines,
    ot_isp_ae_sensor_default *ae_default)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL || ae_default == TD_NULL) {
        return;
    }
    if (full_lines > 65535U) {
        full_lines = 65535U;
    }
    state->fl[0] = full_lines;
    state->regs_info[0].i2c_data[IMX586_VMAX_H].data = full_lines >> 8;
    state->regs_info[0].i2c_data[IMX586_VMAX_L].data = full_lines & 0xff;
    ae_default->full_lines = full_lines;
    ae_default->max_int_time = full_lines - 2U;
}

static td_void imx586_fps_set(ot_vi_pipe vi_pipe, td_float fps, ot_isp_ae_sensor_default *ae_default)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    td_u32 vmax;
    if (state == TD_NULL || ae_default == TD_NULL || fps < MT11_IMX586_MIN_FPS ||
        fps > MT11_IMX586_MAX_FPS) {
        return;
    }
    vmax = (td_u32)(MT11_IMX586_VMAX * MT11_IMX586_MAX_FPS / fps);
    if (vmax > 65535U) {
        vmax = 65535U;
    }
    state->fl_std = vmax;
    state->fl[0] = vmax;
    state->regs_info[0].i2c_data[IMX586_VMAX_H].data = vmax >> 8;
    state->regs_info[0].i2c_data[IMX586_VMAX_L].data = vmax & 0xff;
    ae_default->fps = fps;
    ae_default->lines_per500ms = (td_u32)(vmax * fps * 0.5f);
    ae_default->full_lines_std = vmax;
    ae_default->full_lines = vmax;
    ae_default->max_int_time = vmax - 48U;
    ae_default->hmax_times = (td_u32)(1000000000.0f / (vmax * fps));
}

static td_s32 imx586_get_ae_default(ot_vi_pipe vi_pipe, ot_isp_ae_sensor_default *ae_default)
{
    static const td_u32 route_int_time[] = {174, 32811, 32811, 32811, 32811};
    static const td_u32 route_again[] = {1151, 1151, 65535, 65535, 65535};
    static const td_u32 route_dgain[] = {1024, 1024, 1024, 16380, 16380};
    static const td_u32 route_isp_dgain[] = {1024, 1024, 1024, 1024, 261120};
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    td_u32 i;
    if (state == TD_NULL || ae_default == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    (void)memset(ae_default, 0, sizeof(*ae_default));
    ae_default->ae_compensation = 56;
    ae_default->lines_per500ms = imx586_lines_per_500ms[vi_pipe] != 0 ?
        imx586_lines_per_500ms[vi_pipe] : MT11_IMX586_VMAX * 15U;
    ae_default->flicker_freq = 50U * 256U;
    ae_default->fps = MT11_IMX586_MAX_FPS;
    ae_default->hmax_times = (td_u32)(1000000000.0f /
        (MT11_IMX586_VMAX * MT11_IMX586_MAX_FPS));
    ae_default->init_exposure = imx586_init_exposure[vi_pipe] != 0 ?
        imx586_init_exposure[vi_pipe] : 0x12977U;
    ae_default->full_lines_std = state->fl_std;
    ae_default->full_lines_max = 65535U;
    ae_default->full_lines = state->fl_std;
    ae_default->max_int_time = state->fl_std - 48U;
    ae_default->min_int_time = 16U;
    ae_default->max_int_time_target = 65535U;
    ae_default->min_int_time_target = 16U;
    ae_default->int_time_accu.accu_type = OT_ISP_AE_ACCURACY_LINEAR;
    ae_default->int_time_accu.accuracy = 2.0f;
    ae_default->max_again = 65535U;
    ae_default->min_again = 1151U;
    ae_default->max_again_target = 65535U;
    ae_default->min_again_target = 1151U;
    ae_default->again_accu.accu_type = OT_ISP_AE_ACCURACY_TABLE;
    ae_default->again_accu.accuracy = 1.0f / 256.0f;
    ae_default->max_dgain = 0x0fffU;
    ae_default->min_dgain = 0x0100U;
    ae_default->max_dgain_target = 0x0fffU;
    ae_default->min_dgain_target = 0x0100U;
    ae_default->dgain_accu.accu_type = OT_ISP_AE_ACCURACY_LINEAR;
    ae_default->dgain_accu.accuracy = 1.0f / 256.0f;
    ae_default->max_isp_dgain_target = 0xff00U;
    ae_default->min_isp_dgain_target = 0x0100U;
    ae_default->isp_dgain_shift = 8;
    ae_default->ae_run_interval = 8;
    ae_default->ae_route_ex_valid = TD_TRUE;
    ae_default->ae_route_attr_ex.total_num = 5;
    for (i = 0; i < ae_default->ae_route_attr_ex.total_num; i++) {
        ae_default->ae_route_attr_ex.route_ex_node[i].int_time = route_int_time[i];
        ae_default->ae_route_attr_ex.route_ex_node[i].a_gain = route_again[i];
        ae_default->ae_route_attr_ex.route_ex_node[i].d_gain = route_dgain[i];
        ae_default->ae_route_attr_ex.route_ex_node[i].isp_d_gain = route_isp_dgain[i];
    }
    return TD_SUCCESS;
}

static td_void imx586_ae_fswdr_attr_set(ot_vi_pipe vi_pipe, ot_isp_ae_fswdr_attr *attr)
{
    (void)vi_pipe;
    (void)attr;
}

static td_s32 imx586_get_awb_default(ot_vi_pipe vi_pipe, ot_isp_awb_sensor_default *awb_default)
{
    ot_isp_sns_state *state = imx586_get_state(vi_pipe);
    if (state == TD_NULL || awb_default == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    (void)memset(awb_default, 0, sizeof(*awb_default));
    awb_default->wb_ref_temp = 4950;
    awb_default->gain_offset[0] = 450;
    awb_default->gain_offset[1] = 256;
    awb_default->gain_offset[2] = 256;
    awb_default->gain_offset[3] = 447;
    awb_default->wb_para[0] = -53;
    awb_default->wb_para[1] = 309;
    awb_default->wb_para[2] = 0;
    awb_default->wb_para[3] = 160279;
    awb_default->wb_para[4] = 128;
    awb_default->wb_para[5] = -110119;
    awb_default->sample_rgain = imx586_sample_r_gain[vi_pipe];
    awb_default->sample_bgain = imx586_sample_b_gain[vi_pipe];
    APC_STATIC_ASSERT(sizeof(awb_default->agc_tbl) == sizeof(imx586_awb_agc), "B090 AWB AGC ABI drift");
    APC_STATIC_ASSERT(sizeof(awb_default->ccm) == sizeof(imx586_awb_ccm), "B090 AWB CCM ABI drift");
    memcpy(&awb_default->agc_tbl, imx586_awb_agc, sizeof(imx586_awb_agc));
    memcpy(&awb_default->ccm, imx586_awb_ccm, sizeof(imx586_awb_ccm));
    awb_default->init_rgain = imx586_init_wb[vi_pipe][0];
    awb_default->init_ggain = imx586_init_wb[vi_pipe][1];
    awb_default->init_bgain = imx586_init_wb[vi_pipe][2];
    return TD_SUCCESS;
}

static td_s32 imx586_set_bus_info(ot_vi_pipe vi_pipe, ot_isp_sns_commbus bus)
{
    if (!imx586_valid_pipe(vi_pipe)) {
        return TD_FAILURE;
    }
    imx586_bus[vi_pipe] = bus;
    return TD_SUCCESS;
}

static td_s32 imx586_set_init(ot_vi_pipe vi_pipe, ot_isp_init_attr *attr)
{
    if (!imx586_valid_pipe(vi_pipe) || attr == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    imx586_init_exposure[vi_pipe] = attr->exposure;
    imx586_lines_per_500ms[vi_pipe] = attr->lines_per500ms;
    imx586_init_wb[vi_pipe][0] = attr->wb_r_gain;
    imx586_init_wb[vi_pipe][1] = attr->wb_g_gain;
    imx586_init_wb[vi_pipe][2] = attr->wb_b_gain;
    imx586_init_wb[vi_pipe][3] = attr->wb_g_gain;
    imx586_sample_r_gain[vi_pipe] = attr->sample_r_gain;
    imx586_sample_b_gain[vi_pipe] = attr->sample_b_gain;
    return TD_SUCCESS;
}

static td_s32 imx586_register_callback(ot_vi_pipe vi_pipe, ot_isp_3a_alg_lib *ae_lib,
    ot_isp_3a_alg_lib *awb_lib)
{
    ot_isp_sns_attr_info attr = { .sensor_id = MT11_IMX586_SENSOR_ID };
    ot_isp_sensor_register isp_register = { 0 };
    ot_isp_ae_sensor_register ae_register = { 0 };
    ot_isp_awb_sensor_register awb_register = { 0 };
    td_s32 ret;

    if (!imx586_valid_pipe(vi_pipe) || ae_lib == TD_NULL || awb_lib == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    if (imx586_state[vi_pipe] == TD_NULL) {
        imx586_state[vi_pipe] = (ot_isp_sns_state*)(calloc(1, sizeof(*imx586_state[vi_pipe])));
        if (imx586_state[vi_pipe] == TD_NULL) {
            return OT_ERR_ISP_NOMEM;
        }
    }
    imx586_sensor_global_init(vi_pipe);
    isp_register.sns_exp.pfn_cmos_sensor_init = imx586_sensor_init;
    isp_register.sns_exp.pfn_cmos_sensor_exit = imx586_sensor_exit;
    isp_register.sns_exp.pfn_cmos_sensor_global_init = imx586_sensor_global_init;
    isp_register.sns_exp.pfn_cmos_set_image_mode = imx586_set_image_mode;
    isp_register.sns_exp.pfn_cmos_set_wdr_mode = imx586_set_wdr_mode;
    isp_register.sns_exp.pfn_cmos_get_isp_default = imx586_get_isp_default;
    isp_register.sns_exp.pfn_cmos_get_isp_black_level = imx586_get_black_level;
    isp_register.sns_exp.pfn_cmos_get_sns_reg_info = imx586_get_sns_regs_info;
    isp_register.sns_exp.pfn_cmos_set_pixel_detect = imx586_set_pixel_detect;
    ret = ss_mpi_isp_sensor_reg_callback(vi_pipe, &attr, &isp_register);
    if (ret != TD_SUCCESS) {
        return ret;
    }
    ae_register.sns_exp.pfn_cmos_get_ae_default = imx586_get_ae_default;
    ae_register.sns_exp.pfn_cmos_fps_set = imx586_fps_set;
    ae_register.sns_exp.pfn_cmos_slow_framerate_set = imx586_slow_framerate_set;
    ae_register.sns_exp.pfn_cmos_inttime_update = imx586_inttime_update;
    ae_register.sns_exp.pfn_cmos_gains_update = imx586_gains_update;
    ae_register.sns_exp.pfn_cmos_again_calc_table = imx586_again_calc;
    ae_register.sns_exp.pfn_cmos_ae_fswdr_attr_set = imx586_ae_fswdr_attr_set;
    ret = ss_mpi_ae_sensor_reg_callback(vi_pipe, ae_lib, &attr, &ae_register);
    if (ret != TD_SUCCESS) {
        (void)ss_mpi_isp_sensor_unreg_callback(vi_pipe, MT11_IMX586_SENSOR_ID);
        return ret;
    }
    awb_register.sns_exp.pfn_cmos_get_awb_default = imx586_get_awb_default;
    ret = ss_mpi_awb_sensor_reg_callback(vi_pipe, awb_lib, &attr, &awb_register);
    if (ret != TD_SUCCESS) {
        (void)ss_mpi_ae_sensor_unreg_callback(vi_pipe, ae_lib, MT11_IMX586_SENSOR_ID);
        (void)ss_mpi_isp_sensor_unreg_callback(vi_pipe, MT11_IMX586_SENSOR_ID);
    }
    return ret;
}

static td_s32 imx586_unregister_callback(ot_vi_pipe vi_pipe, ot_isp_3a_alg_lib *ae_lib,
    ot_isp_3a_alg_lib *awb_lib)
{
    td_s32 ret;
    if (!imx586_valid_pipe(vi_pipe) || ae_lib == TD_NULL || awb_lib == TD_NULL) {
        return IMX586_ERR_NULL;
    }
    ret = ss_mpi_isp_sensor_unreg_callback(vi_pipe, MT11_IMX586_SENSOR_ID);
    if (ret == TD_SUCCESS) {
        ret = ss_mpi_ae_sensor_unreg_callback(vi_pipe, ae_lib, MT11_IMX586_SENSOR_ID);
    }
    if (ret == TD_SUCCESS) {
        ret = ss_mpi_awb_sensor_unreg_callback(vi_pipe, awb_lib, MT11_IMX586_SENSOR_ID);
    }
    if (ret == TD_SUCCESS) {
        free(imx586_state[vi_pipe]);
        imx586_state[vi_pipe] = TD_NULL;
    }
    return ret;
}

ot_isp_sns_obj g_sns_mt11_imx586_obj = {
    .pfn_register_callback = imx586_register_callback,
    .pfn_un_register_callback = imx586_unregister_callback,
    .pfn_set_bus_info = imx586_set_bus_info,
    .pfn_set_bus_ex_info = TD_NULL,
    .pfn_standby = mt11_imx586_standby,
    .pfn_restart = mt11_imx586_restart,
    .pfn_mirror_flip = TD_NULL,
    .pfn_set_blc_clamp = TD_NULL,
    .pfn_write_reg = mt11_imx586_write_register,
    .pfn_read_reg = mt11_imx586_read_register,
    .pfn_set_init = imx586_set_init,
};

/*
 * The SS928 sample support expects the IMX678 object name for this transport
 * wiring. Export that compatibility alias only for the MT11 target build.
 */
#ifdef MT11_IMX586_EXPORT_IMX678_ALIAS
extern ot_isp_sns_obj g_sns_imx678_obj
    __attribute__((alias("g_sns_mt11_imx586_obj")));
#endif

#ifdef MT11_IMX586_TEST_API
const ot_isp_sns_state *mt11_imx586_test_state(ot_vi_pipe vi_pipe)
{
    return imx586_get_state(vi_pipe);
}

td_s32 mt11_imx586_test_create(ot_vi_pipe vi_pipe)
{
    if (!imx586_valid_pipe(vi_pipe)) {
        return TD_FAILURE;
    }
    if (imx586_state[vi_pipe] == TD_NULL) {
        imx586_state[vi_pipe] = (ot_isp_sns_state*)(calloc(1, sizeof(*imx586_state[vi_pipe])));
    }
    if (imx586_state[vi_pipe] == TD_NULL) {
        return OT_ERR_ISP_NOMEM;
    }
    imx586_sensor_global_init(vi_pipe);
    return TD_SUCCESS;
}

void mt11_imx586_test_destroy(ot_vi_pipe vi_pipe)
{
    if (imx586_valid_pipe(vi_pipe)) {
        free(imx586_state[vi_pipe]);
        imx586_state[vi_pipe] = TD_NULL;
    }
}
#endif
