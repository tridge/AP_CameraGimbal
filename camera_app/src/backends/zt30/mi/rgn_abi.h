#pragma once
#include "abi.h"
namespace ca_zt30 {
// Pinned 20220422 wrappers: Create copies16, bitmap16, display40, port16.
// RGN has the SYS chip prefix; its ARGB1555 format is0.
struct RgnConfig { uint32_t type,format,width,height; };
struct RgnBitmap { uint32_t format,width,height,address; };
struct RgnDisplay {
    uint32_t show,x,y,layer,constant_alpha;
    uint8_t background_alpha,foreground_alpha,padding[2];
    uint32_t invert,below_threshold,luminance;
    uint16_t cell_width,cell_height;
};
static_assert(sizeof(RgnConfig)==16 && sizeof(RgnBitmap)==16 && sizeof(RgnDisplay)==40,"ZT30 RGN copies");
static_assert(offsetof(RgnDisplay,background_alpha)==20 && offsetof(RgnDisplay,invert)==24,"ZT30 RGN alpha/invert");
struct RgnApi {
    int (*init)(uint16_t,void*);
    int (*deinit)(uint16_t);
    int (*create)(uint16_t,uint32_t,RgnConfig*);
    int (*destroy)(uint16_t,uint32_t);
    int (*bitmap)(uint16_t,uint32_t,RgnBitmap*);
    int (*attach)(uint16_t,uint32_t,Port*,RgnDisplay*);
    int (*detach)(uint16_t,uint32_t,Port*);
    int (*display)(uint16_t,uint32_t,Port*,RgnDisplay*);
};
}
