/* Failure injection oracle: every acquired resource is released, 3A is
 * disabled after failed ISP create, cleanup continues, and failed owners
 * cannot reopen. Explicit argument checks exercise mixed-SocId call shapes.
 */
#include "../camera_app/src/backends/zt30/rgb_owner.h"
#include <cassert>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace ca_zt30;
static std::vector<std::string> calls;
static std::set<std::string> resources;
static unsigned fail_index;
static unsigned expected_scaler_mask=3;
static const RgbPath *expected;
static bool wrong_resolution=false,cleanup_fault=false;
static int event(const char *name) {
    calls.emplace_back(name);
    const bool fail=fail_index==calls.size();
    if(fail && (std::strstr(name,"disable") || std::strstr(name,"destroy") || std::strstr(name,"stop") || !std::strcmp(name,"sys_unbind") || !std::strcmp(name,"sys_exit")))cleanup_fault=true;
    return fail?-99:0;
}
static Api fake_api() {
    Api a{};
    a.sys_init=[](uint16_t p0) {
        (void)p0;
        assert(p0==0);
        const int result=event("sys_init");
        if(!result)resources.insert("SYS");
        return result;
    };
    a.sys_exit=[](uint16_t p0) {
        (void)p0;
        assert(p0==0);
        const int result=event("sys_exit");
        if(!result) { assert(resources.size()==1 || cleanup_fault);resources.clear(); }
        return result;
    };
    a.sys_bind=[](uint16_t p0,Port* p1,Port* p2,uint32_t p3,uint32_t p4,uint32_t p5,uint32_t p6) {
        (void)p0;(void)p1;(void)p2;(void)p3;(void)p4;(void)p5;(void)p6;
        assert(p0==0);
        assert(p3==30 && p4==30 && p6==0);assert((p1->module==Vif && p2->module==Isp && p5==1) || (p1->module==Isp && p2->module==Scl && p5==4));
        const int result=event("sys_bind");
        if(!result)resources.insert(p1->module==Vif?"BIND_ISP":"BIND_SCL");
        return result;
    };
    a.sys_unbind=[](uint16_t p0,Port* p1,Port* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        const int result=event("sys_unbind");
        if(!result)assert(resources.erase(p1->module==Vif?"BIND_ISP":"BIND_SCL"));
        return result;
    };
    a.sys_depth=[](uint16_t p0,Port* p1,uint32_t p2,uint32_t p3) {
        (void)p0;(void)p1;(void)p2;(void)p3;
        assert(p0==0);
        assert(p2==0 && p3==4);
        const int result=event("sys_depth");
        return result;
    };
    a.snr_init=[](void* p0) {
        (void)p0;
        assert(!p0);
        const int result=event("snr_init");
        return result;
    };
    a.snr_plane_mode=[](uint32_t p0,uint8_t p1) {
        (void)p0;(void)p1;
        assert(p0==expected->sensor_pad);
        const int result=event("snr_plane_mode");
        return result;
    };
    a.snr_count=[](uint32_t p0,uint32_t* p1) {
        (void)p0;(void)p1;
        assert(p0==expected->sensor_pad);
        *p1=expected->sensor_pad?2:8;
        const int result=event("snr_count");
        return result;
    };
    a.snr_resolution=[](uint32_t p0,uint8_t p1,SensorResolution* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==expected->sensor_pad);
        assert(p1==expected->sensor_resolution);*p2={};p2->capture=expected->sensor_capture;p2->max_fps=wrong_resolution?20:30;
        const int result=event("snr_resolution");
        return result;
    };
    a.snr_set_resolution=[](uint32_t p0,uint8_t p1) {
        (void)p0;(void)p1;
        assert(p0==expected->sensor_pad);
        assert(p1==expected->sensor_resolution);
        const int result=event("snr_set_resolution");
        return result;
    };
    a.snr_fps=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==expected->sensor_pad);
        assert(p1==30);
        const int result=event("snr_fps");
        return result;
    };
    a.snr_orientation=[](uint32_t p0,uint8_t p1,uint8_t p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==expected->sensor_pad);
        assert(p1==expected->mirrored && p2==expected->flipped);
        const int result=event("snr_orientation");
        return result;
    };
    a.snr_enable=[](uint32_t p0) {
        (void)p0;
        assert(p0==expected->sensor_pad);
        const int result=event("snr_enable");
        resources.insert("SENSOR");
        return result;
    };
    a.snr_disable=[](uint32_t p0) {
        (void)p0;
        assert(p0==expected->sensor_pad);
        const int result=event("snr_disable");
        if(!result)assert(resources.erase("SENSOR"));
        return result;
    };
    a.snr_pad=[](uint32_t p0,SensorPad* p1) {
        (void)p0;(void)p1;
        assert(p0==expected->sensor_pad);
        *p1={};p1->interface_type=4;p1->plane_count=2;
        const int result=event("snr_pad");
        return result;
    };
    a.snr_plane=[](uint32_t p0,uint32_t p1,SensorPlane* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==expected->sensor_pad);
        assert(p1==0);*p2={};p2->capture=expected->sensor_capture;p2->bayer=expected->bayer;p2->precision=expected->precision;
        const int result=event("snr_plane");
        return result;
    };
    a.vif_create_group=[](uint32_t p0,VifGroup* p1) {
        (void)p0;(void)p1;
        assert(p0==expected->vif_group);
        const int result=event("vif_create_group");
        if(!result)resources.insert("VIF_GROUP");
        return result;
    };
    a.vif_destroy_group=[](uint32_t p0) {
        (void)p0;
        assert(p0==expected->vif_group);
        const int result=event("vif_destroy_group");
        if(!result)assert(resources.erase("VIF_GROUP"));
        return result;
    };
    a.vif_set_device=[](uint32_t p0,VifDevice* p1) {
        (void)p0;(void)p1;
        assert(p0==expected->vif_device);
        const int result=event("vif_set_device");
        return result;
    };
    a.vif_enable_device=[](uint32_t p0) {
        (void)p0;
        assert(p0==expected->vif_device);
        const int result=event("vif_enable_device");
        resources.insert("VIF_DEVICE"); // Model an activation which fails after changing state.
        return result;
    };
    a.vif_disable_device=[](uint32_t p0) {
        (void)p0;
        assert(p0==expected->vif_device);
        const int result=event("vif_disable_device");
        if(!result)assert(resources.erase("VIF_DEVICE"));
        return result;
    };
    a.vif_set_port=[](uint32_t p0,uint32_t p1,VifPort* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==expected->vif_device);
        assert(p1==0);
        const int result=event("vif_set_port");
        return result;
    };
    a.vif_enable_port=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==expected->vif_device);
        assert(p1==0);
        const int result=event("vif_enable_port");
        resources.insert("VIF_PORT"); // Model an activation which fails after changing state.
        return result;
    };
    a.vif_disable_port=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==expected->vif_device);
        assert(p1==0);
        const int result=event("vif_disable_port");
        if(!result)assert(resources.erase("VIF_PORT"));
        return result;
    };
    a.isp_create_device=[](uint32_t p0,uint32_t* p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(*p1==1);
        const int result=event("isp_create_device");
        if(!result)resources.insert("ISP_DEVICE");
        return result;
    };
    a.isp_destroy_device=[](uint32_t p0) {
        (void)p0;
        assert(p0==0);
        const int result=event("isp_destroy_device");
        if(!result)assert(resources.erase("ISP_DEVICE"));
        return result;
    };
    a.isp_create_channel=[](uint32_t p0,uint32_t p1,IspChannel* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        assert(p2->sensor_id_mask==(1U<<expected->sensor_pad));
        const int result=event("isp_create_channel");
        if(!result)resources.insert("ISP_CHANNEL");
        resources.insert("3A"); // The SDK starts 3A even on a failed create.
        return result;
    };
    a.isp_destroy_channel=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        const int result=event("isp_destroy_channel");
        if(!result)assert(resources.erase("ISP_CHANNEL"));
        if(!result)assert(resources.erase("3A"));
        return result;
    };
    a.isp_disable_3a=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        const int result=event("isp_disable_3a");
        if(!result)assert(resources.erase("3A"));
        return result;
    };
    a.isp_set_parameters=[](uint32_t p0,uint32_t p1,IspParameters* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        const int result=event("isp_set_parameters");
        return result;
    };
    a.isp_start_channel=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        const int result=event("isp_start_channel");
        resources.insert("ISP_STARTED"); // Model an activation which fails after changing state.
        return result;
    };
    a.isp_stop_channel=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        const int result=event("isp_stop_channel");
        if(!result)assert(resources.erase("ISP_STARTED"));
        return result;
    };
    a.isp_set_port=[](uint32_t p0,uint32_t p1,uint32_t p2,IspPort* p3) {
        (void)p0;(void)p1;(void)p2;(void)p3;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        assert(p2==0);
        const int result=event("isp_set_port");
        return result;
    };
    a.isp_enable_port=[](uint32_t p0,uint32_t p1,uint32_t p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        assert(p2==0);
        const int result=event("isp_enable_port");
        resources.insert("ISP_PORT"); // Model an activation which fails after changing state.
        return result;
    };
    a.isp_disable_port=[](uint32_t p0,uint32_t p1,uint32_t p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        assert(p2==0);
        const int result=event("isp_disable_port");
        if(!result)assert(resources.erase("ISP_PORT"));
        return result;
    };
    a.isp_load_iq=[](uint32_t p0,uint32_t p1,const char* p2,uint32_t p3) {
        (void)p0;(void)p1;(void)p2;(void)p3;
        assert(p0==0);
        assert(p1==expected->isp_channel);
        assert(!std::strcmp(p2,expected->iq_file) && p3==1234);
        const int result=event("isp_load_iq");
        return result;
    };
    a.scl_create_device=[](uint32_t p0,uint32_t* p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(*p1==expected_scaler_mask);
        const int result=event("scl_create_device");
        if(!result)resources.insert("SCL_DEVICE");
        return result;
    };
    a.scl_destroy_device=[](uint32_t p0) {
        (void)p0;
        assert(p0==0);
        const int result=event("scl_destroy_device");
        if(!result)assert(resources.erase("SCL_DEVICE"));
        return result;
    };
    a.scl_create_channel=[](uint32_t p0,uint32_t p1,uint32_t* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        assert(*p2==0);
        const int result=event("scl_create_channel");
        if(!result)resources.insert("SCL_CHANNEL");
        return result;
    };
    a.scl_destroy_channel=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        const int result=event("scl_destroy_channel");
        if(!result)assert(resources.erase("SCL_CHANNEL"));
        return result;
    };
    a.scl_set_rotation=[](uint32_t p0,uint32_t p1,uint32_t* p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        assert(*p2==0);
        const int result=event("scl_set_rotation");
        return result;
    };
    a.scl_start_channel=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        const int result=event("scl_start_channel");
        resources.insert("SCL_STARTED"); // Model an activation which fails after changing state.
        return result;
    };
    a.scl_stop_channel=[](uint32_t p0,uint32_t p1) {
        (void)p0;(void)p1;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        const int result=event("scl_stop_channel");
        if(!result)assert(resources.erase("SCL_STARTED"));
        return result;
    };
    a.scl_set_port=[](uint32_t p0,uint32_t p1,uint32_t p2,SclPort* p3) {
        (void)p0;(void)p1;(void)p2;(void)p3;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        assert(p2==1);
        assert(p3->output.width==1280 && p3->output.height==720 && p3->pixel_format==11);
        const int result=event("scl_set_port");
        return result;
    };
    a.scl_enable_port=[](uint32_t p0,uint32_t p1,uint32_t p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        assert(p2==1);
        const int result=event("scl_enable_port");
        resources.insert("SCL_PORT"); // Model an activation which fails after changing state.
        return result;
    };
    a.scl_disable_port=[](uint32_t p0,uint32_t p1,uint32_t p2) {
        (void)p0;(void)p1;(void)p2;
        assert(p0==0);
        assert(p1==expected->scl_channel);
        assert(p2==1);
        const int result=event("scl_disable_port");
        if(!result)assert(resources.erase("SCL_PORT"));
        return result;
    };
    return a;
}
static void reset(bool wide,unsigned failure=0) {
    resources.clear();calls.clear();fail_index=failure;expected=wide?&WidePath:&ZoomPath;wrong_resolution=false;cleanup_fault=false;
}
int main() {
    auto api=fake_api();
    reset(false);expected_scaler_mask=31;
    { RgbOwner owner(api);assert(owner.open(false,false,31)==0 && owner.close()==0 && resources.empty()); }
    expected_scaler_mask=3;
    unsigned startup_count=0;
    for(bool wide:{false,true}) {
        reset(wide);RgbOwner owner(api);
        assert(owner.load_iq()==-1 && calls.empty());
        assert(owner.open(wide)==0 && owner.ready());startup_count=calls.size();
        assert(owner.open(wide)==-1 && calls.size()==startup_count);
        assert(owner.load_iq()==0);
        assert(owner.close()==0 && !owner.ready() && resources.empty());
        const auto closed=calls.size();assert(owner.close()==0 && calls.size()==closed);
        assert(owner.open(wide)==0);assert(owner.close()==0 && resources.empty());
    }
    for(bool wide:{false,true})for(unsigned step=1;step<=startup_count;step++) {
        reset(wide,step);RgbOwner owner(api);
        assert(owner.open(wide)==-99 && owner.failed_call && owner.raw_status==-99);
        assert(!owner.ready() && resources.empty());
        const auto count=calls.size();assert(owner.close()==0 && calls.size()==count);
        assert(owner.open(wide)==-1 && calls.size()==count);
    }
    for(bool wide:{false,true}) {
        reset(wide);resources={"SYS","ISP_DEVICE","SCL_DEVICE"};
        const auto borrowed=resources;
        RgbOwner secondary(api);assert(secondary.open(wide,true)==0);
        assert(secondary.close()==0 && resources==borrowed);
        for(const auto &call:calls)assert(call!="sys_init" && call!="snr_init" &&
            call!="sys_exit" && call!="isp_create_device" && call!="isp_destroy_device" &&
            call!="scl_create_device" && call!="scl_destroy_device");
        reset(wide);resources=borrowed;RgbOwner count(api);assert(count.open(wide,true)==0);
        const unsigned starts=unsigned(calls.size());assert(count.close()==0 && resources==borrowed);
        for(unsigned fail=1;fail<=starts;fail++) {
            reset(wide,fail);resources=borrowed;RgbOwner bad(api);
            assert(bad.open(wide,true)==-99 && !bad.ready());
            assert(resources==borrowed);assert(bad.close()==0);
        }
    }
    reset(false);wrong_resolution=true;RgbOwner incompatible(api);
    assert(incompatible.open()==-1 && resources.empty());
    assert(calls.back()=="sys_exit");
    reset(false);RgbOwner cleanup_failure(api);assert(cleanup_failure.open()==0);
    fail_index=calls.size()+1;assert(cleanup_failure.close()==-99);
    assert(calls.back()=="sys_exit" && resources.empty());
    assert(cleanup_failure.open()==-1);
    reset(false);RgbOwner iq_failure(api);assert(iq_failure.open()==0);
    fail_index=calls.size()+1;assert(iq_failure.load_iq()==-99);
    assert(iq_failure.close()==0 && resources.empty());
    return 0;
}
