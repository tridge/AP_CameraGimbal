#pragma once
#include "apcam/compiler.h"
#include "mi/star/i6_sys.h"
#include "mi/star/i6_snr.h"
#include "mi/star/i6_vif.h"
#include "mi/star/i6_vpe.h"
#include "mi/star/i6_venc.h"
#include <stddef.h>

/* ZR10 SDK appends multi-device mapping to the upstream i6 VIF ABI.
 * MI_VIF_{Set,Get}DevAttr copy 52 bytes; stock uses map value 1. */
typedef struct { i6_vif_dev base; unsigned multiDevMap; } zr10_vif_dev;
#if defined(__arm__)
/* Sizes/offsets checked against the packaged ARM library wrappers. */
APC_STATIC_ASSERT(sizeof(i6_snr_pad)==52,"sensor pad ABI");
APC_STATIC_ASSERT(sizeof(i6_snr_plane)==72,"sensor plane ABI");
APC_STATIC_ASSERT(sizeof(i6_snr_res)==52,"sensor resolution ABI");
APC_STATIC_ASSERT(sizeof(zr10_vif_dev)==52 && offsetof(zr10_vif_dev,multiDevMap)==48,"ZR10 VIF device ABI");
APC_STATIC_ASSERT(sizeof(i6_vif_port)==32,"VIF port ABI");
APC_STATIC_ASSERT(sizeof(i6_vpe_chn)==108 && offsetof(i6_vpe_chn,chnPort)==104,"VPE channel ABI");
APC_STATIC_ASSERT(sizeof(i6_vpe_para)==28,"VPE parameters ABI");
APC_STATIC_ASSERT(sizeof(i6_vpe_port)==16,"VPE port ABI");
APC_STATIC_ASSERT(sizeof(i6_venc_chn)==76,"VENC channel ABI");
APC_STATIC_ASSERT(sizeof(i6_venc_stat)==40,"VENC status ABI");
APC_STATIC_ASSERT(sizeof(i6_venc_strm)==72,"VENC stream ABI");
APC_STATIC_ASSERT(sizeof(i6_venc_pack)==168 && offsetof(i6_venc_pack,offset)==32,"VENC pack ABI");
#endif

/* Deliberately load only the entry points actually exercised. */
#define MI_FUNCTIONS(X) \
 X(sys, MI_SYS_Init, (void)) \
 X(sys, MI_SYS_Exit, (void)) \
 X(sys, MI_SYS_GetVersion, (i6_sys_ver *)) \
 X(sys, MI_SYS_BindChnPort2, (i6_sys_bind *,i6_sys_bind *,unsigned,unsigned,i6_sys_link,unsigned)) \
 X(sys, MI_SYS_UnBindChnPort, (i6_sys_bind *,i6_sys_bind *)) \
 X(sensor, MI_SNR_SetPlaneMode, (unsigned,unsigned char)) \
 X(sensor, MI_SNR_QueryResCount, (unsigned,unsigned *)) \
 X(sensor, MI_SNR_GetRes, (unsigned,unsigned char,i6_snr_res *)) \
 X(sensor, MI_SNR_SetRes, (unsigned,unsigned char)) \
 X(sensor, MI_SNR_SetFps, (unsigned,unsigned)) \
 X(sensor, MI_SNR_Enable, (unsigned)) \
 X(sensor, MI_SNR_Disable, (unsigned)) \
 X(sensor, MI_SNR_GetPadInfo, (unsigned,i6_snr_pad *)) \
 X(sensor, MI_SNR_GetPlaneInfo, (unsigned,unsigned,i6_snr_plane *)) \
 X(vif, MI_VIF_SetDevAttr, (int,zr10_vif_dev *)) \
 X(vif, MI_VIF_EnableDev, (int)) \
 X(vif, MI_VIF_DisableDev, (int)) \
 X(vif, MI_VIF_SetChnPortAttr, (int,int,i6_vif_port *)) \
 X(vif, MI_VIF_EnableChnPort, (int,int)) \
 X(vif, MI_VIF_DisableChnPort, (int,int)) \
 X(vpe, MI_VPE_CreateChannel, (int,i6_vpe_chn *)) \
 X(vpe, MI_VPE_DestroyChannel, (int)) \
 X(vpe, MI_VPE_SetChannelParam, (int,i6_vpe_para *)) \
 X(vpe, MI_VPE_StartChannel, (int)) \
 X(vpe, MI_VPE_StopChannel, (int)) \
 X(vpe, MI_VPE_SetPortMode, (int,int,i6_vpe_port *)) \
 X(vpe, MI_VPE_EnablePort, (int,int)) \
 X(vpe, MI_VPE_DisablePort, (int,int)) \
 X(venc, MI_VENC_CreateChn, (int,i6_venc_chn *)) \
 X(venc, MI_VENC_DestroyChn, (int)) \
 X(venc, MI_VENC_GetChnDevid, (int,unsigned *)) \
 X(venc, MI_VENC_GetFd, (int)) \
 X(venc, MI_VENC_CloseFd, (int)) \
 X(venc, MI_VENC_StartRecvPic, (int)) \
 X(venc, MI_VENC_RequestIdr, (int,char)) \
 X(venc, MI_VENC_StopRecvPic, (int)) \
 X(venc, MI_VENC_Query, (int,i6_venc_stat *)) \
 X(venc, MI_VENC_GetStream, (int,i6_venc_strm *,unsigned)) \
 X(venc, MI_VENC_ReleaseStream, (int,i6_venc_strm *)) \
 X(isp, MI_ISP_API_CmdLoadBinFile, (unsigned,char *,unsigned))

struct mi_api {
#define MEMBER(lib,name,args) int (*name) args;
    MI_FUNCTIONS(MEMBER)
#undef MEMBER
};
