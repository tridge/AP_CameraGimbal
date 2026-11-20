#pragma once
#include <cstdint>
#include <cstddef>
namespace ca_zt30 {
// SigmaStar ISP API, verified against the retained 20220422 ARM wrappers:
// https://wx.comake.online/doc/doc/Tiramisu_XLS00V024-20220523/customer/development/isp/api.html
// Get before Set: retain per-sensor IQ tables and fields outside these controls.
struct IspLevel { uint32_t enabled,mode,automatic[16],manual; };
struct IspSaturation { uint32_t enabled,mode; uint8_t automatic[16][24],manual[24]; };
struct IspWhiteBalance { uint32_t state,mode; uint16_t gains[4]; uint8_t automatic[1448]; };
struct IspEv { int32_t value; uint32_t denominator; };
struct IspExposure { uint32_t aperture,sensor_gain,isp_gain,shutter_us; };
struct IspExposureLimit {
    uint32_t min_shutter,max_shutter,min_aperture,max_aperture;
    uint32_t min_sensor_gain,min_isp_gain,max_sensor_gain,max_isp_gain;
};
static_assert(sizeof(IspLevel)==76 && offsetof(IspLevel,manual)==72,"ZT30 level copy76");
static_assert(sizeof(IspSaturation)==416 && offsetof(IspSaturation,manual)==392,"ZT30 saturation copy416");
static_assert(sizeof(IspWhiteBalance)==1464 && offsetof(IspWhiteBalance,gains)==8,"ZT30 AWB copy1464");
static_assert(sizeof(IspExposure)==16 && offsetof(IspExposure,shutter_us)==12,"ZT30 exposure copy16");
static_assert(sizeof(IspExposureLimit)==32 && sizeof(IspEv)==8,"ZT30 AE copy sizes");
using IspAttribute=int(*)(uint32_t,uint32_t,void*);
struct ImageApi {
    IspAttribute get_brightness=nullptr,set_brightness=nullptr;
    IspAttribute get_contrast=nullptr,set_contrast=nullptr;
    IspAttribute get_saturation=nullptr,set_saturation=nullptr;
    IspAttribute get_wb=nullptr,set_wb=nullptr;
    IspAttribute get_ev=nullptr,set_ev=nullptr;
    IspAttribute get_mode=nullptr,set_mode=nullptr;
    IspAttribute get_manual=nullptr,set_manual=nullptr;
    IspAttribute get_limit=nullptr,set_limit=nullptr;
    IspAttribute get_metering=nullptr,set_metering=nullptr;
    IspAttribute query=nullptr;
};
}
