#pragma once
#include "camera_app/image_tracker.h"
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <ctime>

// MI_SYS_FrameData_t / MI_SYS_BufInfo_t prefix from SigmaStar's MI SYS
// documentation (Tiramisu XLS00V024 and Infinity6 SSD20X). Only linear NV12
// is accepted. Keep the unused union tail opaque: the packaged GetBuf wrappers
// copy 264 bytes on A8 and 272 on ZR10. Both take (port, info, uint32_handle*)
// WITHOUT SocId, even though Mercury6 SetChnOutputPortDepth takes SocId.
// Checked against libmi_sys.so in the 20260913 stock filesystem captures.
struct ca_mi_frame_prefix {
    uint32_t tile, format, compression, scan, field, layout;
    uint16_t width,height;
    void *virtual_address[3];
    uint64_t physical_address[3];
    uint32_t stride[3], size;
};
struct ca_mi_buffer {
    uint64_t pts, sideband;
    uint32_t type, flags_or_sequence, sequence_or_flags;
    union { ca_mi_frame_prefix frame; uint64_t opaque[30]; };
};
#if defined(__arm__)
static_assert(offsetof(ca_mi_buffer,frame)==32 && sizeof(ca_mi_buffer)==272,"MI buffer ABI");
static_assert(offsetof(ca_mi_frame_prefix,virtual_address)==28 &&
    offsetof(ca_mi_frame_prefix,stride)==64,"MI frame ABI");
#endif
class CA_MI_TrackingFrames {
public:
    bool open(unsigned module,unsigned device,bool mercury)
    {
        _ready=false;
        _port[0]=module; _port[1]=device; _port[2]=0; _port[3]=3;
        _get=reinterpret_cast<decltype(_get)>(dlsym(RTLD_DEFAULT,"MI_SYS_ChnOutputPortGetBuf"));
        _put=reinterpret_cast<decltype(_put)>(dlsym(RTLD_DEFAULT,"MI_SYS_ChnOutputPortPutBuf"));
        void *depth=dlsym(RTLD_DEFAULT,"MI_SYS_SetChnOutputPortDepth");
        if(!_get || !_put || !depth) return false;
        const int result=mercury?
            reinterpret_cast<int(*)(uint16_t,void*,unsigned,unsigned)>(depth)(0,_port,1,3):
            reinterpret_cast<int(*)(void*,unsigned,unsigned)>(depth)(_port,1,3);
        _ready=result==0; return _ready;
    }
    bool available() const { return _ready; }
    bool read(ca_tracking_frame &out)
    {
        if(!_ready) return false;
        ca_mi_buffer b {}; uint32_t handle=0;
        if(_get(_port,&b,&handle)) return false;
        const auto &f=b.frame;
        const bool valid=b.type==1 && f.layout==2 && f.format==11 && f.compression==0 &&
            f.width==320 && f.height==180 && f.stride[0]>=f.width && f.stride[0]<=4096 && f.virtual_address[0];
        if(valid) {
            out.width=f.width; out.height=f.height;
            const auto *src=static_cast<const uint8_t *>(f.virtual_address[0]);
            for(unsigned y=0;y<out.height;y++) std::memcpy(out.pixels+y*out.width,src+y*f.stride[0],out.width);
            timespec t {}; clock_gettime(CLOCK_MONOTONIC,&t);
            out.timestamp_ms=uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;
        }
        (void)_put(handle);
        return valid;
    }
private:
    uint32_t _port[4] {};
    bool _ready=false;
    int (*_get)(void *,ca_mi_buffer *,uint32_t *)=nullptr;
    int (*_put)(uint32_t)=nullptr;
};
