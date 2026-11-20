#ifdef NDEBUG
#error Private test assertions must remain enabled
#endif
#include "../camera_app/src/backends/zt30/thermal_pixels.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>
#include <vector>
using namespace ca_zt30;
using R=ThermalPixelResult;
int main() {
    const size_t W=ThermalPixelWidth,H=ThermalPixelHeight,N=ThermalPixelCount,B=ThermalPixelFrameBytes;
    std::vector<uint8_t> source(B+N*2),y(N+H*17+64,0xcc),uv(N/2+(H/2)*19+64,0xdd);
    std::vector<uint16_t> raw(N+8,0xabcd);
    uint8_t *p=source.data();
    for(size_t r=0;r<H;++r)for(size_t c=0;c<W;c+=2) {
        size_t k=(r*W+c)*2;
        p[k]=uint8_t(r+c);p[k+1]=uint8_t(r*3+c);
        p[k+2]=uint8_t(r+c+37);p[k+3]=uint8_t(r*3+c+1);
    }
    for(size_t i=0;i<N;++i) { uint16_t v=uint16_t(i*31+513);p[N*2+i*2]=uint8_t(v>>8);p[N*2+i*2+1]=uint8_t(v); }
    unsigned cases=0;
    assert(thermal_uyvy_nv12(p,B,y.data(),N,W,uv.data(),N/2,W)==R::Ok);
    for(size_t r=0;r<H;++r)for(size_t c=0;c<W;++c)assert(y[r*W+c]==uint8_t(r*3+c));
    for(size_t r=0;r<H/2;++r)for(size_t c=0;c<W;c+=2) {
        assert(uv[r*W+c]==uint8_t((unsigned(uint8_t(r*2+c))+uint8_t(r*2+c+1)+1)/2));
        assert(uv[r*W+c+1]==uint8_t((unsigned(uint8_t(r*2+c+37))+uint8_t(r*2+c+38)+1)/2));
    }
    ++cases; // Tight NV12 and odd-sum rounded chroma.
    std::fill(y.begin(),y.end(),0xcc);std::fill(uv.begin(),uv.end(),0xdd);
    const size_t ys=W+17,us=W+19,yneed=(H-1)*ys+W,uneed=(H/2-1)*us+W;
    assert(thermal_uyvy_nv12(p,B,y.data(),yneed,ys,uv.data(),uneed,us)==R::Ok);
    for(size_t r=0;r<H;++r)for(size_t c=0;c<ys;++c)assert(y[r*ys+c]==(c<W?uint8_t(r*3+c):0xcc));
    for(size_t i=H*ys;i<y.size();++i)assert(y[i]==0xcc);
    for(size_t r=0;r<H/2;++r)for(size_t c=W;c<us;++c)assert(uv[r*us+c]==0xdd);
    for(size_t i=(H/2)*us;i<uv.size();++i)assert(uv[i]==0xdd);
    ++cases;
    assert(thermal_be16(p,B,raw.data(),N)==R::Ok);
    for(size_t i=0;i<N;++i)assert(raw[i]==uint16_t(i*31+513));
    for(size_t i=N;i<raw.size();++i)assert(raw[i]==0xabcd);
    ++cases;
    assert(thermal_uyvy_nv12(p,B,y.data(),N-1,W,uv.data(),N/2,W)==R::ShortOutput);
    assert(thermal_uyvy_nv12(p,B,y.data(),N,W,uv.data(),N/2-1,W)==R::ShortOutput);
    assert(thermal_be16(p,B,raw.data(),N-1)==R::ShortOutput);++cases;
    assert(thermal_uyvy_nv12(nullptr,B,y.data(),N,W,uv.data(),N/2,W)==R::InvalidInput);
    assert(thermal_uyvy_nv12(p,B,nullptr,N,W,uv.data(),N/2,W)==R::InvalidOutput);
    assert(thermal_uyvy_nv12(p,B,y.data(),N,W,nullptr,N/2,W)==R::InvalidOutput);
    assert(thermal_be16(nullptr,B,raw.data(),N)==R::InvalidInput);
    assert(thermal_be16(p,B,nullptr,N)==R::InvalidOutput);++cases;
    for(size_t bytes:{size_t(0),B-1,B+1}) {
        assert(thermal_uyvy_nv12(p,bytes,y.data(),N,W,uv.data(),N/2,W)==R::InvalidInput);
        assert(thermal_be16(p,bytes,raw.data(),N)==R::InvalidInput);
    }++cases;
    assert(thermal_uyvy_nv12(p,B,y.data(),N,W-1,uv.data(),N/2,W)==R::InvalidStride);
    assert(thermal_uyvy_nv12(p,B,y.data(),N,W,uv.data(),N/2,W-1)==R::InvalidStride);
    const size_t huge=std::numeric_limits<size_t>::max();
    assert(thermal_uyvy_nv12(p,B,y.data(),huge,huge,uv.data(),huge,W)==R::AddressOverflow);
    assert(thermal_uyvy_nv12(p,B,y.data(),huge,W,uv.data(),huge,huge)==R::AddressOverflow);++cases;
    assert(thermal_uyvy_nv12(p,B,p,N,W,uv.data(),N/2,W)==R::Overlap);
    assert(thermal_uyvy_nv12(p,B,y.data(),N,W,p+N*2,N/2,W)==R::Overlap);
    assert(thermal_uyvy_nv12(p,B,y.data(),N,W,y.data()+1,N/2,W)==R::Overlap);
    assert(thermal_be16(p,B,reinterpret_cast<uint16_t*>(p),N)==R::Overlap);
    // Adjacent output footprints and adjacent input/output are legal.
    assert(thermal_uyvy_nv12(p,B,p+B,N,W,p+B+N,N/2,W)==R::Ok);++cases;
    auto *far=reinterpret_cast<uint8_t*>(std::numeric_limits<uintptr_t>::max()-15);
    assert(thermal_uyvy_nv12(far,B,y.data(),N,W,uv.data(),N/2,W)==R::AddressOverflow);
    assert(thermal_uyvy_nv12(p,B,far,N,W,uv.data(),N/2,W)==R::AddressOverflow);
    assert(thermal_be16(far,B,raw.data(),N)==R::AddressOverflow);
    assert(thermal_be16(p,B,reinterpret_cast<uint16_t*>(far),N)==R::AddressOverflow);
    assert(thermal_be16(p,B,reinterpret_cast<uint16_t*>(y.data()+1),N)==R::UnalignedOutput);++cases;
    const auto yc=y,uc=uv;const auto rc=raw;
    assert(thermal_uyvy_nv12(p,B,y.data(),N-1,W,uv.data(),N/2,W)==R::ShortOutput);
    assert(thermal_be16(p,B,raw.data(),N-1)==R::ShortOutput);
    assert(y==yc && uv==uc && raw==rc);++cases;
    assert(cases==10);
    std::printf("ZT30 thermal pixels private PASS cases=%u no-peripheral-access\n",cases);
}
