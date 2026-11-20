#pragma once
#include "mi/abi.h"
namespace ca_zt30 {
/* One captured vendor scene, not the advertised/public stream definition.
 * Wide is cropped before ISP, zoom and wide both use real-time ISP->SCL.
 * These recipes do not activate hardware or replace the vendor supervisor.
 */
struct RgbPath {
    uint32_t sensor_pad;
    uint8_t sensor_resolution;
    uint32_t vif_group,vif_device,isp_channel,scl_channel;
    Rect sensor_capture,vif_crop;
    uint32_t bayer,precision;
    bool mirrored,flipped;
    const char *iq_file;
};
constexpr RgbPath ZoomPath={0,7,0,0,0,0,{2,2,3840,2160},{2,2,3840,2160},3,1,true,true,
                            "/config/iqfile/359G_imx415_v2.bin"};
constexpr RgbPath WidePath={1,1,2,8,1,1,{0,0,2592,1944},{0,0,2592,1458},0,1,false,false,
                            "/config/iqfile/359G_imx335_v3.bin"};
/* IQ path is configuration evidence, not proof of load API/key/order.
 * Geometry and final orientation have independent proc corroboration.
 */
inline VifGroup vif_group(const RgbPath&) { return {4,0,0,2,0,0,0}; }
inline VifDevice vif_device(const RgbPath &path) {
    return {BayerBase+path.precision*BayerKinds+path.bayer,path.vif_crop,0,0,{0,0,0}};
}
inline VifPort vif_port(const RgbPath &path) {
    return {path.vif_crop,{path.vif_crop.width,path.vif_crop.height},
            BayerBase+path.precision*BayerKinds+path.bayer,0};
}
inline IspChannel isp_channel(const RgbPath &path) {
    IspChannel value{};value.sensor_id_mask=1U<<path.sensor_pad;return value;
}
inline IspParameters isp_parameters() { return {0,2,0,0,{0,0},0}; }
inline IspPort isp_port() { return {{0,0,0,0},Yuyv}; }
inline SclPort scl_port(uint16_t width,uint16_t height) {
    return {{0,0,0,0},{width,height},0,0,{0,0},Nv12,0};
}
inline H26xChannel h265_channel(uint32_t width,uint32_t height,uint32_t bitrate) {
    H26xChannel value{};
    value.codec=3;value.max_width=3840;value.max_height=2160;
    value.by_frame=1;value.width=width;value.height=height;
    value.rate_mode=8;value.gop=60;value.fps_num=30;value.fps_den=1;
    value.bitrate=bitrate;return value;
}
// Candidate H.264 schema: same audited 76byte MI create-channel prefix.
// Kept separate from the qualified H.265 defaults; requires live qualification.
inline H26xChannel h264_channel(uint32_t width,uint32_t height,uint32_t bitrate) {
    auto value=h265_channel(width,height,bitrate);
    value.codec=2;value.profile=1;value.rate_mode=1;
    value.max_width=width;value.max_height=height;value.reference_frames=1;
    value.stat_time=1;value.average_level=1;
    return value;
}
inline Port vif_output(const RgbPath &path) { return {Vif,path.vif_device,0,0}; }
inline Port isp_input(const RgbPath &path) { return {Isp,0,path.isp_channel,0}; }
inline Port scl_input(const RgbPath &path) { return {Scl,0,path.scl_channel,0}; }
} // namespace ca_zt30
