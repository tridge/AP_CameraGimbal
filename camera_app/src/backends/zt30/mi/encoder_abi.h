#pragma once
#include "abi.h"
namespace ca_zt30 {
/* Captured 20220422 ARM32 VENC wrapper layouts. Integer pointer slots keep
 * offline host fixtures identical to ARM; they are not host pointers. Native
 * stream access remains gated on owned encoder/packet lifetime qualification.
 * Scalar field meanings follow m6 candidates; copy sizes/strides and key
 * offsets are independently proved by the ZT30 SDK body audit.
 */
struct EncoderDevice { uint32_t max_width,max_height; };
struct EncoderStatus {
    uint32_t left_pictures,left_bytes,left_frames,left_milliseconds,current_packs;
    uint32_t left_receive_pictures,left_encode_pictures,fps_num,fps_den,bitrate;
};
struct EncoderPacket {
    uint64_t physical_address;
    uint32_t data_address,length;
    uint64_t pts;
    uint8_t frame_flags,reserved0[3];
    uint32_t opaque_nalu,offset,opaque_count;
    uint8_t frame_quality,reserved1[3];
    int32_t picture_order;
    uint8_t opaque_tail[136]; // offset48 onward: ParseNalu contract not qualified.
};
struct EncoderStream {
    uint32_t packets_address,count,sequence,handle;
    uint32_t codec_information[14];
};
// ZT30 wrappers copy28. Kernel Set/Get independently map these byte/word
// offsets. Timing semantics additionally require the baseline SPS/live guard.
struct H264Vui {
    uint8_t aspect_present,aspect_idc,overscan_present,overscan_appropriate;
    uint16_t sar_width,sar_height;
    uint8_t timing_present,fixed_rate,reserved[2];
    uint32_t units,time_scale;
    uint8_t signal_present,video_format,full_range,colour_present;
    uint8_t primaries,transfer,matrix,reserved_tail;
};
static_assert(sizeof(H264Vui)==28 && offsetof(H264Vui,timing_present)==8 &&
              offsetof(H264Vui,fixed_rate)==9 && offsetof(H264Vui,units)==12 &&
              offsetof(H264Vui,time_scale)==16,"H264 VUI copy28/offsets");
inline bool baseline_h264_vui(const H264Vui &value) {
    // Before any Set, match the already-captured default SPS timing group
    // at exactly one aligned location. Reject a different/ambiguous schema.
    const auto *bytes=reinterpret_cast<const uint8_t*>(&value);
    unsigned matches=0,location=0;
    for(unsigned offset=4;offset+8<=sizeof(value);offset+=4) {
        uint32_t units=0,scale=0;
        // Read integer members without aliasing or unaligned dereferences.
        __builtin_memcpy(&units,bytes+offset,4);__builtin_memcpy(&scale,bytes+offset+4,4);
        if(bytes[offset-4]==1 && bytes[offset-3]==0 && units==1001 && scale==60060) { matches++;location=offset; }
    }
    return matches==1 && location==12;
}
// Measured JPEG union: vendor codec4, device8/channels4..6. The entire
// create payload is76bytes, with a different picture/rate union from H26x.
struct JpegChannel {
    uint32_t codec,max_width,max_height,buffer_size;
    uint8_t by_frame,reserved[3];uint32_t width,height;
    uint8_t thumbnail,reserved_thumbnail[3];uint32_t marks_per_row,reserved_union;
    uint32_t rate_mode,fps_num,fps_den,quality,rate_padding[4],rate_extension;
};
static_assert(sizeof(JpegChannel)==76 && offsetof(JpegChannel,width)==20 &&
              offsetof(JpegChannel,rate_mode)==40 && offsetof(JpegChannel,quality)==52 &&
              offsetof(JpegChannel,rate_extension)==72,"captured JPEG create union");
inline JpegChannel jpeg_channel(uint32_t width,uint32_t height,unsigned quality=90) {
    JpegChannel a{};a.codec=4;a.max_width=3840;a.max_height=2160;
    a.by_frame=1;a.width=width;a.height=height;a.rate_mode=7;
    a.fps_num=1;a.fps_den=1;a.quality=quality;return a;
}
struct EncoderApi {
    int (*create_device)(uint32_t,EncoderDevice*);
    int (*destroy_device)(uint32_t);
    int (*create_channel)(uint32_t,uint32_t,H26xChannel*);
    int (*destroy_channel)(uint32_t,uint32_t);
    int (*set_input_source)(uint32_t,uint32_t,const uint32_t*);
    int (*get_h264_vui)(uint32_t,uint32_t,H264Vui*);
    int (*set_h264_vui)(uint32_t,uint32_t,const H264Vui*);
    int (*start)(uint32_t,uint32_t);
    int (*stop)(uint32_t,uint32_t);
    int (*start_once)(uint32_t,uint32_t,const int32_t*);
    // GetFd returns an SDK-cached borrowed fd; CloseFd is a no-op in this SDK.
    // Do not close it, or destroy/recreate its channel within the same SDK load.
    int (*get_fd)(uint32_t,uint32_t);
    int (*close_fd)(uint32_t,uint32_t);
    int (*query)(uint32_t,uint32_t,EncoderStatus*);
    // packets_address/count are caller-supplied capacity on entry. Returned
    // count is NOT updated by GetStream: derive its filled prefix before
    // ReleaseStream. Preserve all other fields. Returned mappings are borrowed
    // until ReleaseStream, another
    // GetStream, or DestroyChn. Exactly one consumer; finite timeout only.
    int (*get_stream)(uint32_t,uint32_t,EncoderStream*,int32_t);
    int (*release_stream)(uint32_t,uint32_t,EncoderStream*);
    int (*request_idr)(uint32_t,uint32_t,uint8_t);
};
static_assert(sizeof(EncoderDevice)==8,"VENC device prefix");
static_assert(sizeof(EncoderStatus)==40 && offsetof(EncoderStatus,current_packs)==16,"VENC Query copy40");
static_assert(sizeof(EncoderPacket)==184 && alignof(EncoderPacket)==8,"VENC Get/Release packet stride184");
static_assert(offsetof(EncoderPacket,data_address)==8 && offsetof(EncoderPacket,length)==12,"VENC packet address/length");
static_assert(offsetof(EncoderPacket,pts)==16 && offsetof(EncoderPacket,frame_flags)==24,"VENC packet PTS/flags");
static_assert(offsetof(EncoderPacket,offset)==32 && offsetof(EncoderPacket,frame_quality)==40,"VENC packet offset/quality");
static_assert(offsetof(EncoderPacket,picture_order)==44 && offsetof(EncoderPacket,opaque_tail)==48,"VENC packet metadata");
static_assert(sizeof(EncoderStream)==72 && offsetof(EncoderStream,handle)==12,"VENC stream72/handle12");
} // namespace ca_zt30
