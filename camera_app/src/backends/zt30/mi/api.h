#pragma once
#include "abi.h"
namespace ca_zt30 {
/* Function shapes from the pinned20220422 SDK body audit. SYS has a chip
 * prefix; SNR uses pads, VIF uses groups/devices, ISP/SCL use device/channel.
 * Never prepend a SocId to every module. This table does not load the SDK.
 */
struct Api {
    int (*sys_init)(uint16_t);
    int (*sys_exit)(uint16_t);
    int (*sys_bind)(uint16_t,Port*,Port*,uint32_t,uint32_t,uint32_t,uint32_t);
    int (*sys_unbind)(uint16_t,Port*,Port*);
    int (*sys_depth)(uint16_t,Port*,uint32_t,uint32_t);
    int (*snr_init)(void*);
    int (*snr_plane_mode)(uint32_t,uint8_t);
    int (*snr_count)(uint32_t,uint32_t*);
    int (*snr_resolution)(uint32_t,uint8_t,SensorResolution*);
    int (*snr_set_resolution)(uint32_t,uint8_t);
    int (*snr_fps)(uint32_t,uint32_t);
    int (*snr_orientation)(uint32_t,uint8_t,uint8_t);
    int (*snr_enable)(uint32_t);
    int (*snr_disable)(uint32_t);
    int (*snr_pad)(uint32_t,SensorPad*);
    int (*snr_plane)(uint32_t,uint32_t,SensorPlane*);
    int (*vif_create_group)(uint32_t,VifGroup*);
    int (*vif_destroy_group)(uint32_t);
    int (*vif_set_device)(uint32_t,VifDevice*);
    int (*vif_enable_device)(uint32_t);
    int (*vif_disable_device)(uint32_t);
    int (*vif_set_port)(uint32_t,uint32_t,VifPort*);
    int (*vif_enable_port)(uint32_t,uint32_t);
    int (*vif_disable_port)(uint32_t,uint32_t);
    int (*isp_create_device)(uint32_t,uint32_t*);
    int (*isp_destroy_device)(uint32_t);
    int (*isp_create_channel)(uint32_t,uint32_t,IspChannel*);
    int (*isp_destroy_channel)(uint32_t,uint32_t);
    int (*isp_disable_3a)(uint32_t,uint32_t);
    int (*isp_set_parameters)(uint32_t,uint32_t,IspParameters*);
    int (*isp_start_channel)(uint32_t,uint32_t);
    int (*isp_stop_channel)(uint32_t,uint32_t);
    int (*isp_set_port)(uint32_t,uint32_t,uint32_t,IspPort*);
    int (*isp_enable_port)(uint32_t,uint32_t,uint32_t);
    int (*isp_disable_port)(uint32_t,uint32_t,uint32_t);
    int (*isp_load_iq)(uint32_t,uint32_t,const char*,uint32_t);
    int (*scl_create_device)(uint32_t,uint32_t*);
    int (*scl_destroy_device)(uint32_t);
    int (*scl_create_channel)(uint32_t,uint32_t,uint32_t*);
    int (*scl_destroy_channel)(uint32_t,uint32_t);
    int (*scl_set_rotation)(uint32_t,uint32_t,uint32_t*);
    int (*scl_start_channel)(uint32_t,uint32_t);
    int (*scl_stop_channel)(uint32_t,uint32_t);
    int (*scl_set_port)(uint32_t,uint32_t,uint32_t,SclPort*);
    int (*scl_enable_port)(uint32_t,uint32_t,uint32_t);
    int (*scl_disable_port)(uint32_t,uint32_t,uint32_t);
};
} // namespace ca_zt30
