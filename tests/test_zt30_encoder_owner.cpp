#include "../camera_app/src/backends/zt30/encoder_owner.h"
#include <cassert>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace ca_zt30;
static std::vector<std::string> calls;
static std::set<std::string> resources;
static unsigned failure;
static bool thermal=false;
static unsigned thermal_channel=2;
static EncoderPacket *storage;
static EncoderStream returned;
static uint32_t query_count=2;
static int scenario;
static uint32_t expected_codec=3;
static uint32_t expected_source=0;
static uint32_t expected_channel=1,expected_port=1,expected_width=1280,expected_height=720,expected_bitrate=1570000;
static H264Vui current_vui{};
static unsigned vui_reads=0;
static const uint8_t sample[]={9,8,7,6,5,4};
static int event(const char *name) { calls.emplace_back(name);return failure==calls.size()?-99:0; }
static void ids(uint32_t dev,uint32_t chn) { assert(dev==0 && chn==(thermal?thermal_channel:expected_channel)); }
static Api system_api() {
    Api s{};
    s.sys_bind=[](uint16_t chip,Port *src,Port *dst,uint32_t a,uint32_t b,uint32_t type,uint32_t p) {
        assert(chip==0 && src->module==Scl && src->device==0 && src->channel==expected_source && src->port==expected_port);
        assert(dst->module==Venc && dst->device==0 && dst->channel==expected_channel && dst->port==0);
        assert(a==30 && b==30 && type==1 && p==0);
        const int rc=event("bind");if(!rc)resources.insert("bind");return rc;
    };
    s.sys_unbind=[](uint16_t chip,Port *src,Port *dst) {
        assert(chip==0 && src->module==Scl && src->device==0 && src->channel==expected_source && src->port==expected_port);
        assert(dst->module==Venc && dst->device==0 && dst->channel==expected_channel && dst->port==0);
        const int rc=event("unbind");resources.erase("bind");return rc;
    };
    return s;
}
static EncoderApi encoder_api() {
    EncoderApi a{};
    a.create_device=[](uint32_t dev,EncoderDevice *attr) {
        assert(dev==0 && attr->max_width==3840 && attr->max_height==2160);
        const int rc=event("create_device");if(!rc)resources.insert("device");return rc;
    };
    a.destroy_device=[](uint32_t dev) { assert(dev==0);const int rc=event("destroy_device");resources.erase("device");return rc; };
    a.create_channel=[](uint32_t dev,uint32_t chn,H26xChannel *attr) {
        ids(dev,chn);assert(attr->codec==expected_codec && attr->rate_mode==(expected_codec==2?1U:8U) && attr->profile==(expected_codec==2?1U:0U) && attr->width==(thermal?640U:expected_width) && attr->height==(thermal?512U:expected_height) && attr->fps_num==(thermal?15U:30U) && attr->fps_den==1 && attr->bitrate==(thermal?1000000U:expected_bitrate));
        assert(attr->max_width==(thermal?640U:expected_codec==2?expected_width:3840U) && attr->max_height==(thermal?512U:expected_codec==2?expected_height:2160U));
        assert(attr->reference_frames==(expected_codec==2?1U:0U));
        const int rc=event("create_channel");if(!rc)resources.insert("channel");return rc;
    };
    a.destroy_channel=[](uint32_t dev,uint32_t chn) { ids(dev,chn);const int rc=event("destroy_channel");resources.erase("channel");return rc; };
    a.set_input_source=[](uint32_t dev,uint32_t chn,const uint32_t *input) { ids(dev,chn);assert(input && !*input);return event("input"); };
    a.get_h264_vui=[](uint32_t dev,uint32_t chn,H264Vui *vui) {
        ids(dev,chn);assert(expected_codec==2);const int rc=event("vui_get");if(rc)return rc;
        *vui=current_vui;vui_reads++;
        if(vui_reads==1 && scenario==18)vui->units=42;
        if(vui_reads==2) {
            if(scenario==13)vui->timing_present=0;
            if(scenario==14)vui->fixed_rate=0;
            if(scenario==15)vui->units=2;
            if(scenario==16)vui->time_scale=59;
            if(scenario==19)vui->full_range=0;
            if(scenario==17) { vui->reserved[0]++;vui->reserved[1]++;vui->reserved_tail++; }
        }
        return 0;
    };
    a.set_h264_vui=[](uint32_t dev,uint32_t chn,const H264Vui *vui) {
        ids(dev,chn);assert(expected_codec==2 && vui_reads==1);
        auto expected=current_vui;expected.timing_present=1;expected.fixed_rate=1;expected.units=1;expected.time_scale=60;
        assert(!std::memcmp(&expected,vui,sizeof(expected)));
        const int rc=event("vui_set");if(!rc)current_vui=*vui;return rc;
    };
    a.start=[](uint32_t dev,uint32_t chn) { ids(dev,chn);const int rc=event("start");resources.insert("start");return rc; };
    a.stop=[](uint32_t dev,uint32_t chn) { ids(dev,chn);const int rc=event("stop");resources.erase("start");return rc; };
    a.query=[](uint32_t dev,uint32_t chn,EncoderStatus *status) { ids(dev,chn);const int rc=event("query");status->current_packs=query_count;return rc; };
    a.get_stream=[](uint32_t dev,uint32_t chn,EncoderStream *stream,int32_t timeout) {
        ids(dev,chn);assert(timeout==100 && stream->packets_address==1234 && stream->count==query_count);
        const int rc=event("get");if(rc)return rc;
        for(uint32_t i=0;i<query_count;i++) { storage[i]={};storage[i].data_address=1000+i*100;storage[i].length=6;storage[i].offset=i+1;storage[i].pts=1000000; }
        storage[query_count-1].frame_flags=1;
        stream->sequence=17;stream->handle=5678;stream->codec_information[13]=42;
        if(scenario==1)storage[0].offset=7;
        if(scenario==2)storage[0].data_address=UINT32_MAX-2;
        if(scenario==3)storage[0].length=4*1024*1024+1;
        if(scenario==4)stream->count=33;
        if(scenario==5)stream->packets_address=999;
        if(scenario==6)stream->count=0;
        if(scenario==7)storage[0].data_address=0;
        if(scenario==11)storage[query_count-1].frame_flags=0;
        if(scenario==12)storage[0].frame_flags=1;
        if(scenario==10) { storage[0].frame_flags=1;storage[1]={}; } // end-of-frame before Query capacity
        returned=*stream;resources.insert("stream");return 0;
    };
    a.release_stream=[](uint32_t dev,uint32_t chn,EncoderStream *stream) {
        ids(dev,chn);
        auto expected=returned;if(scenario==10)expected.count=1;
        assert(!std::memcmp(stream,&expected,sizeof(expected)));
        for(unsigned i=0;i<stream->count;i++)assert(storage[i].data_address);
        const int rc=event("release");resources.erase("stream");
        std::memset(storage,0,32*sizeof(*storage)); // invalidate all borrowed descriptors
        return rc;
    };
    return a;
}
static PacketMemory memory_api() {
    PacketMemory m{};
    m.pointer_slot=[](const void *p,uint32_t *slot) { storage=(EncoderPacket*)p;*slot=1234;return scenario!=8; };
    m.copy=[](uint32_t address,uint8_t *out,size_t count) {
        event("copy");assert(resources.count("stream"));
        if(scenario==9)return false;
        const unsigned offset=(address-1000)%100;
        assert(count<=6 && offset+count<=6);std::memcpy(out,sample+offset,count);return true;
    };
    return m;
}
static void reset() {
    calls.clear();resources.clear();failure=0;query_count=2;scenario=0;vui_reads=0;
    std::memset(&current_vui,0x5a,sizeof(current_vui));
    current_vui.timing_present=1;current_vui.fixed_rate=0;current_vui.units=1001;current_vui.time_scale=60060;
}
static void exercise(EncoderCodec codec) {
    expected_codec=uint32_t(codec);
    auto s=system_api();auto a=encoder_api();auto m=memory_api();
    uint8_t data[64]{};EncodedFrame frame{};frame.data=data;frame.capacity=sizeof(data);
    reset();EncoderOwner ok(s,a,m);assert(ok.copy_frame(frame)==-1 && calls.empty());
    assert(ok.open(codec,H264Timing::Fixed30)==0 && ok.ready());const auto startup=calls.size();
    assert(vui_reads==(codec==EncoderCodec::H264?2U:0U));
    if(codec==EncoderCodec::H265)assert(!ok.have_default_vui());
    else assert(ok.have_default_vui() && ok.applied_vui().fixed_rate==1);
    assert(ok.open(codec,H264Timing::Fixed30)==-1 && calls.size()==startup);
    assert(ok.copy_frame(frame)==1 && frame.bytes==9 && frame.packets==2 && frame.sequence==17 && frame.pts[0]==1000000 && frame.pts[1]==1000000);
    const uint8_t expected[]={8,7,6,5,4,7,6,5,4};assert(!std::memcmp(data,expected,9));
    assert(calls.back()=="release" && !resources.count("stream"));
    query_count=0;assert(ok.copy_frame(frame)==0 && frame.bytes==0 && frame.packets==0);
    assert(ok.close()==0 && resources.empty());const auto n=calls.size();assert(ok.close()==0 && calls.size()==n && ok.open(codec,H264Timing::Fixed30)==-1);
    for(unsigned step=1;step<=startup;step++) {
        reset();failure=step;EncoderOwner owner(s,a,m);assert(owner.open(codec,H264Timing::Fixed30)==-99 && !owner.ready() && resources.empty());
        if(codec==EncoderCodec::H264 && step>=4 && step<=6)
            for(const auto &call:calls)assert(call!="stop");
        const auto count=calls.size();assert(owner.open(codec,H264Timing::Fixed30)==-1 && calls.size()==count && owner.close()==0);
    }
    for(unsigned step=1;step<=4;step++) {
        reset();EncoderOwner owner(s,a,m);assert(owner.open(codec,H264Timing::Fixed30)==0);failure=calls.size()+step;
        assert(owner.close()==-99 && calls.back()=="destroy_device" && resources.empty() && !owner.ready());
    }
    for(int value=1;value<=9;value++) {
        reset();EncoderOwner owner(s,a,m);assert(owner.open(codec,H264Timing::Fixed30)==0);scenario=value;
        assert(owner.copy_frame(frame)==-1 && frame.bytes==0 && !owner.ready());
        if(value==4 || value==5 || value==6 || value==7)assert(calls.back()=="get"); // malformed array cannot be released
        else if(value!=8)assert(calls.back()=="release");
        assert(owner.close()==0);resources.erase("stream");assert(resources.empty());
    }
    reset();EncoderOwner short_fill(s,a,m);assert(short_fill.open(codec,H264Timing::Fixed30)==0);scenario=10;
    assert(short_fill.copy_frame(frame)==1 && frame.packets==1 && frame.bytes==5 && calls.back()=="release");
    assert(short_fill.close()==0 && resources.empty());
    for(int value:{11,12}) {
        reset();EncoderOwner owner(s,a,m);assert(owner.open(codec,H264Timing::Fixed30)==0);scenario=value;
        assert(owner.copy_frame(frame)==-1 && frame.bytes==0 && calls.back()=="release");
        assert(owner.close()==0 && resources.empty());
    }
    if(codec==EncoderCodec::H264) {
        for(int value:{13,14,15,16,18,19}) {
            reset();scenario=value;EncoderOwner owner(s,a,m);
            assert(owner.open(codec,H264Timing::Fixed30)==-1 && !owner.ready() && resources.empty());
            for(const auto &call:calls)assert(call!="start" && call!="stop");
            if(value==18)for(const auto &call:calls)assert(call!="vui_set");
            const auto n=calls.size();assert(owner.open(codec,H264Timing::Fixed30)==-1 && calls.size()==n);
        }
        reset();scenario=17;EncoderOwner padding(s,a,m);
        assert(padding.open(codec,H264Timing::Fixed30)==0 && padding.close()==0 && resources.empty());
        for(unsigned missing:{0U,1U}) {
            reset();auto incomplete=a;
            if(missing)incomplete.set_h264_vui=nullptr;else incomplete.get_h264_vui=nullptr;
            EncoderOwner owner(s,incomplete,m);assert(owner.open(codec,H264Timing::Fixed30)==-1 && calls.empty() && resources.empty());
        }
    }
    reset();EncoderOwner overflow(s,a,m);assert(overflow.open(codec,H264Timing::Fixed30)==0);query_count=33;
    assert(overflow.copy_frame(frame)==-1 && calls.back()=="query");assert(overflow.close()==0);
    reset();EncoderOwner small(s,a,m);assert(small.open(codec,H264Timing::Fixed30)==0);frame.capacity=4;
    assert(small.copy_frame(frame)==-1 && calls.back()=="release" && frame.bytes==0);assert(small.close()==0);frame.capacity=64;
    for(unsigned step:{1U,2U,5U}) { // Query, GetStream, ReleaseStream error
        reset();EncoderOwner owner(s,a,m);assert(owner.open(codec,H264Timing::Fixed30)==0);failure=calls.size()+step;
        assert(owner.copy_frame(frame)==-1 && frame.bytes==0 && !owner.ready());assert(owner.close()==0 && resources.empty());
    }
}

static void exercise_read_only() {
    expected_codec=2;
    auto s=system_api();auto a=encoder_api();auto m=memory_api();
    a.set_h264_vui=nullptr;
    reset();current_vui.units=0;current_vui.time_scale=0;
    EncoderOwner owner(s,a,m);
    assert(owner.open(EncoderCodec::H264)==0 && owner.ready());
    H264Vui output{};
    assert(owner.read_h264_vui(output)==0 && output.units==0 && output.time_scale==0);
    uint8_t data[64]{};EncodedFrame frame{};frame.data=data;frame.capacity=sizeof(data);
    assert(owner.copy_frame(frame)==1 && !resources.count("stream"));
    assert(owner.read_h264_vui(output)==0);
    for(const auto &call:calls)assert(call!="vui_set");
    failure=calls.size()+1;
    assert(owner.read_h264_vui(output)==-99 && !owner.ready() && output.timing_present==0);
    assert(owner.close()==0 && resources.empty());
    reset();a.get_h264_vui=nullptr;EncoderOwner missing(s,a,m);
    assert(missing.open(EncoderCodec::H264,H264Timing::ReadOnly)==-1 && calls.empty());
}

static void configurable_outputs() {
    auto s=system_api();auto a=encoder_api();auto m=memory_api();
    for(auto codec:{EncoderCodec::H264,EncoderCodec::H265})for(unsigned width:{1920U,2560U,3840U}) {
        reset();expected_codec=uint32_t(codec);expected_channel=7;expected_port=3;
        expected_width=width;expected_height=width*9/16;expected_bitrate=12000000;
        EncoderOptions options;options.width=expected_width;options.height=expected_height;
        options.bitrate=expected_bitrate;options.channel=expected_channel;options.source_port=expected_port;options.own_device=false;
        EncoderOwner output(s,a,m);assert(output.open(codec,H264Timing::ReadOnly,EncoderSource::Zoom,options)==0);
        assert(output.ready() && !resources.count("device"));assert(output.close()==0 && resources.empty());
        for(const auto &call:calls)assert(call!="create_device" && call!="destroy_device");
    }
    expected_channel=expected_port=1;expected_width=1280;expected_height=720;expected_bitrate=1570000;
}
static int bound_source=-1;
static bool fail_switch=false,fail_restore=false;
static void source_switching() {
    reset();expected_codec=2;expected_source=0;
    auto sys=system_api();auto api=encoder_api();auto memory=memory_api();
    sys.sys_bind=[](uint16_t chip,Port *source,Port *dest,uint32_t a,uint32_t b,uint32_t type,uint32_t flags) {
        assert(!chip && source->module==Scl && source->channel<=1 && source->port==1 &&
            dest->module==Venc && dest->channel==1 && a==30 && b==30 && type==FrameBind && !flags);
        if((source->channel==1 && fail_switch) || (source->channel==0 && fail_restore))return -77;
        bound_source=int(source->channel);resources.insert("bind");return 0;
    };
    sys.sys_unbind=[](uint16_t,Port *source,Port *) {
        assert(bound_source<0 || int(source->channel)==bound_source);
        bound_source=-1;resources.erase("bind");return 0;
    };
    EncoderOwner owner(sys,api,memory);
    assert(owner.open(EncoderCodec::H264,H264Timing::ReadOnly)==0 && bound_source==0);
    assert(owner.rebind(EncoderSource::Wide)==0 && bound_source==1);
    assert(owner.rebind(EncoderSource::Wide)==0 && bound_source==1);
    assert(owner.rebind(EncoderSource::Zoom)==0 && bound_source==0);
    fail_switch=true;
    assert(owner.rebind(EncoderSource::Wide)==-77 && bound_source==0 && owner.ready());
    fail_restore=true;
    assert(owner.rebind(EncoderSource::Wide)==EOWNERDEAD && bound_source==-1);
    // Native caller terminates on ambiguous ownership; the fake can still unwind.
    fail_switch=fail_restore=false;
    assert(owner.close()==0 && resources.empty());
}
int main() {
    source_switching();
    configurable_outputs();
    exercise_read_only();
    exercise(EncoderCodec::H265);exercise(EncoderCodec::H264);
    reset();auto s=system_api();auto a=encoder_api();auto m=memory_api();
    EncoderOwner invalid(s,a,m);assert(invalid.open(static_cast<EncoderCodec>(99))==-1);
    assert(calls.empty() && resources.empty() && !invalid.ready());
    expected_source=1;
    // Every startup failure must unwind the selected source, including a
    // partially started encoder; success must unbind Wide rather than Zoom.
    for(auto codec:{EncoderCodec::H265,EncoderCodec::H264}) {
        expected_codec=uint32_t(codec);
        for(unsigned step=0;step<=(codec==EncoderCodec::H264?6U:5U);step++) {
            reset();failure=step;EncoderOwner wide(s,a,m);
            const int rc=wide.open(codec,H264Timing::ReadOnly,EncoderSource::Wide);
            assert(rc==(step?-99:0));
            if(!step)assert(wide.ready());
            for(const auto &call:calls)assert(call!="vui_set");
            assert(wide.close()==0 && resources.empty());
        }
    }
    reset();EncoderOwner bad_source(s,a,m);
    assert(bad_source.open(EncoderCodec::H265,H264Timing::ReadOnly,static_cast<EncoderSource>(99))==-1);
    assert(calls.empty() && resources.empty() && !bad_source.ready());
    thermal=true;expected_codec=2;
    for(unsigned ch:{2U,6U,8U})for(unsigned step=0;step<=3;step++) {
        thermal_channel=ch;
        const auto codec=ch==8?EncoderCodec::H265:EncoderCodec::H264;
        expected_codec=uint32_t(codec);
        reset();failure=step;EncoderOwner th(s,a,m);
        assert(th.open_thermal(ch,codec)==(step?-99:0));
        if(!step) {
            assert(th.ready());uint8_t data[64]{};EncodedFrame frame{};frame.data=data;frame.capacity=64;
            assert(th.copy_frame(frame)==1 && frame.bytes==9);
        }
        assert(th.close()==0 && resources.empty());
        for(const auto &call:calls)assert(call!="create_device" && call!="destroy_device" && call!="bind" && call!="unbind" && call!="vui_set");
    }
    thermal_channel=2;expected_codec=2;
    for(unsigned step=1;step<=2;step++) {
        reset();EncoderOwner th(s,a,m);assert(th.open_thermal()==0);failure=calls.size()+step;
        assert(th.close()==-99 && resources.empty());
    }
    return 0;
}
