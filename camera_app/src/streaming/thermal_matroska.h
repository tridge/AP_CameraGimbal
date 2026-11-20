#pragma once
#include <stdint.h>
#include <string.h>
#include <vector>
#include <utility>

// Small forward-only Matroska muxer: one intra frame/BlockGroup/Cluster.
// Frame JSON is BlockAdditional ID 2, experimental FourCC mapping APCG.
namespace thermal_mkv {
using Bytes = std::vector<uint8_t>;
inline void append(Bytes &a, const Bytes &b) { a.insert(a.end(), b.begin(), b.end()); }
inline Bytes number(uint64_t n) {
    unsigned len = 1; while (len < 8 && (n >> (8*len))) len++;
    Bytes b; for (unsigned i = len; i; i--) b.push_back(n >> ((i-1)*8)); return b;
}
inline Bytes element(uint32_t id, const Bytes &data) {
    Bytes b = number(id);
    unsigned len = 1; while (len < 8 && data.size() >= ((UINT64_C(1) << (7*len))-1)) len++;
    uint64_t size = data.size() | (UINT64_C(1) << (7*len));
    for (unsigned i = len; i; i--) b.push_back(size >> ((i-1)*8));
    append(b, data); return b;
}
inline Bytes uint_element(uint32_t id, uint64_t n) { return element(id, number(n)); }
inline Bytes text(uint32_t id, const char *s) { return element(id, Bytes(s, s+strlen(s))); }
inline Bytes header(const uint8_t *extra, size_t size, unsigned fps) {
    Bytes ebml;
    for (auto p : {std::pair<uint32_t, uint64_t>{0x4286,1}, {0x42F7,1}, {0x42F2,4}, {0x42F3,8}})
        append(ebml, uint_element(p.first,p.second));
    append(ebml,text(0x4282,"matroska")); append(ebml,uint_element(0x4287,4)); append(ebml,uint_element(0x4285,2));
    Bytes out = element(0x1A45DFA3,ebml);
    append(out,Bytes{0x18,0x53,0x80,0x67,0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff});
    Bytes info = uint_element(0x2AD7B1,1000000);
    append(info,text(0x4D80,"AP_CameraGimbal")); append(info,text(0x5741,"APCG thermal v1"));
    append(out,element(0x1549A966,info));
    Bytes track;
    for (auto p : {std::pair<uint32_t,uint64_t>{0xD7,1},{0x73C5,1},{0x83,1},{0x9C,0},{0x55EE,2}})
        append(track,uint_element(p.first,p.second));
    // Live rate changes use variable frame timing, without a fixed default duration.
    if (fps) append(track,uint_element(0x23E383,1000000000U/fps));
    append(track,text(0x86,"V_FFV1")); append(track,element(0x63A2,Bytes(extra,extra+size)));
    Bytes video = uint_element(0xB0,640); append(video,uint_element(0xBA,512));
    Bytes colour = uint_element(0x55B2,16); append(video,element(0x55B0,colour));
    append(track,element(0xE0,video));
    Bytes mapping = uint_element(0x41F0,2);
    append(mapping,text(0x41A4,"apcg.thermal.v1 JSON")); append(mapping,uint_element(0x41E7,0x41504347));
    append(track,element(0x41E4,mapping));
    append(out,element(0x1654AE6B,element(0xAE,track)));
    return out;
}
inline Bytes frame(const Bytes &encoded, const char *metadata, uint64_t ms) {
    Bytes block{0x81,0,0,0}; append(block,encoded);
    Bytes group = element(0xA1,block);
    Bytes more = uint_element(0xEE,2); append(more,text(0xA5,metadata));
    append(group,element(0x75A1,element(0xA6,more)));
    Bytes cluster = uint_element(0xE7,ms); append(cluster,element(0xA0,group));
    return element(0x1F43B675,cluster);
}
}
