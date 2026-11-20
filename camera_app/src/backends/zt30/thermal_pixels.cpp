#include "thermal_pixels.h"
#include <limits>
namespace ca_zt30 {
namespace {
struct Span { uintptr_t first,end; };
bool span(const void *p,size_t n,Span &s) {
    s.first=reinterpret_cast<uintptr_t>(p);
    if(n>std::numeric_limits<uintptr_t>::max()-s.first)return false;
    s.end=s.first+n;return true;
}
bool overlaps(const Span &a,const Span &b) { return a.first<b.end && b.first<a.end; }
bool footprint(size_t stride,size_t rows,size_t &bytes) {
    if(stride>(std::numeric_limits<size_t>::max()-ThermalPixelWidth)/(rows-1))return false;
    bytes=(rows-1)*stride+ThermalPixelWidth;return true;
}
}
ThermalPixelResult thermal_uyvy_nv12(const uint8_t *frame,size_t bytes,
    uint8_t *y,size_t y_capacity,size_t y_stride,
    uint8_t *uv,size_t uv_capacity,size_t uv_stride) {
    if(!frame || bytes!=ThermalPixelFrameBytes)return ThermalPixelResult::InvalidInput;
    if(!y || !uv)return ThermalPixelResult::InvalidOutput;
    if(y_stride<ThermalPixelWidth || uv_stride<ThermalPixelWidth)return ThermalPixelResult::InvalidStride;
    size_t y_bytes=0,uv_bytes=0;
    if(!footprint(y_stride,ThermalPixelHeight,y_bytes) ||
        !footprint(uv_stride,ThermalPixelHeight/2,uv_bytes))return ThermalPixelResult::AddressOverflow;
    if(y_capacity<y_bytes || uv_capacity<uv_bytes)return ThermalPixelResult::ShortOutput;
    Span input{},luma{},chroma{};
    if(!span(frame,bytes,input) || !span(y,y_bytes,luma) || !span(uv,uv_bytes,chroma))return ThermalPixelResult::AddressOverflow;
    if(overlaps(input,luma) || overlaps(input,chroma) || overlaps(luma,chroma))return ThermalPixelResult::Overlap;
    for(size_t row=0;row<ThermalPixelHeight;++row) {
        const uint8_t *p=frame+row*ThermalPixelWidth*2;
        for(size_t col=0;col<ThermalPixelWidth;++col)y[row*y_stride+col]=p[col*2+1];
    }
    for(size_t row=0;row<ThermalPixelHeight/2;++row) {
        const uint8_t *a=frame+row*ThermalPixelWidth*4,*b=a+ThermalPixelWidth*2;
        for(size_t col=0;col<ThermalPixelWidth;col+=2) {
            uv[row*uv_stride+col]=uint8_t((unsigned(a[col*2])+b[col*2]+1)>>1);
            uv[row*uv_stride+col+1]=uint8_t((unsigned(a[col*2+2])+b[col*2+2]+1)>>1);
        }
    }
    return ThermalPixelResult::Ok;
}
ThermalPixelResult thermal_be16(const uint8_t *frame,size_t bytes,
    uint16_t *values,size_t capacity_elements) {
    if(!frame || bytes!=ThermalPixelFrameBytes)return ThermalPixelResult::InvalidInput;
    if(!values)return ThermalPixelResult::InvalidOutput;
    if(reinterpret_cast<uintptr_t>(values)%alignof(uint16_t))return ThermalPixelResult::UnalignedOutput;
    if(capacity_elements<ThermalPixelCount)return ThermalPixelResult::ShortOutput;
    Span input{},output{};
    if(!span(frame,bytes,input) || !span(values,ThermalPixelCount*2,output))return ThermalPixelResult::AddressOverflow;
    if(overlaps(input,output))return ThermalPixelResult::Overlap;
    const uint8_t *p=frame+ThermalPixelCount*2;
    for(size_t i=0;i<ThermalPixelCount;++i)values[i]=uint16_t((unsigned(p[i*2])<<8)|p[i*2+1]);
    return ThermalPixelResult::Ok;
}
} // namespace ca_zt30
