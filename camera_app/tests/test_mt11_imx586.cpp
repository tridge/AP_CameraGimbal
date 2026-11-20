#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "imx586.h"
#include "ot_common_ae.h"
#include "ot_common_awb.h"
#include "ot_common_sns.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"
#include "ss_mpi_isp.h"

static ot_isp_sensor_register captured_isp;
static ot_isp_ae_sensor_register captured_ae;
static ot_isp_awb_sensor_register captured_awb;
static ot_sensor_id captured_sensor_id;

td_s32 ss_mpi_isp_sensor_reg_callback(ot_vi_pipe pipe, ot_isp_sns_attr_info *attr,
    const ot_isp_sensor_register *reg)
{
    assert(pipe == 0);
    captured_sensor_id = attr->sensor_id;
    captured_isp = *reg;
    return TD_SUCCESS;
}

td_s32 ss_mpi_ae_sensor_reg_callback(ot_vi_pipe pipe, ot_isp_3a_alg_lib *lib,
    const ot_isp_sns_attr_info *attr, ot_isp_ae_sensor_register *reg)
{
    assert(pipe == 0 && lib != NULL && attr->sensor_id == MT11_IMX586_SENSOR_ID);
    captured_ae = *reg;
    return TD_SUCCESS;
}

td_s32 ss_mpi_awb_sensor_reg_callback(ot_vi_pipe pipe, ot_isp_3a_alg_lib *lib,
    ot_isp_sns_attr_info *attr, ot_isp_awb_sensor_register *reg)
{
    assert(pipe == 0 && lib != NULL && attr->sensor_id == MT11_IMX586_SENSOR_ID);
    captured_awb = *reg;
    return TD_SUCCESS;
}

td_s32 ss_mpi_isp_sensor_unreg_callback(ot_vi_pipe pipe, ot_sensor_id sensor_id)
{
    assert(pipe == 0 && sensor_id == MT11_IMX586_SENSOR_ID);
    return TD_SUCCESS;
}

td_s32 ss_mpi_ae_sensor_unreg_callback(ot_vi_pipe pipe, const ot_isp_3a_alg_lib *lib,
    ot_sensor_id sensor_id)
{
    assert(pipe == 0 && lib != NULL && sensor_id == MT11_IMX586_SENSOR_ID);
    return TD_SUCCESS;
}

td_s32 ss_mpi_awb_sensor_unreg_callback(ot_vi_pipe pipe, const ot_isp_3a_alg_lib *lib,
    ot_sensor_id sensor_id)
{
    assert(pipe == 0 && lib != NULL && sensor_id == MT11_IMX586_SENSOR_ID);
    return TD_SUCCESS;
}

int main(void)
{
    ot_isp_3a_alg_lib ae_lib = { 0 };
    ot_isp_3a_alg_lib awb_lib = { 0 };
    ot_isp_sns_commbus bus = { .i2c_dev = 3 };
    ot_isp_sns_regs_info regs = { (ot_isp_sns_type)(0) };
    ot_isp_ae_sensor_default ae_default;
    ot_isp_awb_sensor_default awb_default;
    ot_isp_cmos_black_level black_level;
    td_u32 again_lin;
    td_u32 again_db = 0;
    unsigned i;

    assert(g_sns_mt11_imx586_obj.pfn_set_bus_info(0, bus) == TD_SUCCESS);
    assert(g_sns_mt11_imx586_obj.pfn_register_callback(0, &ae_lib, &awb_lib) == TD_SUCCESS);
    assert(captured_sensor_id == MT11_IMX586_SENSOR_ID);
    assert(captured_isp.sns_exp.pfn_cmos_get_sns_reg_info != NULL);
    assert(captured_ae.sns_exp.pfn_cmos_again_calc_table != NULL);
    assert(captured_awb.sns_exp.pfn_cmos_get_awb_default != NULL);

    assert(captured_isp.sns_exp.pfn_cmos_get_sns_reg_info(0, &regs) == TD_SUCCESS);
    assert(regs.sns_type == OT_ISP_SNS_I2C_TYPE);
    assert(regs.reg_num == 8 && regs.cfg2_valid_delay_max == 2);
    assert(regs.com_bus.i2c_dev == 3);
    for (i = 0; i < regs.reg_num; ++i) {
        assert(regs.i2c_data[i].dev_addr == 0x34);
        assert(regs.i2c_data[i].addr_byte_num == 2);
        assert(regs.i2c_data[i].data_byte_num == 1);
    }
    assert(regs.i2c_data[0].reg_addr == 0x0202);
    assert(regs.i2c_data[7].reg_addr == 0x0341);
    assert(regs.i2c_data[0].data == 0x0b && regs.i2c_data[1].data == 0xc4);
    assert(regs.i2c_data[4].data == 0x01 && regs.i2c_data[5].data == 0x00);
    assert(regs.i2c_data[6].data == 0x0b && regs.i2c_data[7].data == 0xf8);

    captured_ae.sns_exp.pfn_cmos_inttime_update(0, 1);
    captured_ae.sns_exp.pfn_cmos_gains_update(0, 0x3ab, 0xabc);
    memset(&regs, 0, sizeof(regs));
    regs.config = TD_TRUE;
    assert(captured_isp.sns_exp.pfn_cmos_get_sns_reg_info(0, &regs) == TD_SUCCESS);
    assert(regs.i2c_data[0].data == 0 && regs.i2c_data[1].data == 16);
    assert(regs.i2c_data[2].data == 3 && regs.i2c_data[3].data == 0xab);
    assert(regs.i2c_data[4].data == 0x0a && regs.i2c_data[5].data == 0xbc);

    again_lin = 1024;
    captured_ae.sns_exp.pfn_cmos_again_calc_table(0, &again_lin, &again_db);
    assert(again_db == 112 && again_lin == 1149);
    again_lin = 65536;
    captured_ae.sns_exp.pfn_cmos_again_calc_table(0, &again_lin, &again_db);
    assert(again_db == 1008 && again_lin == 65536);

    assert(captured_ae.sns_exp.pfn_cmos_get_ae_default(0, &ae_default) == TD_SUCCESS);
    assert(ae_default.full_lines_std == MT11_IMX586_VMAX);
    assert(ae_default.full_lines_max == 65535);
    assert(ae_default.min_int_time == 16);
    assert(ae_default.max_again == 65535 && ae_default.min_again == 1151);
    assert(ae_default.ae_compensation == 56 && ae_default.ae_run_interval == 8);
    assert(ae_default.int_time_accu.accuracy == 2.0f);
    assert(ae_default.ae_route_ex_valid == TD_TRUE);
    assert(ae_default.ae_route_attr_ex.total_num == 5);
    assert(ae_default.ae_route_attr_ex.route_ex_node[0].int_time == 174);
    assert(ae_default.ae_route_attr_ex.route_ex_node[2].a_gain == 65535);
    assert(ae_default.ae_route_attr_ex.route_ex_node[3].d_gain == 16380);
    assert(ae_default.ae_route_attr_ex.route_ex_node[4].isp_d_gain == 261120);
    captured_ae.sns_exp.pfn_cmos_fps_set(0, 15.0f, &ae_default);
    assert(ae_default.full_lines_std == MT11_IMX586_VMAX * 2);
    assert(ae_default.max_int_time == MT11_IMX586_VMAX * 2 - 48);

    assert(captured_isp.sns_exp.pfn_cmos_get_isp_black_level(0, &black_level) == TD_SUCCESS);
    assert(black_level.user_black_level[0][0] == 1200);
    assert(black_level.manual_attr.black_level[0][0] == 1024);
    assert(captured_awb.sns_exp.pfn_cmos_get_awb_default(0, &awb_default) == TD_SUCCESS);
    assert(awb_default.wb_ref_temp == 4950);
    assert(awb_default.gain_offset[0] == 450 && awb_default.gain_offset[3] == 447);

    assert(g_sns_mt11_imx586_obj.pfn_un_register_callback(0, &ae_lib, &awb_lib) == TD_SUCCESS);
    puts("PASS: MT11 IMX586 callback/state tests");
    return 0;
}
