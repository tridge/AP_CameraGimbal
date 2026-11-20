/* Known flat values from the qualified ZT30 warm-start capture, converted
 * to small SDK argument fixtures. No camera I/O, SDK loading or pointers.
 */
#include "../camera_app/src/backends/zt30/media_graph.h"
#include "../camera_app/src/backends/zt30/mi/encoder_abi.h"
#include <cassert>
#include <cstring>
using namespace ca_zt30;
template<class T,size_t N> void matches(const T &actual,const uint32_t (&words)[N]) {
    static_assert(sizeof(T)==N*sizeof(uint32_t),"fixture size");
    assert(std::memcmp(&actual,words,sizeof(actual))==0);
}
int main() {
    const uint32_t zoom_vif[]={35,0x00020002,0x08700f00,0,0};
    const uint32_t wide_vif[]={32,0,0x05b20a20,0,0};
    matches(vif_device(ZoomPath),zoom_vif);matches(vif_device(WidePath),wide_vif);
    const uint32_t zoom_port[]={0x00020002,0x08700f00,0x08700f00,35,0};
    matches(vif_port(ZoomPath),zoom_port);
    const uint32_t group[]={4,0,0,2,0,0,0};matches(vif_group(WidePath),group);
    const uint32_t isp_params[]={0,2,0,0};matches(isp_parameters(),isp_params);
    const uint32_t isp_crop[]={0,0,0};matches(isp_port(),isp_crop);
    const uint32_t sub_scl[]={0,0,0x02d00500,0,11,0};matches(scl_port(1280,720),sub_scl);
    const uint32_t h265[]={3,3840,2160,0,0,1,1280,720,0,0,8,60,0,30,1,1570000,0,0,0};
    matches(h265_channel(1280,720,1570000),h265);
    const auto zoom=isp_channel(ZoomPath),wide=isp_channel(WidePath);
    assert(zoom.sensor_id_mask==1 && wide.sensor_id_mask==2);
    assert(zoom.sync_3a==0 && wide.sync_3a==0);
    assert(ZoomPath.mirrored && ZoomPath.flipped && !WidePath.mirrored && !WidePath.flipped);
    assert(vif_output(WidePath).device==8 && isp_input(WidePath).channel==1 && scl_input(WidePath).channel==1);
    assert(WidePath.sensor_capture.height==1944 && WidePath.vif_crop.height==1458);
    return 0;
}
