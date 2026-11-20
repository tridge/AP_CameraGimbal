#include "isp.h"
#include "pipeline.h"

#include "camera_app/log.h"

#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"
#include "ss_mpi_isp.h"

#include <string.h>
#include <errno.h>

static td_s32 set_csc(ot_vi_pipe pipe, const struct ca_config *config)
{
    ot_isp_csc_attr attr;
    td_s32 result = ss_mpi_isp_get_csc_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.luma = (td_u8)config->brightness;
    attr.satu = (td_u8)config->saturation;
    attr.contr = (td_u8)config->contrast;
    return ss_mpi_isp_set_csc_attr(pipe, &attr);
}

static td_u16 ev_bias(int tenths)
{
    if (tenths < 0) return (td_u16)((10240 + tenths * 896) / 10);
    return (td_u16)((10240 + tenths * 3072) / 10);
}

static td_s32 set_exposure(ot_vi_pipe pipe, const struct ca_config *config)
{
    static const td_u32 exposure_us[] = {
        0U, 33333U, 20000U, 10000U, 4000U, 2000U, 1333U, 1000U, 500U,
    };
    ot_isp_exposure_attr attr;
    td_s32 result = ss_mpi_isp_get_exposure_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;

    attr.op_type = config->iso == CA_ISO_AUTO && config->shutter == CA_SHUTTER_AUTO
        ? OT_OP_MODE_AUTO : OT_OP_MODE_MANUAL;
    attr.auto_attr.ev_bias = ev_bias(config->exposure_compensation);
    if (config->iso == CA_ISO_AUTO) {
        attr.manual_attr.a_gain_op_type = OT_OP_MODE_AUTO;
        attr.manual_attr.d_gain_op_type = OT_OP_MODE_AUTO;
        attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_AUTO;
    } else {
        td_u32 gain = 1024U << ((unsigned)config->iso - 1U);
        attr.manual_attr.a_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.d_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.a_gain = gain;
        attr.manual_attr.d_gain = 1024U;
        attr.manual_attr.isp_d_gain = 1024U;
    }
    if (config->shutter == CA_SHUTTER_AUTO) {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_AUTO;
    } else {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.exp_time = exposure_us[config->shutter];
    }
    return ss_mpi_isp_set_exposure_attr(pipe, &attr);
}

static td_s32 set_metering(ot_vi_pipe pipe, enum ca_metering_mode mode)
{
    ot_isp_stats_cfg stats;
    td_s32 result = ss_mpi_isp_get_stats_cfg(pipe, &stats);
    if (result != TD_SUCCESS) return result;
    memset(stats.ae_cfg.weight, 1, sizeof(stats.ae_cfg.weight));
    if (mode == CA_METERING_CENTER) {
        for (unsigned row = 5U; row <= 9U; row++) {
            for (unsigned column = 6U; column <= 10U; column++) {
                stats.ae_cfg.weight[row][column] = 8U;
            }
        }
    } else if (mode == CA_METERING_SPOT) {
        stats.ae_cfg.weight[OT_ISP_AE_ZONE_ROW / 2U]
                           [OT_ISP_AE_ZONE_COLUMN / 2U] = 15U;
    }
    return ss_mpi_isp_set_stats_cfg(pipe, &stats);
}

static td_s32 set_white_balance(ot_vi_pipe pipe, enum ca_white_balance mode)
{
    static const ot_isp_mwb_attr presets[] = {
        {0, 0, 0, 0},
        {486, 256, 256, 453},
        {427, 256, 256, 456},
        {392, 256, 256, 498},
        {416, 256, 256, 564},
    };
    ot_isp_wb_attr attr;
    td_s32 result = ss_mpi_isp_get_wb_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    if (mode == CA_WB_AUTO) {
        attr.op_type = OT_OP_MODE_AUTO;
    } else {
        attr.op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr = presets[mode];
    }
    return ss_mpi_isp_set_wb_attr(pipe, &attr);
}

td_s32 ca_mt11_apply_isp_config(const struct ca_config *config, bool live)
{
    if (config == NULL) return TD_FAILURE;
    for (ot_vi_pipe pipe = CA_MT11_FIRST_PIPE;
         pipe <= CA_MT11_WIDE_PIPE; pipe++) {
        td_s32 result;
        bool custom_exposure = config->exposure_compensation != 0 ||
                               config->iso != CA_ISO_AUTO ||
                               config->shutter != CA_SHUTTER_AUTO;
        if ((result = set_csc(pipe, config)) != TD_SUCCESS ||
            ((custom_exposure || live) &&
             (result = set_exposure(pipe, config)) != TD_SUCCESS) ||
            (result = set_metering(pipe, config->metering)) != TD_SUCCESS ||
            (result = set_white_balance(pipe, config->white_balance)) !=
                TD_SUCCESS) {
            ca_log("ISP configuration failed for pipe %d: 0x%x", pipe,
                   (unsigned)result);
            return result;
        }
    }
    ca_log("ISP configured brightness=%d saturation=%d contrast=%d ev=%d "
           "iso=%d shutter=%d metering=%d white_balance=%d",
           config->brightness, config->saturation, config->contrast,
           config->exposure_compensation, config->iso, config->shutter,
           config->metering, config->white_balance);
    return TD_SUCCESS;
}

int ca_mt11_exposure(unsigned lens, struct ca_exposure *s)
{
    if (lens>1) return -ENOTSUP; /* Thermal AGC is not RGB auto exposure. */
    ot_vi_pipe pipe=lens==0 ? CA_MT11_WIDE_PIPE : CA_MT11_ZOOM_PIPE;
    ot_isp_exp_info info= {};
    td_s32 result=ss_mpi_isp_query_exposure_info(pipe,&info);
    if (result!=TD_SUCCESS) return result;
    s->shutter_us=info.exp_time;
    s->analog_gain=info.a_gain/1024.0f;
    s->digital_gain=info.d_gain/1024.0f;
    s->isp_gain=info.isp_d_gain/1024.0f;
    s->luma=info.ave_lum;
    /* SDK histogram error is logged raw; it is not a luminance target. */
    s->error=info.hist_error;
    s->state=info.exposure_is_max ? CA_AE_STATE_LIMIT : 0;
    s->valid=CA_AE_SHUTTER|CA_AE_AGAIN|CA_AE_DGAIN|CA_AE_IGAIN|
             CA_AE_LUMA|CA_AE_ERROR|CA_AE_LIMIT;
    ot_isp_exposure_attr attr={(td_bool)(0)};
    result=ss_mpi_isp_get_exposure_attr(pipe,&attr);
    if (result!=TD_SUCCESS) return result;
    if (attr.op_type==OT_OP_MODE_AUTO) s->mode=CA_AE_AUTO;
    else {
        bool auto_gain=attr.manual_attr.a_gain_op_type==OT_OP_MODE_AUTO ||
            attr.manual_attr.d_gain_op_type==OT_OP_MODE_AUTO ||
            attr.manual_attr.ispd_gain_op_type==OT_OP_MODE_AUTO;
        bool auto_time=attr.manual_attr.exp_time_op_type==OT_OP_MODE_AUTO;
        s->mode=auto_gain ? (auto_time ? CA_AE_AUTO : CA_AE_SHUTTER_PRIORITY) :
                           (auto_time ? CA_AE_GAIN_PRIORITY : CA_AE_MANUAL);
    }
    s->valid|=CA_AE_MODE;
    /* This SDK does not expose the current target or convergence flag. */
    return 0;
}
