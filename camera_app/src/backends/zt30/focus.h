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
#endif
