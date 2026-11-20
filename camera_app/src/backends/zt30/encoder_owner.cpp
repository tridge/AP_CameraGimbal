#include "encoder_owner.h"
#include "media_graph.h"
#include <cstring>
#include <cerrno>
namespace ca_zt30 {
bool EncoderOwner::check(int result,const char *call) {
    if(!result)return true;
    unhealthy=true;
    if(!failed_call) { failed_call=call;raw_status=result; }
    return false;
}
void EncoderOwner::cleanup(int result,const char *call,int &first) {
    if(result && !first)first=result;
    (void)check(result,call);
}
int EncoderOwner::open(EncoderCodec codec,H264Timing timing,EncoderSource source,const EncoderOptions &options) {
    if(used || options.width<16 || options.width>3840 || options.height<16 || options.height>2160 ||
       (options.width&1) || (options.height&1) || !options.bitrate ||
       (options.channel!=1 && options.channel!=3 && options.channel!=4 && options.channel!=7) || options.source_port>4)return -1;
    used=true;
    if(source!=EncoderSource::Zoom && source!=EncoderSource::Wide) {
        check(-1,"unsupported encoder source");return -1;
    }
    source_channel=uint32_t(source);
    if(codec!=EncoderCodec::H264 && codec!=EncoderCodec::H265) {
        check(-1,"unsupported encoder codec");return -1;
    }
    if(timing!=H264Timing::Fixed30 && timing!=H264Timing::ReadOnly) {
        check(-1,"unsupported H264 timing policy");return -1;
    }
    h264=codec==EncoderCodec::H264;
    if(h264 && (!a.get_h264_vui || (timing==H264Timing::Fixed30 && !a.set_h264_vui))) {
        check(-1,"H264 VUI functions unavailable");return -1;
    }
#define CALL(expr) do { if(!check((expr),#expr)) { close();return raw_status; } } while(0)
    channel_id=options.channel;source_port=options.source_port;
    EncoderDevice dev{3840,2160};
    if(options.own_device) { CALL(a.create_device(0,&dev));device=true; }
    auto attr=codec==EncoderCodec::H264 ? h264_channel(options.width,options.height,options.bitrate) :
                                       h265_channel(options.width,options.height,options.bitrate);
    CALL(a.create_channel(0,channel_id,&attr));channel=true;
    const uint32_t normal_input=0;
    CALL(a.set_input_source(0,channel_id,&normal_input));
    if(codec==EncoderCodec::H264) {
        CALL(a.get_h264_vui(0,channel_id,&initial_vui));got_default_vui=true;
        if(timing==H264Timing::Fixed30) {
            if(!baseline_h264_vui(initial_vui)) {
                check(-1,"H264 default VUI differs from baseline SPS");close();return raw_status;
            }
            auto requested=initial_vui;
            requested.timing_present=1;requested.fixed_rate=1;
            requested.units=1;requested.time_scale=60; // 30fps, two ticks per frame
            CALL(a.set_h264_vui(0,channel_id,&requested));
            CALL(a.get_h264_vui(0,channel_id,&verified_vui));
            // Check all consumed non-timing fields; ignore padding10/11/27.
            const auto *actual=reinterpret_cast<const uint8_t*>(&verified_vui);
            const auto *expected=reinterpret_cast<const uint8_t*>(&requested);
            if(std::memcmp(actual,expected,8) || std::memcmp(actual+20,expected+20,7) ||
               verified_vui.timing_present!=1 || verified_vui.fixed_rate!=1 ||
               verified_vui.units!=1 || verified_vui.time_scale!=60) {
                check(-1,"H264 VUI readback differs");close();return raw_status;
            }
        }
    } // Fixed30 only; ReadOnly preserves automatic timing.
    started=true; // Stop even a partially successful activation.
    CALL(a.start(0,channel_id));
    Port input{Scl,0,source_channel,source_port},destination{Venc,0,channel_id,0};
    CALL(s.sys_bind(0,&input,&destination,30,30,FrameBind,0));bound=true;
#undef CALL
    return 0;
}
int EncoderOwner::rebind(EncoderSource selected) {
    const unsigned next=unsigned(selected);
    if(next>1 || !bound || software || !channel || !started)return EINVAL;
    if(next==source_channel)return 0;
    Port before{Scl,0,source_channel,source_port},after{Scl,0,next,source_port},dest{Venc,device_id,channel_id,0};
    int rc=s.sys_unbind(0,&before,&dest);if(rc)return rc;
    rc=s.sys_bind(0,&after,&dest,30,30,FrameBind,0);
    if(rc) {
        // The old route must be restored before returning a recoverable error.
        if(s.sys_bind(0,&before,&dest,30,30,FrameBind,0))return EOWNERDEAD;
        return rc;
    }
    source_channel=next;
    return 0;
}
int EncoderOwner::open_thermal(unsigned selected,EncoderCodec codec) {
    if(used || (selected!=2 && selected!=6 && selected!=8) ||
       (codec!=EncoderCodec::H264 && codec!=EncoderCodec::H265))return -1;
    used=true;software=true;h264=codec==EncoderCodec::H264;channel_id=selected;
    if(!a.create_channel || !a.destroy_channel || !a.set_input_source || !a.start ||
       !a.stop || !a.query || !a.get_stream || !a.release_stream ||
       !m.pointer_slot || !m.copy) { check(-1,"thermal API unavailable");return -1; }
    auto attr=h264?h264_channel(640,512,1000000):h265_channel(640,512,1000000);
    attr.max_width=640;attr.max_height=512;attr.fps_num=15;attr.gop=30;
    if(!check(a.create_channel(0,channel_id,&attr),"thermal CreateChn"))return raw_status;
    channel=true;
    const uint32_t source=0;
    if(!check(a.set_input_source(0,channel_id,&source),"thermal SetInputSource"))return raw_status;
    started=true; // Stop also covers a partially successful start.
    if(!check(a.start(0,channel_id),"thermal StartRecvPic"))return raw_status;
    return 0;
}
int EncoderOwner::read_h264_vui(H264Vui &output) {
    output={};
    if(!h264 || !ready())return -1;
    if(!check(a.get_h264_vui(0,channel_id,&output),"a.get_h264_vui active")) { output={};return raw_status; }
    return 0;
}
int EncoderOwner::open_jpeg(EncoderSource source,uint32_t id,unsigned width,unsigned height,
                            bool own_device,bool software_input,unsigned quality) {
    if(used || !a.start_once || id<4 || id>6 || !width || width>3840 || !height || height>2160 ||
       (source!=EncoderSource::Zoom && source!=EncoderSource::Wide) || !quality || quality>100)return -1;
    used=true;jpeg=true;device_id=8;channel_id=id;source_channel=uint32_t(source);source_port=0;software=software_input;
#define CALL(expr) do { if(!check((expr),#expr)) { close();return raw_status; } } while(0)
    if(own_device) { EncoderDevice dev{3840,2160};CALL(a.create_device(device_id,&dev));device=true; }
    const auto j=jpeg_channel(width,height,quality);H26xChannel attr{};std::memcpy(&attr,&j,sizeof(attr));
    CALL(a.create_channel(device_id,channel_id,&attr));channel=true;
    const uint32_t normal=0;CALL(a.set_input_source(device_id,channel_id,&normal));
    if(!software) {
        Port src{Scl,0,source_channel,source_port},dst{Venc,device_id,channel_id,0};
        CALL(s.sys_bind(0,&src,&dst,30,30,FrameBind,0));bound=true;
    }
#undef CALL
    return 0;
}
int EncoderOwner::start_once() {
    if(!jpeg || !channel || unhealthy || started)return -1;
    const int32_t count=1;started=true;
    return check(a.start_once(device_id,channel_id,&count),"JPEG start once")?0:-1;
}
int EncoderOwner::stop_once() {
    if(!started)return 0;
    const int result=a.stop(device_id,channel_id);
    if(!check(result,"JPEG stop once"))return -1;
    started=false;return 0;
}
int EncoderOwner::pending_packs(uint32_t &packs) {
    packs=0;if(!ready())return -1;
    EncoderStatus status{};
    if(!check(a.query(device_id,channel_id,&status),"a.query pending"))return -1;
    packs=status.current_packs;return 0;
}
int EncoderOwner::copy_frame(EncodedFrame &output) {
    output.bytes=0;output.packets=0;output.sequence=0;
    std::memset(output.pts,0,sizeof(output.pts));
    if(!ready() || !output.data || !output.capacity)return -1;
    EncoderStatus status{};
    if(!check(a.query(device_id,channel_id,&status),"a.query"))return -1;
    if(!status.current_packs)return 0;
    if(status.current_packs>32) { check(-1,"packet capacity exceeded");return -1; }
    EncoderStream stream{};
    if(!m.pointer_slot(packets,&stream.packets_address) || !stream.packets_address) {
        check(-1,"ARM32 packet pointer unavailable");return -1;
    }
    const uint32_t address=stream.packets_address;
    stream.count=status.current_packs;
    std::memset(packets,0,sizeof(packets));
    // Query showed output available; never block beyond this finite SDK bound.
    if(!check(a.get_stream(device_id,channel_id,&stream,100),"a.get_stream"))return -1;
    // ReleaseStream iterates the caller's array. A corrupt returned pointer or
    // capacity cannot safely be passed back. Poison and let process teardown
    // reclaim mappings instead of dereferencing untrusted output.
    if(stream.packets_address!=address || !stream.count || stream.count>status.current_packs) {
        check(-1,"invalid returned stream array");return -1;
    }
    // This SDK stops at end-of-frame and DOES NOT update stream.count.
    // Query may describe several queued frames. Only the filled prefix owns
    // packets: releasing the untouched capacity would release extra queues.
    uint32_t filled=0;
    while(filled<stream.count && packets[filled].data_address)filled++;
    if(!filled || !stream.handle) { check(-1,"empty returned packet prefix");return -1; }
    for(uint32_t i=filled;i<stream.count;i++) {
        const EncoderPacket empty{};
        if(std::memcmp(&packets[i],&empty,sizeof(empty))) { check(-1,"noncontiguous returned packets");return -1; }
    }
    stream.count=filled; // only count is an input capacity, not SDK output
    const uint32_t sequence=stream.sequence;
    bool valid=jpeg || packets[filled-1].frame_flags!=0;
    if(!jpeg)for(uint32_t i=0;i+1<filled;i++)valid=valid && packets[i].frame_flags==0;
    size_t bytes=0;
    uint64_t pts[32]{};
    for(uint32_t i=0;valid && i<stream.count;i++) {
        const auto &p=packets[i];
        const size_t length=p.length>=p.offset?p.length-p.offset:0;
        if(!p.data_address || !length || p.length>(jpeg?8U:4U)*1024*1024 ||
           uint64_t(p.data_address)+p.length>UINT64_C(0x100000000) ||
           length>output.capacity-bytes ||
           !m.copy(p.data_address+p.offset,output.data+bytes,length)) { valid=false;break; }
        bytes+=length;pts[i]=p.pts;
    }
    if(jpeg)valid=valid && bytes>=4 && output.data[0]==0xff && output.data[1]==0xd8 && output.data[bytes-2]==0xff && output.data[bytes-1]==0xd9;
    // Preserve every SDK output field, including the mapped virtual handle;
    // count now names only packets actually acquired by GetStream.
    const int released=a.release_stream(device_id,channel_id,&stream);
    if(!check(released,"a.release_stream"))return -1;
    if(!valid) { check(-1,"invalid/unreadable packet payload");return -1; }
    output.bytes=bytes;output.packets=filled;output.sequence=sequence;
    std::memcpy(output.pts,pts,sizeof(pts));
    return 1;
}
int EncoderOwner::close() {
    int first=0;
    Port source{Scl,0,source_channel,source_port},destination{Venc,device_id,channel_id,0};
#define CLEAN(flag,expr) do { if(flag) { cleanup((expr),#expr,first);flag=false; } } while(0)
    CLEAN(bound,s.sys_unbind(0,&source,&destination));
    CLEAN(started,a.stop(device_id,channel_id));
    CLEAN(channel,a.destroy_channel(device_id,channel_id));
    CLEAN(device,a.destroy_device(device_id));
#undef CLEAN
    return first;
}
} // namespace ca_zt30
