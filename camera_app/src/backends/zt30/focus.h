#ifndef CAMERA_APP_ZT30_FOCUS_H
#define CAMERA_APP_ZT30_FOCUS_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Vendor CUS3A callback block at51dec: five16-element arrays of five-byte
 * values, then16 three-byte values. These reads require at least448 available
 * bytes. They do not prove the callback input object size or layout of the
 *7168-byte MI_ISP_CUS3A_GetAFStats result (7168=16*448). Call only with the
 * callback stats-data pointer, after native pointer/lifetime qualification. */
static constexpr size_t CA_ZT30_AF_CALLBACK_BYTES=448;
static constexpr size_t CA_ZT30_AF_PAYLOAD_BYTES=27;
static inline bool ca_zt30_pack_focus_callback(const uint8_t *block,size_t size,
                                               unsigned region,uint8_t *payload,
                                               size_t capacity)
{
    if(!block || !payload || size<CA_ZT30_AF_CALLBACK_BYTES || region>=16 ||
       capacity<CA_ZT30_AF_PAYLOAD_BYTES)return false;
    // Vendor4b624 emits only the low32 bits of the third five-byte value.
    const size_t offsets[]={0,80,160,240,320,400};
    const size_t widths[]={5,5,4,5,5,3};
    uint8_t packed[CA_ZT30_AF_PAYLOAD_BYTES];
    size_t used=0;
    for(unsigned field=0;field<6;field++) {
        size_t stride=field==5?3:5;
        memcpy(packed+used,block+offsets[field]+region*stride,widths[field]);
        used+=widths[field];
    }
    memcpy(payload,packed,sizeof(packed));
    return true;
}

/* MI_ISP_CUS3A_GetAFStats returns 0x1c00 bytes; only the first 448-byte block
 * is filled. Its 16 regions are the default 4x4 grid of windows over the
 * frame, row by row. Sum the four centre windows into one payload: the corner
 * windows often see sky or a blank wall and focus the lens on nothing. */
static inline bool ca_zt30_pack_focus_centre(const uint8_t *block,size_t size,
                                             uint8_t *payload,size_t capacity)
{
    if(!block || !payload || size<CA_ZT30_AF_CALLBACK_BYTES ||
       capacity<CA_ZT30_AF_PAYLOAD_BYTES)return false;
    const size_t offsets[]={0,80,160,240,320,400};
    const size_t widths[]={5,5,4,5,5,3};
    const unsigned centre[]={5,6,9,10};
    size_t used=0;
    for(unsigned field=0;field<6;field++) {
        const size_t stride=field==5?3:5;
        uint64_t sum=0;
        for(unsigned region:centre) {
            // as the vendor does: only the low 32 bits of the third value
            uint64_t value=0;
            for(size_t byte=0;byte<widths[field];byte++)
                value|=uint64_t(block[offsets[field]+region*stride+byte])<<(8*byte);
            sum+=value;
        }
        // saturate: a wrapped sum would look like lost contrast
        const uint64_t limit=(uint64_t(1)<<(8*widths[field]))-1;
        if(sum>limit)sum=limit;
        for(size_t byte=0;byte<widths[field];byte++)payload[used++]=uint8_t(sum>>(8*byte));
    }
    return true;
}

#ifdef __cplusplus
#include <mutex>
/* The lens MCU runs autofocus from these statistics; without them the lens
 * hunts continuously. The video thread offers, the MCU link owner takes. */
static constexpr size_t CA_ZT30_AF_STATS_BYTES=16*CA_ZT30_AF_CALLBACK_BYTES;
struct ca_zt30_focus_latest {
    std::mutex lock;
    uint8_t payload[CA_ZT30_AF_PAYLOAD_BYTES];
    bool fresh=false;
};
inline ca_zt30_focus_latest &ca_zt30_focus() { static ca_zt30_focus_latest value;return value; }
inline bool ca_zt30_offer_focus(const uint8_t *stats,size_t size)
{
    auto &latest=ca_zt30_focus();
    std::lock_guard<std::mutex> guard(latest.lock);
    if(!ca_zt30_pack_focus_centre(stats,size,latest.payload,sizeof(latest.payload)))return false;
    latest.fresh=true;
    return true;
}
inline bool ca_zt30_take_focus(uint8_t payload[CA_ZT30_AF_PAYLOAD_BYTES])
{
    auto &latest=ca_zt30_focus();
    std::lock_guard<std::mutex> guard(latest.lock);
    if(!latest.fresh)return false;
    memcpy(payload,latest.payload,sizeof(latest.payload));
    latest.fresh=false;
    return true;
}
#endif
#endif
