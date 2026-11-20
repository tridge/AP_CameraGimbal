#include "../camera_app/src/backends/zt30/owned_media.h"
#include "../camera_app/src/backends/zt30/ram_protocol.h"
#include <cassert>
#include <initializer_list>
#include <vector>
#include <set>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
using namespace ca_zt30;
int interface_checks() {
    // All function pointers null: construction/getters/unsupported operations
    // and rejected initialization/destruction must make no SDK calls.
    Api api{};EncoderApi enc{};PacketMemory mem{};
    MediaBackend backend(api,enc,mem);
    APC_Media_Backend *view=&backend;
    assert(!view->ready() && !view->tracking_available() && !view->recording());
    assert(!view->recording_path() && !view->thermal_main() && !view->side_by_side());
    assert(!std::isfinite(view->zoom()) && !std::isfinite(view->lens_zoom(CA_MEDIA_LENS_ZOOM)));
    assert(view->hfov(false)==0 && view->hfov(true)==0 && view->frame_rate(false)==30 && view->frame_rate(true)==0);
    assert(view->lens()==CA_MEDIA_LENS_ZOOM);
    for(int rc:{view->set_zoom(2),view->set_lens(CA_MEDIA_LENS_WIDE),view->set_recording(true),view->autofocus(1,1),view->manual_focus(1),view->capture_photo(CA_PHOTO_SCOPE_ALL),view->set_thermal_main(true),view->set_side_by_side(false),view->apply_overlay(nullptr),view->apply_image(nullptr),view->configure_raw_thermal(nullptr)})assert(rc==-1);
    assert(errno==ENOTSUP);
    assert(view->set_lens(CA_MEDIA_LENS_ZOOM)==0 && !view->ready());
    MediaHooks hooks{};
    assert(backend.initialize(hooks,nullptr,0)==-1 && errno==EINVAL && !backend.ready());
    assert(backend.initialize(hooks,nullptr,0)==-1 && errno==EALREADY);
    assert(backend.shutdown()==0 && backend.shutdown()==0);
    MediaBackend invalid(api,enc,mem);
    MediaHooks valid{};
    valid.gate=[](void*,bool){return true;};valid.open_sink=[](void*){return true;};
    valid.vui=[](void*,const char*,const H264Vui&){return true;};
    valid.frame=[](void*,const EncodedFrame&,uint64_t,uint64_t){return true;};valid.drain=[](void*){return true;};
    std::vector<uint8_t> buffer(4U*1024*1024);
    assert(invalid.initialize(valid,buffer.data(),buffer.size(),static_cast<EncoderSource>(99))==-1 && errno==EINVAL);
    assert(invalid.shutdown()==0);
    char id[16]="CAM_LENS";
    assert(ram_lens_read(-1,id) && !ram_lens_read(0,id));
    id[15]='x';assert(!ram_lens_read(-1,id));
    std::memset(id,0,sizeof(id));assert(!ram_lens_read(-1,id));
    std::memcpy(id,"CAM_ZOOM",8);assert(!ram_lens_read(-1,id));
    return 0;
}

// Reuse the independently asserted resource models, in separate namespaces.
#define main rgb_resource_checks
namespace rgb_fixture {
#include "test_zt30_rgb_owner.cpp"
}
#undef main
#define main encoder_resource_checks
namespace encoder_fixture {
#include "test_zt30_encoder_owner.cpp"
}
#undef main
static bool fail_thread_create=false,fail_active_get=false;
extern "C" int __real_pthread_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
extern "C" int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void*(*f)(void*),void *p) {
    return fail_thread_create?EAGAIN:__real_pthread_create(t,a,f,p);
}
static Api rgb_api=rgb_fixture::fake_api();
static EncoderApi encoder_api=encoder_fixture::encoder_api();
struct TestSink {
    int failure=0;unsigned frames=0,gates=0,vuis=0;bool drained=false,continuous=false;volatile sig_atomic_t stop=0;
    static bool gate(void *p,bool after) { auto &s=*static_cast<TestSink*>(p);assert(after==(s.gates==1));s.gates++;if(!after && s.failure==7)rgb_fixture::fail_index=rgb_fixture::calls.size()+1;return s.failure!=(after?8:6); }
    static bool open(void *p) { return static_cast<TestSink*>(p)->failure!=26; }
    static bool vui(void *p,const char *phase,const H264Vui&) {
        auto &s=*static_cast<TestSink*>(p);const char *names[]={"pre-start","started","first-frame"};
        assert(s.vuis<3 && !std::strcmp(phase,names[s.vuis++]));return s.failure!=21;
    }
    static bool frame(void *p,const EncodedFrame &f,uint64_t before,uint64_t after) {
        auto &s=*static_cast<TestSink*>(p);assert(!encoder_fixture::resources.count("stream"));
        assert(encoder_fixture::resources.count("start") && f.bytes==9 && f.packets==2 && after>=before);s.frames++;if(s.failure==24 || (s.continuous && s.frames==135))s.stop=1;return s.failure!=27;
    }
    static bool drain(void *p) { auto &s=*static_cast<TestSink*>(p);assert(s.frames==(s.continuous?135U:90U));s.drained=true;return s.failure!=28; }
};
static void lifecycle_checks(int failure,bool wide=false,MediaRun run=MediaRun::Qualification) {
    rgb_fixture::reset(wide);encoder_fixture::reset();encoder_fixture::expected_codec=2;encoder_fixture::expected_source=wide?1:0;
    Api api=rgb_api;
    api.sys_bind=[](uint16_t chip,Port *src,Port *dst,uint32_t a,uint32_t b,uint32_t type,uint32_t arg) {
        return (dst->module==Venc?encoder_fixture::system_api():rgb_api).sys_bind(chip,src,dst,a,b,type,arg);
    };
    api.sys_unbind=[](uint16_t chip,Port *src,Port *dst) {
        return (dst->module==Venc?encoder_fixture::system_api():rgb_api).sys_unbind(chip,src,dst);
    };
    auto enc=encoder_api;enc.set_h264_vui=nullptr;
    enc.get_h264_vui=[](uint32_t dev,uint32_t chn,H264Vui *v) {
        if(fail_active_get && encoder_fixture::vui_reads==2)return -99;
        return encoder_api.get_h264_vui(dev,chn,v);
    };
    fail_thread_create=failure==29;fail_active_get=failure==22;
    if(failure==122)enc.query=[](uint32_t,uint32_t,EncoderStatus*) { return -77; };
    if(failure==124)enc.query=[](uint32_t,uint32_t,EncoderStatus *out) { *out={};return 0; };
    auto memory=encoder_fixture::memory_api();std::vector<uint8_t> buffer(4U*1024*1024);
    TestSink sink{};sink.failure=failure;sink.continuous=run==MediaRun::Continuous;
    MediaHooks hooks{};hooks.opaque=&sink;hooks.stop=&sink.stop;hooks.gate=TestSink::gate;hooks.open_sink=TestSink::open;hooks.vui=TestSink::vui;hooks.frame=TestSink::frame;hooks.drain=TestSink::drain;
    MediaBackend backend(api,enc,memory);
    const int rc=backend.initialize(hooks,buffer.data(),buffer.size(),wide?EncoderSource::Wide:EncoderSource::Zoom,run);
    assert(backend.lens()==(wide?CA_MEDIA_LENS_WIDE:CA_MEDIA_LENS_ZOOM));
    assert(backend.set_lens(backend.lens())==0);
    assert(backend.set_lens(wide?CA_MEDIA_LENS_ZOOM:CA_MEDIA_LENS_WIDE)==-1 && errno==ENOTSUP);
    if(failure==6 || failure==7 || failure==8 || failure==21 || failure==26 || failure==29)assert(rc==-1 && !backend.ready());
    else {
        assert(rc==0);
        for(unsigned i=0;i<3000 && !backend.finished();i++)usleep(1000);
        assert(backend.finished() && !backend.ready());
    }
    assert(backend.result()==(failure==122?22:failure==124?24:failure));
    assert(backend.shutdown()==0 && !backend.ready());
    assert(rgb_fixture::resources.empty() && encoder_fixture::resources.empty());
    for(const auto &call:encoder_fixture::calls)assert(call!="vui_set");
    if(!failure)assert(backend.frames()==(sink.continuous?135U:90U) && backend.bytes()==(sink.continuous?1215U:810U) && sink.drained);
    if(failure==22)assert(backend.raw_status()==-99 && !std::strcmp(backend.failed_call(),"a.get_h264_vui active"));
    const auto count=rgb_fixture::calls.size()+encoder_fixture::calls.size();
    assert(backend.shutdown()==0 && rgb_fixture::calls.size()+encoder_fixture::calls.size()==count);
}
static void poisoned_startup_check(int failure,bool wide=false) {
    const pid_t child=fork();assert(child>=0);
    if(!child) {
        rgb_fixture::reset(wide);encoder_fixture::reset();encoder_fixture::expected_codec=2;encoder_fixture::expected_source=wide?1:0;
        if(failure==21)encoder_fixture::failure=2; // failed CreateChn, encoder open already unwinds
        if(failure==5)rgb_fixture::fail_index=1; // failed SYS initialization
        auto api=rgb_fixture::fake_api();auto enc=encoder_fixture::encoder_api();auto memory=encoder_fixture::memory_api();
        std::vector<uint8_t> buffer(4U*1024*1024);TestSink sink{};sink.failure=failure;
        MediaHooks hooks{};hooks.opaque=&sink;hooks.gate=TestSink::gate;hooks.open_sink=TestSink::open;hooks.vui=TestSink::vui;hooks.frame=TestSink::frame;hooks.drain=TestSink::drain;
        MediaBackend backend(api,enc,memory);
        assert(backend.initialize(hooks,buffer.data(),buffer.size(),wide?EncoderSource::Wide:EncoderSource::Zoom)==-1);
        assert(backend.result()==failure && backend.shutdown()==EOWNERDEAD && !backend.ready());
        assert(encoder_fixture::resources.empty());
        if(failure!=5) {
            assert(!rgb_fixture::resources.empty());
            for(const auto &call:rgb_fixture::calls)assert(call!="sys_exit" && call!="snr_disable");
        }
        const auto calls=rgb_fixture::calls.size()+encoder_fixture::calls.size();
        assert(backend.shutdown()==EOWNERDEAD);
        assert(rgb_fixture::calls.size()+encoder_fixture::calls.size()==calls);
        _exit(0); // real harness also exits without RGB/SDK teardown
    }
    int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}

static bool context_destroyed=false;
struct TestContext {
    Api api=rgb_fixture::fake_api();
    EncoderApi encoder=encoder_fixture::encoder_api();
    PacketMemory memory=encoder_fixture::memory_api();
    ~TestContext() {
        // The same first-base ownership wrapper as the RAM factory must let
        // MediaBackend join/close before the SDK context is destroyed.
        assert(rgb_fixture::resources.empty() && encoder_fixture::resources.empty());
        context_destroyed=true;
    }
};
static void owned_context_lifetime_check(bool wide=false) {
    fail_thread_create=false;fail_active_get=false;
    rgb_fixture::reset(wide);encoder_fixture::reset();encoder_fixture::expected_codec=2;encoder_fixture::expected_source=wide?1:0;context_destroyed=false;
    auto c=std::unique_ptr<TestContext>(new TestContext);
    c->api.sys_bind=[](uint16_t chip,Port *src,Port *dst,uint32_t a,uint32_t b,uint32_t type,uint32_t arg) {
        return (dst->module==Venc?encoder_fixture::system_api():rgb_api).sys_bind(chip,src,dst,a,b,type,arg);
    };
    c->api.sys_unbind=[](uint16_t chip,Port *src,Port *dst) {
        return (dst->module==Venc?encoder_fixture::system_api():rgb_api).sys_unbind(chip,src,dst);
    };
    std::vector<uint8_t> buffer(4U*1024*1024);TestSink sink{};
    MediaHooks hooks{};hooks.opaque=&sink;hooks.stop=&sink.stop;hooks.gate=TestSink::gate;hooks.open_sink=TestSink::open;hooks.vui=TestSink::vui;hooks.frame=TestSink::frame;hooks.drain=TestSink::drain;
    {
        OwnedMediaBackend<TestContext> backend(std::move(c));
        assert(backend.initialize(hooks,buffer.data(),buffer.size(),wide?EncoderSource::Wide:EncoderSource::Zoom)==0);
        // Destruction deliberately races the bounded worker; it must join
        // before releasing resources even when capture hasn't completed.
        assert(!context_destroyed);
    }
    assert(context_destroyed);
}

int main() {
    interface_checks();
    for(bool wide:{false,true}) {
        lifecycle_checks(0,wide,MediaRun::Continuous);
        lifecycle_checks(27,wide,MediaRun::Continuous);
        lifecycle_checks(28,wide,MediaRun::Continuous);
        lifecycle_checks(122,wide,MediaRun::Continuous);
        lifecycle_checks(124,wide,MediaRun::Continuous);
        owned_context_lifetime_check(wide);
        for(int failure:{5,7,21})poisoned_startup_check(failure,wide);
        for(int failure:{0,6,8,21,22,24,26,27,28,29})lifecycle_checks(failure,wide);
    }
}
