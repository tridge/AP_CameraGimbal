#include "input_buffer.h"
#include <cstring>
#include <limits>
namespace ca_zt30 {
namespace {
uint32_t word(const InputBufferInfo &info,size_t offset) {
    uint32_t value;std::memcpy(&value,reinterpret_cast<const uint8_t*>(&info)+offset,4);return value;
}
uint16_t half(const InputBufferInfo &info,size_t offset) {
    uint16_t value;std::memcpy(&value,reinterpret_cast<const uint8_t*>(&info)+offset,2);return value;
}
bool span(uintptr_t address,size_t bytes,uintptr_t &end) {
    if(!address || !bytes || bytes>std::numeric_limits<uintptr_t>::max()-address)return false;
    end=address+bytes;return true;
}
bool overlap(uintptr_t a,uintptr_t end,uintptr_t b,size_t bytes) {
    uintptr_t other=0;return !span(b,bytes,other) || (a<other && b<end);
}
bool buffer_shape(const InputBufferInfo &info,size_t &allocation,size_t &uv_offset) {
    // Captured mi_sys.ko fill_bufinfo corroborates these raw offsets. Names
    // and NV12/NV21 ordering remain candidates; reject all other numeric layouts.
    if(word(info,16)!=1 || word(info,32)!=0 || word(info,36)!=11 ||
       word(info,40)!=0 || word(info,44)!=0 || word(info,48)!=0 || word(info,52)!=2 ||
       half(info,56)!=640 || half(info,58)!=512 || word(info,68)!=0 || word(info,104)!=0)return false;
    const uint32_t y=info.plane_address[0],uv=info.plane_address[1];
    const size_t ys=info.stride[0],us=info.stride[1];
    allocation=word(info,108);
    if(ys<640 || us<640 || ys>4096 || us>4096 || allocation<ThermalNv12Bytes || allocation>16*1024*1024)return false;
    if(!y || !uv || y%4 || uv%4 || allocation>UINT32_MAX-y || uv<y)return false;
    uv_offset=size_t(uv-y);
    const size_t y_bytes=511*ys+640,uv_bytes=255*us+640;
    // One SDK allocation must contain both disjoint two-plane footprints.
    // Padding is untouched, including the final row's trailing padding.
    return y_bytes<=allocation && uv_offset>=y_bytes && uv_offset<=allocation && uv_bytes<=allocation-uv_offset;
}
}
InputBufferResult InputBufferOwner::submit(const uint8_t *source,size_t bytes,uint64_t opaque_config8,int32_t timeout) {
    using R=InputBufferResult;
    if(busy)return R::Reentrant;
    if(poisoned)return R::Unhealthy;
    uintptr_t source_end=0;
    if(bytes!=ThermalNv12Bytes || !span(reinterpret_cast<uintptr_t>(source),bytes,source_end))return R::InvalidSource;
    if(timeout<0 || timeout>1000)return R::InvalidTimeout;
    if(!a.get || !a.put || !m.map || !m.copy_rows)return R::MissingApi;
    busy=true;
    struct BusyGuard { bool &value;~BusyGuard() { value=false; } } guard{busy};
    get_status=0;put_status=0;
    InputBufferConfig config{};
    config.type=1;config.opaque8=opaque_config8;
    config.width=640;config.height=512;config.numeric_format=11;
    InputBufferInfo info{};InputBufferHandle handle=0;
    InputPort port=p; // Get must not mutate the next frame's saved port.
    get_status=a.get(&port,&config,&info,&handle,timeout);
    if(get_status) { poisoned=true;return R::GetFailed; } // outputs may be garbage
    // Captured kernel allocates input handles in [64,8191]. A success with
    // anything else has no safely identified lease; leave cleanup to teardown.
    if(handle<64 || handle>8191) { poisoned=true;return R::InvalidHandle; }
    R result=R::InvalidBuffer;
    size_t allocation=0,uv_offset=0;
    uint8_t *mapped=nullptr;uintptr_t destination_end=0;
    if(buffer_shape(info,allocation,uv_offset)) {
        result=R::MapFailed;
        if(m.map(info.plane_address[0],allocation,&mapped) &&
           span(reinterpret_cast<uintptr_t>(mapped),allocation,destination_end)) {
            const uintptr_t begin=reinterpret_cast<uintptr_t>(mapped);
            // Host fakes may translate integer slots to host addresses; check
            // resolved aliases as well as numeric SDK plane disjointness.
            if(!overlap(begin,destination_end,reinterpret_cast<uintptr_t>(source),bytes) &&
               !overlap(begin,destination_end,reinterpret_cast<uintptr_t>(this),sizeof(*this)) &&
               !overlap(begin,destination_end,reinterpret_cast<uintptr_t>(&info),sizeof(info)) &&
               !overlap(begin,destination_end,reinterpret_cast<uintptr_t>(&handle),sizeof(handle)) &&
               !overlap(begin,destination_end,reinterpret_cast<uintptr_t>(&config),sizeof(config)) &&
               !overlap(begin,destination_end,reinterpret_cast<uintptr_t>(&port),sizeof(port))) {
                result=R::CopyFailed;
                if(m.copy_rows(mapped,info.stride[0],source,640,512) &&
                   m.copy_rows(mapped+uv_offset,info.stride[1],source+640*512,640,256))result=R::Submitted;
            }
        }
    }
    // Discard skips Info metadata interpretation in the captured kernel. Keep
    // all264 returned bytes (including unknown byte20 and tail) untouched.
    put_status=a.put(handle,&info,result==R::Submitted?uint8_t(0):uint8_t(1));
    if(put_status)result=R::PutFailed;
    if(result!=R::Submitted)poisoned=true;
    return result;
}
} // namespace ca_zt30
