/* Fixed ZT30 flat parameter registry. Source proof is stored in the port
 * analysis payload-stub-audit-20261009/registry.json. Device IDs must match
 * the qualified observer's rdev table; ioctl numbers alone are ambiguous. */
#pragma once
#include <stdint.h>
struct zt30_ioctl_layout { uint32_t command;uint16_t length,pre_length;uint8_t device,post;const char *name; };
static const struct zt30_ioctl_layout zt30_ioctl_layouts[]={
    {0x40306902U,48,40,0,0,"MI_SYS_BindChnPort"},
    {0x40306903U,48,48,0,0,"MI_SYS_BindChnPort2"},
    {0x40086908U,8,8,0,0,"MI_SYS_InitPtsBase"},
    {0x40086909U,8,8,0,0,"MI_SYS_SyncPts"},
    {0x40186915U,24,24,0,0,"MI_SYS_SetChnOutputPortDepth"},
    {0xc0386902U,56,4,1,1,"MI_SNR_GetPadInfo"},
    {0xc0506903U,80,8,1,1,"MI_SNR_GetPlaneInfo"},
    {0x40086905U,8,8,1,0,"MI_SNR_SetFps"},
    {0xc0086908U,8,4,1,1,"MI_SNR_QueryResCount"},
    {0xc03c6909U,60,8,1,1,"MI_SNR_GetRes"},
    {0x4008690bU,8,8,1,0,"MI_SNR_SetRes"},
    {0x4008690cU,8,8,1,0,"MI_SNR_SetOrien"},
    {0x4008690eU,8,8,1,0,"MI_SNR_SetPlaneMode"},
    {0x40206900U,32,32,2,0,"MI_VIF_CreateDevGroup"},
    {0x40186904U,24,24,2,0,"MI_VIF_SetDevAttr"},
    {0x401c6909U,28,28,2,0,"MI_VIF_SetOutputPortAttr"},
    {0x40586902U,88,88,3,0,"MI_ISP_CreateChannel"},
    {0x40186906U,24,24,3,0,"MI_ISP_SetChnParam"},
    {0x4018690aU,24,24,3,0,"MI_ISP_SetOutputPortParam"},
    {0x400c6902U,12,12,4,0,"MI_SCL_CreateChannel"},
    {0x400c6906U,12,12,4,0,"MI_SCL_SetChnParam"},
    {0x4024690aU,36,36,4,0,"MI_SCL_SetOutputPortParam"},
    {0x40546902U,84,84,5,0,"MI_VENC_CreateChn"},
};
static inline const struct zt30_ioctl_layout *zt30_ioctl_layout_find(unsigned device,uint32_t command) {
    for(unsigned i=0;i<sizeof(zt30_ioctl_layouts)/sizeof(*zt30_ioctl_layouts);i++)
        if(zt30_ioctl_layouts[i].device==device && zt30_ioctl_layouts[i].command==command)return &zt30_ioctl_layouts[i];
    return 0;
}

/* Captured live node identities; caller must also verify fd against the
 * corresponding named node, as the metadata observer does. This RAM tool
 * deliberately stays metadata-only if this pinned mapping changes. */
struct zt30_ioctl_device_id {unsigned major,minor;};
static const struct zt30_ioctl_device_id zt30_ioctl_payload_ids[]={
#ifdef ZT30_PAYLOAD_FIXTURE_DEVICES
    /* Explicit no-SDK fixture: named /dev/null and /dev/zero, not MI nodes. */
    {1,3},{1,5},
#else
    {230,63},{230,62},
#endif
    {230,46},{230,55},{230,58},{230,45}
};
static inline int zt30_ioctl_payload_device_matches(unsigned device,unsigned major,unsigned minor) {
    return device<6 && major==zt30_ioctl_payload_ids[device].major && minor==zt30_ioctl_payload_ids[device].minor;
}
