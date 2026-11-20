#pragma once
/* ZT30 SDK 20220422 / ARM EABI. Flat structures, not A8 loader policies.
 * Copied sizes and scalar prefixes: pinned SDK disassembly in port analysis.
 * Field interpretation: m6 definitions corroborated by the 20261009 vendor
 * startup payload capture and restored /proc/mi_modules state. Native loading
 * and activation remain gated separately; equal sizes alone are insufficient.
 */
#include <cstddef>
#include <cstdint>
namespace ca_zt30 {
struct Rect { uint16_t x,y,width,height; };
struct Dimension { uint16_t width,height; };
struct Port { uint32_t module,device,channel,port; };
enum Module : uint32_t { Venc=2,Vif=6,Sys=9,Vdisp=14,Isp=33,Scl=34 };
enum PixelFormat : uint32_t { Yuyv=0,Nv12=11,BayerBase=20 };
constexpr uint32_t BayerKinds=12;
/* Bind names follow m6 enum candidates; raw values1/4 are captured. */
constexpr uint32_t FrameBind=1,RealtimeBind=4;
struct SensorPad {
    uint32_t plane_count,interface_type,hdr;
    uint32_t interface_union[9]; // MIPI/BT656/parallel interpretation is separate.
    uint8_t early_init,reserved[3];
};
struct SensorPlane {
    uint32_t plane_id;
    char name[32];
    Rect capture;
    uint32_t bayer,precision,hdr_source,shutter_us,sensor_gain,compensation_gain,pixel_format;
};
struct SensorResolution {
    Rect capture;
    Dimension output;
    uint32_t max_fps,min_fps;
    char description[32];
};
struct VifGroup { uint32_t interface_type,work,hdr,edge,clock,interlace,stitch; };
struct VifDevice {
    uint32_t pixel_format;
    Rect capture;
    uint32_t field;
    uint8_t half_h_scan,reserved[3];
};
struct VifPort { Rect capture;Dimension output;uint32_t pixel_format,frame_rate_mode; };
struct IspIqVersion { uint32_t revision,size;uint8_t data[64]; };
struct IspChannel { uint32_t sensor_id_mask;IspIqVersion iq_version;uint32_t sync_3a; };
struct IspParameters { uint32_t hdr,level_3dnr;uint8_t mirror,flip,reserved[2];uint32_t rotation; };
struct IspPort { Rect crop;uint32_t pixel_format; };
struct SclPort {
    Rect crop;Dimension output;
    uint8_t mirror,flip,reserved[2];
    uint32_t pixel_format,compression;
};
/* Only the measured H26x CBR branch is modelled here. JPEG and other rate
 * modes have different unions; never reinterpret them with these fields.
 * rate_extension is the ARM32 ABI pointer slot, required to be zero by the
 * initial native plan. It is never dereferenced or persisted as a pointer.
 */
struct H26xChannel {
    uint32_t codec,max_width,max_height,buffer_size,profile;
    uint8_t by_frame,reserved[3];
    uint32_t width,height,b_frames,reference_frames;
    uint32_t rate_mode,gop,stat_time,fps_num,fps_den,bitrate,average_level;
    uint32_t rate_union_tail,rate_extension;
};
static_assert(sizeof(Rect)==8 && sizeof(Dimension)==4,"MI geometry ABI");
static_assert(sizeof(Port)==16 && offsetof(Port,port)==12,"MI port ABI");
static_assert(sizeof(SensorPad)==52 && offsetof(SensorPad,early_init)==48,"SNR pad copy");
static_assert(sizeof(SensorPlane)==72 && offsetof(SensorPlane,capture)==36 && offsetof(SensorPlane,pixel_format)==68,"SNR plane copy");
static_assert(sizeof(SensorResolution)==52 && offsetof(SensorResolution,description)==20,"SNR resolution copy");
static_assert(sizeof(VifGroup)==28 && sizeof(VifDevice)==20 && sizeof(VifPort)==20,"VIF copies");
static_assert(sizeof(IspChannel)==80 && offsetof(IspChannel,sync_3a)==76,"ISP channel copy");
static_assert(sizeof(IspParameters)==16 && offsetof(IspParameters,rotation)==12 && sizeof(IspPort)==12,"ISP copies");
static_assert(sizeof(SclPort)==24 && offsetof(SclPort,pixel_format)==16,"SCL copy");
static_assert(sizeof(H26xChannel)==76 && offsetof(H26xChannel,width)==24 && offsetof(H26xChannel,rate_mode)==40 && offsetof(H26xChannel,fps_den)==56 && offsetof(H26xChannel,rate_extension)==72,"VENC channel copy");
static_assert(offsetof(SensorPad,interface_union)==12 && offsetof(SensorPlane,bayer)==44 && offsetof(SensorPlane,precision)==48,"SNR field candidates");
static_assert(offsetof(SensorResolution,output)==8 && offsetof(SensorResolution,max_fps)==12,"SNR resolution fields");
static_assert(offsetof(VifDevice,capture)==4 && offsetof(VifDevice,half_h_scan)==16,"VIF device fields");
static_assert(offsetof(VifPort,output)==8 && offsetof(VifPort,pixel_format)==12,"VIF port fields");
static_assert(offsetof(IspChannel,iq_version)==4 && offsetof(IspParameters,mirror)==8 && offsetof(IspPort,pixel_format)==8,"ISP field candidates");
static_assert(offsetof(SclPort,output)==8 && offsetof(SclPort,mirror)==12 && offsetof(SclPort,compression)==20,"SCL field candidates");
static_assert(offsetof(H26xChannel,by_frame)==20 && offsetof(H26xChannel,gop)==44 && offsetof(H26xChannel,bitrate)==60,"H26x field candidates");
} // namespace ca_zt30
