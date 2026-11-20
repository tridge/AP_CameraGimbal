#include "rgb_owner.h"
#include <cstring>
namespace ca_zt30 {
bool RgbOwner::check(int result,const char *call) {
    if(!result)return true;
    unhealthy=true;
    if(!failed_call) { failed_call=call;raw_status=result; }
    return false;
}
void RgbOwner::cleanup(int result,const char *call,int &first_error) {
    if(result && !first_error)first_error=result;
    (void)check(result,call);
}
#define CALL(expression) do { if(!check((expression),#expression)) { close();return raw_status; } } while(0)
int RgbOwner::open(bool wide,bool borrow_devices,unsigned scaler_mask) {
    if(path || sys || unhealthy) return -1;
    failed_call=nullptr;raw_status=0;path=wide?&WidePath:&ZoomPath;
    if(!borrow_devices) { CALL(a.sys_init(0));sys=true;CALL(a.snr_init(nullptr)); }
    CALL(a.snr_plane_mode(path->sensor_pad,0));
    uint32_t count=0;
    CALL(a.snr_count(path->sensor_pad,&count));
    if(count<=path->sensor_resolution) { check(-1,"sensor resolution absent");close();return raw_status; }
    SensorResolution resolution{};
    CALL(a.snr_resolution(path->sensor_pad,path->sensor_resolution,&resolution));
    if(std::memcmp(&resolution.capture,&path->sensor_capture,sizeof(Rect)) || resolution.max_fps<30) {
        check(-1,"sensor mode differs from qualified graph");close();return raw_status;
    }
    CALL(a.snr_set_resolution(path->sensor_pad,path->sensor_resolution));
    CALL(a.snr_fps(path->sensor_pad,30));
    CALL(a.snr_orientation(path->sensor_pad,path->mirrored,path->flipped));
    sensor=true; // Enable may partially activate before returning an error.
    CALL(a.snr_enable(path->sensor_pad));
    SensorPad pad{};SensorPlane plane{};
    CALL(a.snr_pad(path->sensor_pad,&pad));
    CALL(a.snr_plane(path->sensor_pad,0,&plane));
    if(pad.interface_type!=4 || pad.plane_count!=2 || plane.bayer!=path->bayer || plane.precision!=path->precision || std::memcmp(&plane.capture,&path->sensor_capture,sizeof(Rect))) {
        check(-1,"sensor plane differs from qualified graph");close();return raw_status;
    }
    auto group=ca_zt30::vif_group(*path);
    CALL(a.vif_create_group(path->vif_group,&group));vif_group=true;
    auto device=ca_zt30::vif_device(*path);
    CALL(a.vif_set_device(path->vif_device,&device));
    vif_device=true; // Unwind even if activation returns a partial failure.
    CALL(a.vif_enable_device(path->vif_device));
    auto vport=ca_zt30::vif_port(*path);
    CALL(a.vif_set_port(path->vif_device,0,&vport));
    vif_port=true; // Unwind even if activation returns a partial failure.
    CALL(a.vif_enable_port(path->vif_device,0));
    auto voutput=vif_output(*path);
    CALL(a.sys_depth(0,&voutput,0,4));
    uint32_t combo=1;
    if(!borrow_devices) { CALL(a.isp_create_device(0,&combo));isp_device=true; }
    auto channel=ca_zt30::isp_channel(*path);
    const int created=a.isp_create_channel(0,path->isp_channel,&channel);
    if(created) {
        check(created,"a.isp_create_channel");
        // The pinned wrapper calls EnableUserspace3A even on ioctl failure.
        int error=0;cleanup(a.isp_disable_3a(0,path->isp_channel),"a.isp_disable_3a",error);
        close();return raw_status;
    }
    isp_channel=true;
    auto parameters=isp_parameters();
    CALL(a.isp_set_parameters(0,path->isp_channel,&parameters));
    isp_started=true; // Unwind even if activation returns a partial failure.
    CALL(a.isp_start_channel(0,path->isp_channel));
    auto iport=ca_zt30::isp_port();
    CALL(a.isp_set_port(0,path->isp_channel,0,&iport));
    isp_port=true; // Unwind even if activation returns a partial failure.
    CALL(a.isp_enable_port(0,path->isp_channel,0));
    auto ioutput=isp_input(*path);
    CALL(a.sys_depth(0,&ioutput,0,4));
    CALL(a.sys_bind(0,&voutput,&ioutput,30,30,FrameBind,0));bound_isp=true;
    uint32_t mask=scaler_mask;
    if(!borrow_devices) { CALL(a.scl_create_device(0,&mask));scl_device=true; }
    uint32_t reserved=0;
    CALL(a.scl_create_channel(0,path->scl_channel,&reserved));scl_channel=true;
    uint32_t rotation=0;
    CALL(a.scl_set_rotation(0,path->scl_channel,&rotation));
    scl_started=true; // Unwind even if activation returns a partial failure.
    CALL(a.scl_start_channel(0,path->scl_channel));
    auto sport=ca_zt30::scl_port(1280,720);
    CALL(a.scl_set_port(0,path->scl_channel,1,&sport));
    scl_port=true; // Unwind even if activation returns a partial failure.
    CALL(a.scl_enable_port(0,path->scl_channel,1));
    Port soutput{Scl,0,path->scl_channel,1};
    // No application queue until a frame consumer is implemented. Reserving
    // an unread user buffer would stall this qualification graph.
    CALL(a.sys_depth(0,&soutput,0,4));
    auto sinput=scl_input(*path);
    CALL(a.sys_bind(0,&ioutput,&sinput,30,30,RealtimeBind,0));bound_scl=true;
    return 0;
}
#undef CALL
int RgbOwner::load_iq() {
    if(!ready() || !path || unhealthy)return -1;
    const int result=a.isp_load_iq(0,path->isp_channel,path->iq_file,1234);
    (void)check(result,"a.isp_load_iq");return result;
}
int RgbOwner::close() {
    int first_error=0;
    if(!path)return 0;
    auto voutput=vif_output(*path),ioutput=isp_input(*path),sinput=scl_input(*path);
#define CLEAN(flag,expression) do { if(flag) { cleanup((expression),#expression,first_error);flag=false; } } while(0)
    CLEAN(bound_scl,a.sys_unbind(0,&ioutput,&sinput));
    CLEAN(scl_port,a.scl_disable_port(0,path->scl_channel,1));
    CLEAN(scl_started,a.scl_stop_channel(0,path->scl_channel));
    CLEAN(scl_channel,a.scl_destroy_channel(0,path->scl_channel));
    CLEAN(scl_device,a.scl_destroy_device(0));
    CLEAN(bound_isp,a.sys_unbind(0,&voutput,&ioutput));
    CLEAN(isp_port,a.isp_disable_port(0,path->isp_channel,0));
    CLEAN(isp_started,a.isp_stop_channel(0,path->isp_channel));
    // DestroyChannel disables/relinquishes SDK userspace3A first, then the
    // kernel channel. No separate double CUS3A_Release on the success path.
    CLEAN(isp_channel,a.isp_destroy_channel(0,path->isp_channel));
    CLEAN(isp_device,a.isp_destroy_device(0));
    CLEAN(vif_port,a.vif_disable_port(path->vif_device,0));
    CLEAN(vif_device,a.vif_disable_device(path->vif_device));
    CLEAN(vif_group,a.vif_destroy_group(path->vif_group));
    CLEAN(sensor,a.snr_disable(path->sensor_pad));
    CLEAN(sys,a.sys_exit(0));
#undef CLEAN
    path=nullptr;return first_error;
}
} // namespace ca_zt30
