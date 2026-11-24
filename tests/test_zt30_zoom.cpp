#include "../camera_app/src/backends/zt30/zoom_control.h"
#include <cassert>
#include <cstdio>
#include <limits>
using namespace ca_zt30;
static SclPort applied[5]{};
static unsigned writes=0;
static bool bad_readback=false,fail_restore=false;
static int fail_port=-1;
static int set_crop(uint32_t device,uint32_t channel,uint32_t port,SclPort *crop) {
    assert(!device && !channel);++writes;
    if(fail_restore && writes==2)return -1;
    if(int(port)==fail_port) { fail_port=-1;return -1; }
    applied[port]=*crop;return 0;
}
static int get_crop(uint32_t device,uint32_t channel,uint32_t port,SclPort *crop) {
    assert(!device && !channel);*crop=applied[port];
    if(bad_readback && writes==1)crop->crop.width=0;
    return 0;
}
int main() {
    OpticalZoom motor;
    assert(!motor.request(NAN,0) && !motor.request(.9f,0) && !motor.request(30.1f,0));
    assert(motor.request(2,0) && motor.wanted(NAN,45000)==0 && !motor.active());
    assert(motor.request(2,1000));
    assert(motor.wanted(NAN,1000)==0 && motor.active());
    assert(motor.wanted(1,1000)==1);motor.sent(1,1000);
    assert(motor.wanted(1.2f,1119)==1);
    assert(motor.wanted(1.3f,1120)==0);
    // A failed stop is retried before another pulse can be started.
    assert(motor.wanted(1.5f,1400)==0 && motor.current()==1);
    motor.sent(0,1400);assert(motor.wanted(1.5f,1539)==0);
    assert(motor.wanted(1.6f,1540)==1);motor.sent(1,1540);
    assert(motor.wanted(2.2f,1550)==0);motor.sent(0,1550);assert(!motor.active());
    assert(motor.request(1,2000) && motor.wanted(2,2000)==-1);motor.sent(-1,2000);
    assert(motor.wanted(NAN,2001)==0);motor.sent(0,2001);assert(!motor.active());
    assert(motor.request(30,3000) && motor.wanted(1,3000)==1);motor.sent(1,3000);
    assert(motor.wanted(2,48000)==0);motor.sent(0,48000);assert(!motor.active());
    assert(!motor.request(5,49000));motor.reset();
    assert(motor.request(5,49000));motor.cancel();assert(!motor.active() && !motor.wanted(1,49000));
    assert(motor.request(1,50000) && motor.wanted(1.1f,50000)==-1);motor.sent(-1,50000);
    assert(motor.wanted(1.1f,52000)==0);motor.sent(0,52000);assert(!motor.active());
    assert(motor.request(1.1f,53000) && motor.wanted(1,53000)==1);
    motor.sent(1,53000);assert(motor.wanted(1.1f,53020)==0);motor.sent(0,53020);
    // A stream of new targets cannot keep a jammed lens alive indefinitely.
    motor.reset();assert(motor.request(10,60000));
    for(uint64_t now=60000;now<62000;now+=50) {
        assert(motor.request(10+float(now-60000)/1000,now));
        const int next=motor.wanted(2,now);
        if(next!=motor.current())motor.sent(next,now);
    }
    assert(motor.wanted(2,62000)==0);motor.sent(0,62000);
    assert(!motor.active() && !motor.request(20,62050));
    assert(motor.request(1,62100));assert(motor.wanted(2,62200)==-1);motor.sent(-1,62200);
    // Direction reversal stops the old pulse before starting the new one.
    assert(motor.request(3,62201)==false); // stalled positive direction is latched
    assert(motor.wanted(1.9f,62202)==-1); // actual movement releases that latch
    assert(motor.request(3,62203) && motor.wanted(1.9f,62203)==0);
    motor.sent(0,62203);assert(motor.wanted(1.9f,62342)==0);
    assert(motor.wanted(1.9f,62343)==1);
    Rect crop{};
    for(float factor : {1.f,1.1f,1.5f,2.f,3.f,6.f}) {
        assert(zoom_crop(factor,crop));
        assert(!(crop.x&1) && !(crop.y&1) && !(crop.width&1) && !(crop.height&1));
        assert(crop.width*9==crop.height*16 && crop.x+crop.width<=3840 && crop.y+crop.height<=2160);
    }
    assert(zoom_crop(6,crop) && crop.width==640 && crop.height==360);
    assert(!zoom_crop(NAN,crop) && !zoom_crop(.5f,crop) && !zoom_crop(6.1f,crop));
    Api api{};api.scl_set_port=set_crop;api.scl_get_port=get_crop;DigitalZoom digital(api);
    for(unsigned p=0;p<5;p++) { applied[p].output={uint16_t(1280+p*32),720};applied[p].pixel_format=11; }
    assert(digital.set(2)==0 && digital.get()==2 && applied[0].crop.width==1920 && applied[0].crop.height==1080);
    for(unsigned p=0;p<5;p++)assert(applied[p].output.width==1280+p*32 && applied[p].pixel_format==11 &&
        applied[p].crop.width==((0x0b&(1U<<p))?1920:0));
    SclPort original[5];memcpy(original,applied,sizeof(original));fail_port=3;
    assert(digital.set(6)<0 && errno==EIO && !memcmp(original,applied,sizeof(original)) && digital.get()==2);
    SclPort before[5];memcpy(before,applied,sizeof(before));writes=0;bad_readback=true;
    assert(digital.set(3)<0 && errno==EIO && !memcmp(before,applied,sizeof(before)) && digital.get()==2);
    writes=0;fail_restore=true;
    assert(digital.set(3)<0 && errno==EOWNERDEAD);
    ZoomFeedback feedback;assert(feedback.observe(10,0,1000));feedback.digital(2);
    assert(feedback.get(1000)==10 && feedback.combined(1000)==20);
    assert(feedback.hfov(false,1000)<ca_zoom_lens_hfov(10,1));
    assert(!std::isfinite(feedback.combined(3501)) && feedback.hfov(false,3501)==0);
    puts("ZT30 optical pulse/stop/stale/timeout and digital crop/readback/rollback/FOV PASS");
}
