#pragma once
#include "api.h"
#include "encoder_abi.h"
#include "buffer_abi.h"
#include "isp_abi.h"
#include "rgn_abi.h"
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
namespace ca_zt30 {
/* RAM diagnostics only until the ownership/runtime gate is qualified. dlopen
 * constructors may touch MI devices; caller must stop vendor/supervisor first.
 */
class NativeSdk {
public:
    Api api{};
    EncoderApi encoder{};
    InputBufferApi input{};
    ImageApi image{};
    RgnApi rgn{};
    int (*current_pts)(uint16_t,uint64_t*)=nullptr;
    NativeSdk()=default;
    NativeSdk(const NativeSdk&)=delete;
    NativeSdk &operator=(const NativeSdk&)=delete;
    ~NativeSdk() { close(); }
    bool open(const char *bundle) {
        if(handle)return false;
        handle=dlopen(bundle,RTLD_NOW|RTLD_GLOBAL);
        if(!handle) { std::fprintf(stderr,"LOAD FAIL SDK group: %s\n",dlerror());return false; }
        if(!symbol(api.sys_init,"libmi_sys.so","MI_SYS_Init"))return false;
        if(!symbol(api.sys_exit,"libmi_sys.so","MI_SYS_Exit"))return false;
        if(!symbol(api.sys_bind,"libmi_sys.so","MI_SYS_BindChnPort2"))return false;
        if(!symbol(api.sys_unbind,"libmi_sys.so","MI_SYS_UnBindChnPort"))return false;
        if(!symbol(api.sys_depth,"libmi_sys.so","MI_SYS_SetChnOutputPortDepth"))return false;
        if(!symbol(api.snr_init,"libmi_sensor.so","MI_SNR_InitDev"))return false;
        if(!symbol(api.snr_plane_mode,"libmi_sensor.so","MI_SNR_SetPlaneMode"))return false;
        if(!symbol(api.snr_count,"libmi_sensor.so","MI_SNR_QueryResCount"))return false;
        if(!symbol(api.snr_resolution,"libmi_sensor.so","MI_SNR_GetRes"))return false;
        if(!symbol(api.snr_set_resolution,"libmi_sensor.so","MI_SNR_SetRes"))return false;
        if(!symbol(api.snr_fps,"libmi_sensor.so","MI_SNR_SetFps"))return false;
        if(!symbol(api.snr_orientation,"libmi_sensor.so","MI_SNR_SetOrien"))return false;
        if(!symbol(api.snr_enable,"libmi_sensor.so","MI_SNR_Enable"))return false;
        if(!symbol(api.snr_disable,"libmi_sensor.so","MI_SNR_Disable"))return false;
        if(!symbol(api.snr_pad,"libmi_sensor.so","MI_SNR_GetPadInfo"))return false;
        if(!symbol(api.snr_plane,"libmi_sensor.so","MI_SNR_GetPlaneInfo"))return false;
        if(!symbol(api.vif_create_group,"libmi_vif.so","MI_VIF_CreateDevGroup"))return false;
        if(!symbol(api.vif_destroy_group,"libmi_vif.so","MI_VIF_DestroyDevGroup"))return false;
        if(!symbol(api.vif_set_device,"libmi_vif.so","MI_VIF_SetDevAttr"))return false;
        if(!symbol(api.vif_enable_device,"libmi_vif.so","MI_VIF_EnableDev"))return false;
        if(!symbol(api.vif_disable_device,"libmi_vif.so","MI_VIF_DisableDev"))return false;
        if(!symbol(api.vif_set_port,"libmi_vif.so","MI_VIF_SetOutputPortAttr"))return false;
        if(!symbol(api.vif_enable_port,"libmi_vif.so","MI_VIF_EnableOutputPort"))return false;
        if(!symbol(api.vif_disable_port,"libmi_vif.so","MI_VIF_DisableOutputPort"))return false;
        if(!symbol(api.isp_create_device,"libmi_isp.so","MI_ISP_CreateDevice"))return false;
        if(!symbol(api.isp_destroy_device,"libmi_isp.so","MI_ISP_DestoryDevice"))return false;
        if(!symbol(api.isp_create_channel,"libmi_isp.so","MI_ISP_CreateChannel"))return false;
        if(!symbol(api.isp_destroy_channel,"libmi_isp.so","MI_ISP_DestroyChannel"))return false;
        if(!symbol(api.isp_disable_3a,"libmi_isp.so","MI_ISP_DisableUserspace3A"))return false;
        if(!symbol(api.isp_set_parameters,"libmi_isp.so","MI_ISP_SetChnParam"))return false;
        if(!symbol(api.isp_start_channel,"libmi_isp.so","MI_ISP_StartChannel"))return false;
        if(!symbol(api.isp_stop_channel,"libmi_isp.so","MI_ISP_StopChannel"))return false;
        if(!symbol(api.isp_set_port,"libmi_isp.so","MI_ISP_SetOutputPortParam"))return false;
        if(!symbol(api.isp_enable_port,"libmi_isp.so","MI_ISP_EnableOutputPort"))return false;
        if(!symbol(api.isp_disable_port,"libmi_isp.so","MI_ISP_DisableOutputPort"))return false;
        if(!symbol(api.isp_load_iq,"libmi_isp.so","MI_ISP_API_CmdLoadBinFile"))return false;
        if(!symbol(api.scl_create_device,"libmi_scl.so","MI_SCL_CreateDevice"))return false;
        if(!symbol(api.scl_destroy_device,"libmi_scl.so","MI_SCL_DestroyDevice"))return false;
        if(!symbol(api.scl_create_channel,"libmi_scl.so","MI_SCL_CreateChannel"))return false;
        if(!symbol(api.scl_destroy_channel,"libmi_scl.so","MI_SCL_DestroyChannel"))return false;
        if(!symbol(api.scl_set_rotation,"libmi_scl.so","MI_SCL_SetChnParam"))return false;
        if(!symbol(api.scl_start_channel,"libmi_scl.so","MI_SCL_StartChannel"))return false;
        if(!symbol(api.scl_stop_channel,"libmi_scl.so","MI_SCL_StopChannel"))return false;
        if(!symbol(api.scl_set_port,"libmi_scl.so","MI_SCL_SetOutputPortParam"))return false;
        if(!symbol(api.scl_enable_port,"libmi_scl.so","MI_SCL_EnableOutputPort"))return false;
        if(!symbol(api.scl_disable_port,"libmi_scl.so","MI_SCL_DisableOutputPort"))return false;
        return true;
    }
    // Only use with the separately pinned encoder dependency group.
    bool resolve_encoder() {
        if(!handle)return false;
#define LOAD(field,name) if(!symbol(encoder.field,"libmi_venc.so",name))return false
        LOAD(create_device,"MI_VENC_CreateDev");LOAD(destroy_device,"MI_VENC_DestroyDev");
        LOAD(create_channel,"MI_VENC_CreateChn");LOAD(destroy_channel,"MI_VENC_DestroyChn");
        LOAD(set_input_source,"MI_VENC_SetInputSourceConfig");
        LOAD(start,"MI_VENC_StartRecvPic");LOAD(stop,"MI_VENC_StopRecvPic");
        LOAD(query,"MI_VENC_Query");LOAD(get_stream,"MI_VENC_GetStream");
        LOAD(release_stream,"MI_VENC_ReleaseStream");
#undef LOAD
        return true;
    }
    bool resolve_jpeg() {
        return handle && symbol(encoder.start_once,"libmi_venc.so","MI_VENC_StartRecvPicEx");
    }
    bool resolve_h264_getter() {
        encoder.get_h264_vui=nullptr;encoder.set_h264_vui=nullptr;
        if(!handle)return false;
        return symbol(encoder.get_h264_vui,"libmi_venc.so","MI_VENC_GetH264Vui");
    }
    bool resolve_h264_timing() {
        encoder.get_h264_vui=nullptr;encoder.set_h264_vui=nullptr;
        if(!handle)return false;
        decltype(encoder.get_h264_vui) get=nullptr;
        decltype(encoder.set_h264_vui) set=nullptr;
        if(!symbol(get,"libmi_venc.so","MI_VENC_GetH264Vui") ||
           !symbol(set,"libmi_venc.so","MI_VENC_SetH264Vui"))return false;
        // Publish only a complete pair; no partially resolved API on failure.
        encoder.get_h264_vui=get;encoder.set_h264_vui=set;return true;
    }
    bool resolve_input() {
        return handle && symbol(input.get,"libmi_sys.so","MI_SYS_ChnInputPortGetBuf") &&
            symbol(input.put,"libmi_sys.so","MI_SYS_ChnInputPortPutBuf");
    }
    bool resolve_clock() {
        return handle && symbol(current_pts,"libmi_sys.so","MI_SYS_GetCurPts");
    }
    bool resolve_image() {
        if(!handle)return false;
#define LOAD(field,name) if(!symbol(image.field,"libmi_isp.so",name))return false
        LOAD(get_brightness,"MI_ISP_IQ_GetBrightness");LOAD(set_brightness,"MI_ISP_IQ_SetBrightness");
        LOAD(get_contrast,"MI_ISP_IQ_GetContrast");LOAD(set_contrast,"MI_ISP_IQ_SetContrast");
        LOAD(get_saturation,"MI_ISP_IQ_GetSaturation");LOAD(set_saturation,"MI_ISP_IQ_SetSaturation");
        LOAD(get_wb,"MI_ISP_AWB_GetAttr");LOAD(set_wb,"MI_ISP_AWB_SetAttr");
        LOAD(get_ev,"MI_ISP_AE_GetEVComp");LOAD(set_ev,"MI_ISP_AE_SetEVComp");
        LOAD(get_mode,"MI_ISP_AE_GetExpoMode");LOAD(set_mode,"MI_ISP_AE_SetExpoMode");
        LOAD(get_manual,"MI_ISP_AE_GetManualExpo");LOAD(set_manual,"MI_ISP_AE_SetManualExpo");
        LOAD(get_limit,"MI_ISP_AE_GetExposureLimit");LOAD(set_limit,"MI_ISP_AE_SetExposureLimit");
        LOAD(get_metering,"MI_ISP_AE_GetWinWgtType");LOAD(set_metering,"MI_ISP_AE_SetWinWgtType");
        LOAD(query,"MI_ISP_AE_QueryExposureInfo");
#undef LOAD
        return true;
    }
    bool resolve_rgn() {
        if(!handle)return false;
#define LOAD(field,name) if(!symbol(rgn.field,"libmi_rgn.so",name))return false
        LOAD(init,"MI_RGN_Init");LOAD(deinit,"MI_RGN_DeInit");
        LOAD(create,"MI_RGN_Create");LOAD(destroy,"MI_RGN_Destroy");
        LOAD(bitmap,"MI_RGN_SetBitMap");LOAD(attach,"MI_RGN_AttachToChn");
        LOAD(detach,"MI_RGN_DetachFromChn");LOAD(display,"MI_RGN_SetDisplayAttr");
#undef LOAD
        return true;
    }
    void close() {
        if(handle)dlclose(handle);
        handle=nullptr;api={};encoder={};input={};image={};rgn={};current_pts=nullptr;
    }
private:
    void *handle=nullptr;
    template<class T> bool symbol(T &output,const char *library,const char *name) {
        output=reinterpret_cast<T>(dlsym(handle,name));
        if(output)return true;
        std::fprintf(stderr,"SYMBOL FAIL %s/%s: %s\n",library,name,dlerror());
        return false;
    }
};
} // namespace ca_zt30
