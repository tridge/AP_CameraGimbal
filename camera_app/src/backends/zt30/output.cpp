#include "output.h"
#include "media_graph.h"
#include "camera_app/log.h"
#include "camera_app/video_metadata.h"
#include <cerrno>
#include <ctime>
#include <unistd.h>
namespace ca_zt30 {
uint64_t VideoOutput::now() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
VideoOutput::~VideoOutput() { if(close())_exit(28); }
int VideoOutput::start(EncoderSource selected,unsigned scaler_port,unsigned channel,
                       EncoderCodec format,unsigned width,unsigned height,unsigned bitrate,Frame fn,void *argument) {
    if(used || !buffer || !fn || scaler_port<2 || scaler_port>4 || (channel!=3 && channel!=4 && channel!=7) ||
       width<16 || width>3840 || height<16 || height>2160 || (width&1) || (height&1) ||
       unsigned(selected)>1 || (format!=EncoderCodec::H264 && format!=EncoderCodec::H265) || !bitrate) { errno=EINVAL;return -1; }
    used=true;source=unsigned(selected);port=scaler_port;codec=format;callback=fn;opaque=argument;
    auto attr=scl_port(uint16_t(width),uint16_t(height));
    int rc=sdk.api.scl_set_port(0,source,port,&attr);
    if(rc)return rc;
    port_enabled=true; // Disable even a partially successful activation.
    if((rc=sdk.api.scl_enable_port(0,source,port)))return rc;
    Port output{Scl,0,source,port};
    if((rc=sdk.api.sys_depth(0,&output,0,4)))return rc;
    EncoderOptions options;options.width=width;options.height=height;options.bitrate=bitrate;
    options.channel=channel;options.source_port=port;options.own_device=false;
    if((rc=encoder.open(format,H264Timing::ReadOnly,selected,options)))return rc;
    rc=pthread_create(&thread,nullptr,[](void *p)->void* { static_cast<VideoOutput*>(p)->consume();return nullptr; },this);
    if(rc) { errno=rc;return -1; }
    thread_started=true;
    const uint64_t deadline=now()+3000000000ULL;
    while(!frames.load() && !failed.load() && now()<deadline)usleep(10000);
    if(!frames.load()) { errno=ETIMEDOUT;return -1; }
    return 0;
}
void VideoOutput::consume() {
    EncodedFrame frame;frame.data=buffer.get();frame.capacity=4U*1024*1024;
    uint64_t last=now();
    while(!stop.load()) {
        const int got=encoder.copy_frame(frame);
        if(got<0) { failed.store(true);break; }
        if(got) {
            PtsObservation sample;
            bool valid=observe_pts_clock(clock,[this](uint64_t *pts){return sdk.current_pts(0,pts);},now,sample);
            uint64_t mapped=0;
            valid=valid && frame.packets && clock.frame(frame.pts[0],now()/1000,mapped);
            for(unsigned i=1;i<frame.packets;i++)if(frame.pts[i]!=frame.pts[0])valid=false;
            size_t offset=0,nal=0,length=0;bool key=false;
            while(ca_annexb_next(frame.data,frame.bytes,&offset,&nal,&length))if(length) {
                unsigned type=codec==EncoderCodec::H264 ? frame.data[nal]&31 : (frame.data[nal]>>1)&63;
                if(codec==EncoderCodec::H264 ? type==5 : type>=16 && type<=21)key=true;
            }
            if(!valid || !callback(opaque,frame,mapped,key)) { failed.store(true);break; }
            frames.fetch_add(1);
            last=now();
        }
        if(now()-last>4000000000ULL) { failed.store(true);break; }
        if(!got)usleep(2000);
    }
    if(failed.load())ca_log("ZT30 auxiliary video failed port=%u call=%s raw=%d",port,
        encoder.failed_call?encoder.failed_call:"frame/clock/callback",encoder.raw_status);
}
int VideoOutput::close() {
    stop.store(true);
    if(thread_started) { const int rc=pthread_join(thread,nullptr);if(rc)return rc;thread_started=false; }
    const int closed=encoder.close();
    int port_error=0;
    if(port_enabled) { port_error=sdk.api.scl_disable_port(0,source,port);port_enabled=false; }
    return closed?closed:port_error;
}
}
