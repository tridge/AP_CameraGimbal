#include "input_metadata.h"
#include "media_graph.h"
namespace ca_zt30 {
bool InputMetadata::check(int rc,const char *name) {
    if(o.event)o.event(o.context,name,rc);
    if(rc && !failure) { failure=name;raw_status=rc; }
    return rc==0;
}
bool InputMetadata::within_budget() {
    const uint64_t current=o.now_ns(o.context);
    if(!current || current<begin || current>=deadline || o.cancelled(o.context)) {
        if(!failure) { failure="deadline/cancel";raw_status=-1; }
        return false;
    }
    return true;
}
bool InputMetadata::observe(InputSnapshot phase,const InputBufferInfo *info,uint32_t handle) {
    if(!within_budget())return false;
    const uint64_t start=o.now_ns(o.context);
    if(!start || start<begin || start>=deadline)return check(-1,"snapshot clock");
    const uint64_t end=deadline-start<SnapshotNs ? deadline : start+SnapshotNs;
    const bool ok=o.snapshot(o.context,phase,info,handle,end);
    const uint64_t finish=o.now_ns(o.context);
    return check(ok && finish>=start && finish<end ? 0 : -1,"snapshot") && within_budget();
}
bool InputMetadata::run() {
    if(used)return false;
    used=true;
    if(!a.sys_init || !a.sys_exit || !a.create_device || !a.destroy_device ||
       !a.create_channel || !a.destroy_channel || !a.set_input_source ||
       !a.start || !a.stop || !a.buffer.get || !a.buffer.put || !o.now_ns ||
       !o.cancelled || !o.snapshot) { failure="missing API";raw_status=-1;return false; }
    begin=o.now_ns(o.context);
    if(!begin || begin>UINT64_MAX-BudgetNs) { failure="invalid clock";raw_status=-1;return false; }
    deadline=begin+BudgetNs;
    bool sys=false,device=false,channel=false,start_attempted=false;
    // Stage flags follow successful creation; Stop covers partial Start only
    // on our successfully created channel. Never destroy an unowned resource.
    do {
        if(!within_budget() || !check(a.sys_init(0),"SYS Init"))break;
        sys=true;
        EncoderDevice dev{640,512};
        if(!within_budget() || !check(a.create_device(0,&dev),"CreateDev"))break;
        device=true;
        auto attr=h264_channel(640,512,1570000);
        if(!within_budget() || !check(a.create_channel(0,2,&attr),"CreateChn"))break;
        channel=true;
        const uint32_t source=0;
        if(!within_budget() || !check(a.set_input_source(0,2,&source),"SetInputSource"))break;
        if(!within_budget())break;
        start_attempted=true;
        if(!check(a.start(0,2),"StartRecvPic") || !within_budget())break;
        if(!observe(InputSnapshot::BeforeGet,nullptr,0))break;
        InputPort port{{2,0,2,0}};
        InputBufferConfig config{};
        config.type=1;config.width=640;config.height=512;config.numeric_format=11;
        // opaque8 zero for discard-only inspection; not an encoder PTS claim.
        InputBufferInfo info{};uint32_t handle=0;
        if(!within_budget() || !check(a.buffer.get(&port,&config,&info,&handle,100),"InputGet"))break;
        if(handle<64 || handle>8191) { check(-1,"invalid successful handle");break; }
        // On cancellation, deadline, or snapshot failure, Put still runs once.
        (void)observe(InputSnapshot::Leased,&info,handle);
        if(!check(a.buffer.put(handle,&info,1),"InputPut discard"))break;
        (void)observe(InputSnapshot::AfterPut,&info,handle);
    } while(false);
    // Cleanup attempts all owned stages even on timeout/earlier cleanup error.
    // No retries: a failed cleanup is evidence of failure, not success.
#define CLEAN(flag,call,name) if(flag) { const int rc=(call);if(rc && !cleanup_status)cleanup_status=rc;check(rc,name);flag=false; }
    CLEAN(start_attempted,a.stop(0,2),"StopRecvPic");
    CLEAN(channel,a.destroy_channel(0,2),"DestroyChn");
    CLEAN(device,a.destroy_device(0),"DestroyDev");
    CLEAN(sys,a.sys_exit(0),"SYS Exit");
#undef CLEAN
    (void)observe(InputSnapshot::AfterExit,nullptr,0);
    return !failure && within_budget();
}
} // namespace ca_zt30
