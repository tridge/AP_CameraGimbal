#include "../camera_app/src/backends/zt30/input_metadata.h"
#include "../camera_app/src/backends/zt30/media_graph.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace ca_zt30;
static std::vector<std::string> calls;
static const char *fail=nullptr,*cancel_at=nullptr,*slow_at=nullptr;
static uint64_t clock_ns=1000000000;
static bool cancelled=false;
static unsigned get_count=0,put_count=0;
static uint32_t returned_handle=64;
static InputBufferInfo returned_info{};
static InputMetadata *active=nullptr;
static bool reenter=false;
static int hit(const char *name) {
    calls.emplace_back(name);
    if(cancel_at && !std::strcmp(cancel_at,name))cancelled=true;
    if(slow_at && !std::strcmp(slow_at,name))clock_ns+=InputMetadata::BudgetNs;
    if(reenter && !std::strcmp(name,"get")) { const auto n=calls.size();assert(!active->run() && calls.size()==n); }
    return fail && !std::strcmp(fail,name) ? -99 : 0;
}
static void ids(uint32_t d,uint32_t c) { assert(d==0 && c==2); }
static InputMetadataApi api() {
    InputMetadataApi a{};
    a.sys_init=[](uint16_t c) { assert(c==0);return hit("init"); };
    a.sys_exit=[](uint16_t c) { assert(c==0);return hit("exit"); };
    a.create_device=[](uint32_t d,EncoderDevice *p) { assert(d==0 && p->max_width==640 && p->max_height==512);return hit("device"); };
    a.destroy_device=[](uint32_t d) { assert(d==0);return hit("undevice"); };
    a.create_channel=[](uint32_t d,uint32_t c,H26xChannel *p) {
        ids(d,c);auto expected=h264_channel(640,512,1570000);assert(!std::memcmp(p,&expected,sizeof(expected)));return hit("channel");
    };
    a.destroy_channel=[](uint32_t d,uint32_t c) { ids(d,c);return hit("unchannel"); };
    a.set_input_source=[](uint32_t d,uint32_t c,const uint32_t *p) { ids(d,c);assert(p && *p==0);return hit("source"); };
    a.start=[](uint32_t d,uint32_t c) { ids(d,c);return hit("start"); };
    a.stop=[](uint32_t d,uint32_t c) { ids(d,c);return hit("stop"); };
    a.buffer.get=[](InputPort *p,InputBufferConfig *c,InputBufferInfo *i,uint32_t *h,int32_t t) {
        assert(p->words[0]==2 && p->words[1]==0 && p->words[2]==2 && p->words[3]==0 && t==100);
        InputBufferConfig expected{};expected.type=1;expected.width=640;expected.height=512;expected.numeric_format=11;
        assert(!std::memcmp(c,&expected,sizeof(expected)));
        ++get_count;*i=returned_info;*h=returned_handle;return hit("get");
    };
    a.buffer.put=[](uint32_t h,InputBufferInfo *i,uint8_t f) {
        ++put_count;assert(h==returned_handle && f==1 && !std::memcmp(i,&returned_info,264));return hit("put");
    };
    return a;
}
static InputMetadataObserver observer() {
    InputMetadataObserver o{};
    o.now_ns=[](void*) { return clock_ns; };
    o.cancelled=[](void*) { return cancelled; };
    o.snapshot=[](void*,InputSnapshot phase,const InputBufferInfo *i,uint32_t h,uint64_t deadline) {
        assert(deadline>clock_ns && deadline-clock_ns<=InputMetadata::SnapshotNs);
        if(phase==InputSnapshot::Leased || phase==InputSnapshot::AfterPut)assert(i && h==returned_handle && !std::memcmp(i,&returned_info,264));
        else assert(!i && !h);
        const char *name=phase==InputSnapshot::BeforeGet ? "before" : phase==InputSnapshot::Leased ? "lease" : phase==InputSnapshot::AfterPut ? "after" : "idle";
        return hit(name)==0;
    };
    return o;
}
static void reset() {
    calls.clear();fail=cancel_at=slow_at=nullptr;clock_ns=1000000000;cancelled=false;
    get_count=put_count=0;returned_handle=64;reenter=false;
    // Includes deliberately unusable address/stride/size/type fields. The
    // discard-only path preserves them without treating them as pointers.
    std::memset(&returned_info,0xff,264);
}
static bool run() { InputMetadata x(api(),observer());active=&x;return x.run(); }
int main() {
    unsigned cases=0;
    const std::vector<std::string> all={"init","device","channel","source","start","before","get","lease","put","after","stop","unchannel","undevice","exit","idle"};
    reset();assert(run() && calls==all && get_count==1 && put_count==1);++cases;
    for(uint32_t h:{64U,8191U}) { reset();returned_handle=h;assert(run() && put_count==1);++cases; }
    for(uint32_t h:{0U,63U,8192U,UINT32_MAX}) { reset();returned_handle=h;assert(!run() && put_count==0 && calls.back()=="idle");++cases; }
    for(const char *name:{"init","device","channel","source","start","before","get","lease","put","after","stop","unchannel","undevice","exit","idle"}) {
        reset();fail=name;InputMetadata x(api(),observer());active=&x;assert(!x.run() && x.raw_status==(!std::strcmp(name,"before") || !std::strcmp(name,"lease") || !std::strcmp(name,"after") || !std::strcmp(name,"idle") ? -1 : -99));
        const size_t n=calls.size();assert(!x.run() && calls.size()==n);
        // No retry, never use failed Get outputs. Observer errors still discard.
        if(!std::strcmp(name,"get"))assert(get_count==1 && put_count==0);
        if(!std::strcmp(name,"lease"))assert(get_count==1 && put_count==1);
        if(!std::strcmp(name,"put"))assert(put_count==1 && calls.back()=="idle");
        if(!std::strcmp(name,"init"))assert(calls==std::vector<std::string>({"init","idle"}));
        if(!std::strcmp(name,"device"))assert(calls==std::vector<std::string>({"init","device","exit","idle"}));
        if(!std::strcmp(name,"channel"))assert(calls==std::vector<std::string>({"init","device","channel","undevice","exit","idle"}));
        if(!std::strcmp(name,"start"))assert(calls==std::vector<std::string>({"init","device","channel","source","start","stop","unchannel","undevice","exit","idle"}));
        ++cases;
    }
    for(const char *name:{"init","device","channel","source","start","before","get","lease","put","after","stop","unchannel","undevice","exit"}) {
        for(bool cancel:{false,true}) {
            reset();if(cancel)cancel_at=name;else slow_at=name;
            assert(!run());
            if(!std::strcmp(name,"get") || !std::strcmp(name,"lease"))assert(put_count==1);
            if(!std::strcmp(name,"init"))assert(calls==std::vector<std::string>({"init","exit"}));
            if(!std::strcmp(name,"stop"))assert(calls.back()=="exit");
            ++cases;
        }
    }
    reset();reenter=true;assert(run() && calls==all);++cases;
    reset();InputMetadata x(api(),observer());active=&x;assert(x.run());auto n=calls.size();assert(!x.run() && calls.size()==n);++cases;
    reset();auto a=api();a.buffer.put=nullptr;InputMetadata missing(a,observer());assert(!missing.run() && calls.empty());++cases;
    reset();clock_ns=0;assert(!run() && calls.empty());++cases;
    reset();clock_ns=UINT64_MAX;assert(!run() && calls.empty());++cases;
    std::printf("ZT30 input metadata private PASS cases=%u no-peripheral-access\n",cases);
}
