#pragma once
// ArduPilot AP_Terrain v1 disk format (TerrainIO.cpp / TerrainUtil.cpp).
// Decode explicitly: files are little endian, with 2048-byte disk records.
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <initializer_list>
namespace ap_terrain {
inline uint16_t u16(const uint8_t *p) { return p[0] | uint16_t(p[1])<<8; }
inline uint32_t u32(const uint8_t *p) { return u16(p) | uint32_t(u16(p+2))<<16; }
inline uint64_t u64(const uint8_t *p) { return u32(p) | uint64_t(u32(p+4))<<32; }
inline uint16_t crc(const uint8_t *b) {
    uint16_t c=0;
    // Minor version (byte 1821) and padding are excluded, as in AP_Terrain.
    for(unsigned i=0;i<1821;i++) {
        c ^= uint16_t(i==16 || i==17 ? 0 : b[i])<<8;
        for(unsigned j=0;j<8;j++) c=(c<<1)^((c&0x8000)?0x1021:0);
    }
    return c;
}
inline bool valid(const uint8_t *b) {
    return u64(b) && !(u64(b)>>56) && u16(b+18)==1 &&
           u16(b+20)>=1 && u16(b+20)<=1000 && u16(b+16)==crc(b);
}
inline bool empty(const uint8_t *b) {
    for(unsigned i=0;i<2048;i++) if(b[i]) return false;
    return true;
}
inline int degree(int32_t v) { return int(floor(v*1.0e-7)); }
inline float scale(int32_t lat) { return fmaxf(cosf(lat*1.0e-7f*float(M_PI/180)),0.01f); }
constexpr float metres=0.011131884502145034f; // AP Location scaling, metres / 1e-7 degree
inline unsigned stride(int lat, unsigned spacing) {
    // AP_Terrain::east_blocks: add two full blocks at the east edge.
    int32_t lon_delta=10000000+int32_t((2*spacing*32/metres)/scale(lat*10000000));
    return float(lon_delta)*metres*scale(lat*10000000)/(spacing*28);
}
inline bool name(const char *n, int &lat, int &lon) {
    if(strlen(n)!=11 || (n[0]!='N' && n[0]!='S') || (n[3]!='E' && n[3]!='W') || strcmp(n+7,".DAT")) return false;
    for(unsigned i: {1U,2U,4U,5U,6U}) if(n[i]<'0'||n[i]>'9') return false;
    lat=(n[1]-'0')*10+n[2]-'0'; lon=(n[4]-'0')*100+(n[5]-'0')*10+n[6]-'0';
    if(n[0]=='S') lat=-lat;
    if(n[3]=='W') lon=-lon;
    return lat>=-90 && lat<90 && lon>=-180 && lon<180;
}
inline bool located(const uint8_t *b,int lat,int lon,uint64_t index) {
    unsigned spacing=u16(b+20), x=u16(b+1814),y=u16(b+1816);
    if(!spacing || x*24U*spacing>111319U || y>=stride(lat,spacing)) return false;
    int32_t glat=lat*10000000+int32_t(x*24*spacing/metres);
    int64_t glon=int64_t(lon)*10000000+int64_t((y*28*spacing/metres)/scale((lat*10000000+glat)/2));
    if(glon>=1800000000LL) glon-=3600000000LL;
    return int8_t(b[1820])==lat && int16_t(u16(b+1818))==lon &&
        uint64_t(stride(lat,spacing))*x+y==index &&
        llabs(int64_t(int32_t(u32(b+8)))-glat)<=2 && llabs(int64_t(int32_t(u32(b+12)))-glon)<=2;
}
}
