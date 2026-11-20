#include "camera_app/src/backends/zt30/isp.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace ca_zt30;
struct Sensor {
    IspLevel brightness{},contrast{};
    IspSaturation saturation{};
    IspWhiteBalance wb{};
    IspEv ev{};
    IspExposure manual{};
    IspExposureLimit limit{};
    uint32_t mode=0,metering=0;
};
static Sensor sensors[2];
static int fail_set=-1,fail_get=-1;
static unsigned setters=0;
#define ACCESS(member) \
static int get_##member(uint32_t device,uint32_t channel,void *out) { \
    assert(device==0 && channel<2); \
    if(fail_get==int(channel)) { fail_get=-1;return 17; } \
    memcpy(out,&sensors[channel].member,sizeof(sensors[channel].member));return 0; \
} \
static int set_##member(uint32_t device,uint32_t channel,void *in) { \
    assert(device==0 && channel<2);setters++; \
    memcpy(&sensors[channel].member,in,sizeof(sensors[channel].member)); \
    if(fail_set==int(channel)) { fail_set=-1;return 17; } \
    return 0; \
}
ACCESS(brightness) ACCESS(contrast) ACCESS(saturation) ACCESS(wb) ACCESS(ev)
ACCESS(manual) ACCESS(limit) ACCESS(mode) ACCESS(metering)
#undef ACCESS
static int query(uint32_t device,uint32_t channel,void *out) {
    assert(!device && channel<2);auto &q=*static_cast<ca_sstar_exposure_info*>(out);
    q={};q.long_value.us=1000+channel;q.long_value.sensor_gain=2048;
    q.long_value.isp_gain=1024;q.weighted_y=60;q.target=50;q.stable=1;return 0;
}
static ImageApi api() {
    ImageApi a;
#define PAIR(member) a.get_##member=get_##member;a.set_##member=set_##member
    PAIR(brightness);PAIR(contrast);PAIR(saturation);PAIR(wb);PAIR(ev);
    PAIR(manual);PAIR(limit);PAIR(mode);PAIR(metering);
#undef PAIR
    a.query=query;return a;
}
static ca_config defaults() {
    ca_config c{};c.brightness=c.saturation=c.contrast=50;return c;
}
static void reset() {
    for(auto &s:sensors) {
        s={};s.brightness.manual=s.contrast.manual=50;
        for(unsigned i=0;i<16;i++) { s.brightness.automatic[i]=i+30;s.contrast.automatic[i]=i+40; }
        memset(s.saturation.automatic,0x2a,sizeof(s.saturation.automatic));
        memset(s.saturation.manual,0x34,sizeof(s.saturation.manual));
        memset(s.wb.automatic,0x53,sizeof(s.wb.automatic));
        s.manual={16,1024,1024,14000};
        s.limit={300,14000,18,18,1024,1024,8192,1024};s.ev={0,3};
    }
    fail_set=fail_get=-1;setters=0;
}
int main() {
    reset();auto a=api();ImageControls image(a);auto c=defaults();
    assert(image.initialize(3,c)==0);
    for(const auto &s:sensors) {
        assert(s.brightness.enabled==1 && s.brightness.mode==1 && s.brightness.manual==50);
        assert(s.saturation.manual[0]==32);
        assert(s.saturation.manual[1]==0x34 && s.wb.automatic[500]==0x53);
        assert(s.limit.min_shutter==300 && s.limit.max_shutter==14000);
    }
    c.brightness=63;c.contrast=71;c.saturation=0;c.exposure_compensation=-5;
    c.iso=CA_ISO_200;c.shutter=CA_SHUTTER_1_1000;c.metering=CA_METERING_SPOT;c.white_balance=CA_WB_CLOUDY;
    assert(image.apply(c)==0);
    for(const auto &s:sensors) {
        assert(s.brightness.manual==63 && s.contrast.manual==71 && s.saturation.manual[0]==0);
        assert(s.ev.value==-5 && s.ev.denominator==10 && s.metering==2);
        assert(s.manual.shutter_us==1000 && s.manual.sensor_gain==2048 && s.manual.isp_gain==1024);
        assert(s.manual.aperture==16 && s.mode==4 && s.wb.mode==1);
        assert(s.brightness.automatic[15]==45 && s.contrast.automatic[15]==55);
    }
    c.iso=CA_ISO_3200;assert(image.apply(c)==0);
    assert(sensors[0].manual.sensor_gain==8192 && sensors[0].manual.isp_gain==4096);
    c.iso=CA_ISO_AUTO;c.shutter=CA_SHUTTER_1_30;assert(image.apply(c)==0);
    assert(sensors[0].mode==3 && sensors[0].manual.shutter_us==33333 && sensors[0].limit.max_shutter==33333);
    c.shutter=CA_SHUTTER_AUTO;c.iso=CA_ISO_100;assert(image.apply(c)==0);
    assert(sensors[0].mode==2 && sensors[0].limit.max_shutter==14000);
    c.iso=CA_ISO_AUTO;c.white_balance=CA_WB_AUTO;assert(image.apply(c)==0);
    assert(!sensors[0].mode && !sensors[0].wb.mode);
    Sensor before[2];memcpy(before,sensors,sizeof(before));
    c.brightness=35;fail_set=1;
    assert(image.apply(c)<0 && errno==EIO);
    assert(!memcmp(before,sensors,sizeof(before))); // Including partial failed writes on lens 2.
    setters=0;fail_get=1;assert(image.apply(c)<0 && setters==0);
    c.brightness=101;assert(image.apply(c)<0 && errno==EINVAL && setters==0);
    ca_exposure e=ca_exposure_empty(0,0);assert(image.exposure(0,&e)==0 && e.shutter_us==1001);
    assert(e.analog_gain==2 && e.isp_gain==1 && e.luma==60 && e.error==-10 && e.mode==CA_AE_AUTO);
    assert(image.exposure(2,&e)<0 && errno==ENOTSUP);
    reset();ImageControls one(a);assert(one.initialize(1,defaults())==0);
    assert(sensors[1].brightness.enabled==0 && one.exposure(0,&e)<0);
    puts("PASS ZT30 image controls: both RGB sensors, IQ preservation, all AE modes, rollback and exposure readback");
}
