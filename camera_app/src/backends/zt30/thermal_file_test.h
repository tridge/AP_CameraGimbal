#pragma once
#include "thermal_encoder.h"
#include "mi/native_sdk.h"
#include "camera_app/log.h"
#include <atomic>
#include <memory>
#include <new>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <ctime>
#include <unistd.h>
namespace ca_zt30 {
// Default-off development qualification: eight frames, one attempt per input
// path in tmpfs. No published thermal capability or persistent config change.
class ThermalFileTest {
public:
    ThermalFileTest(NativeSdk &sdk,PacketMemory &packets):sdk(sdk),encoder(sdk.api,sdk.encoder,packets,sdk.input) {}
    ~ThermalFileTest() { join(); }
    bool start(const char *path) {
        const char prefix[]="/tmp/apcg-zt30-thermal/";
        if(!path || std::strncmp(path,prefix,sizeof(prefix)-1) || std::strlen(path)>180)return false;
        const char *name=path+sizeof(prefix)-1;
        if(!*name)return false;
        for(const char *p=name;*p;++p)if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-' || *p=='.'))return false;
        if(std::strstr(name,".."))return false;
        input=::open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        struct stat st{};
        if(input<0 || fstat(input,&st) || !S_ISREG(st.st_mode) || st.st_uid || st.st_size!=8*ThermalNv12Bytes)return false;
        char file[224];std::snprintf(file,sizeof(file),"%s.started",path);
        int once=::open(file,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(once<0)return false;
        ::close(once);std::snprintf(file,sizeof(file),"%s.h264",path);
        output=::open(file,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
        if(output<0)return false;
        const int rc=pthread_create(&thread,nullptr,[](void *v)->void* { static_cast<ThermalFileTest*>(v)->run();return nullptr; },this);
        started=!rc;return started;
    }
    void join() {
        stop.store(true);
        if(started) { if(pthread_join(thread,nullptr))_exit(26);started=false; }
        if(input>=0) { ::close(input);input=-1; }
        if(output>=0) { ::close(output);output=-1; }
    }
private:
    static uint64_t now() { timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec; }
    bool io(int fd,uint8_t *bytes,size_t length,bool writing) {
        while(length && !stop.load() && now()<deadline) {
            ssize_t n=writing ? ::write(fd,bytes,length) : ::read(fd,bytes,length);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)return false;
            bytes+=n;length-=size_t(n);
        }
        return !length;
    }
    void run() {
        deadline=now()+4000000000ULL;
        auto pixels=std::unique_ptr<uint8_t[]>(new(std::nothrow) uint8_t[ThermalNv12Bytes]);
        auto packet=std::unique_ptr<uint8_t[]>(new(std::nothrow) uint8_t[4*1024*1024]);
        if(!pixels || !packet) { ca_log("ZT30 thermal test allocation failed");return; }
        if(encoder.open()) { ca_log("ZT30 thermal open failed %s raw=%d",encoder.failure(),encoder.status());_exit(26); }
        unsigned sent=0,received=0;bool ok=true;
        const uint64_t first=now();
        EncodedFrame frame{};frame.data=packet.get();frame.capacity=4*1024*1024;
        while(ok && !stop.load() && now()<deadline && received<8) {
            if(sent<8 && now()>=first+uint64_t(sent)*66666667ULL) {
                uint64_t pts=0;
                // First bounded commit uses zero; subsequent timestamps are
                // controlled samples of the separately qualified SYS clock.
                if(sent && sdk.current_pts(0,&pts)) { ok=false;break; }
                if(!io(input,pixels.get(),ThermalNv12Bytes,false)) { ok=false;break; }
                const auto rc=encoder.submit(pixels.get(),ThermalNv12Bytes,pts);
                ca_log("ZT30 thermal input frame=%u pts=%llu result=%u get=%d put=%d",sent,(unsigned long long)pts,unsigned(rc),encoder.get_status(),encoder.put_status());
                if(rc!=InputBufferResult::Submitted) {
                    // Ambiguous SDK ownership requires process recovery. The
                    // one-shot marker and default-off setting prevent retries.
                    if(rc==InputBufferResult::GetFailed || rc==InputBufferResult::InvalidHandle || rc==InputBufferResult::PutFailed)_exit(26);
                    ok=false;break;
                }
                ++sent;
            }
            const int got=encoder.copy_frame(frame);
            if(got<0)_exit(26);
            if(got) {
                ca_log("ZT30 thermal output frame=%u sequence=%u pts=%llu bytes=%zu packets=%u",received,frame.sequence,(unsigned long long)frame.pts[0],frame.bytes,frame.packets);
                if(!io(output,frame.data,frame.bytes,true)) { ok=false;break; }
                ++received;
            } else { timespec delay{0,2000000};nanosleep(&delay,nullptr); }
        }
        if(encoder.close())_exit(26);
        ca_log("ZT30 thermal file test %s sent=%u received=%u",ok && sent==8 && received==8?"PASS":"FAIL",sent,received);
    }
    NativeSdk &sdk;ThermalEncoder encoder;
    std::atomic<bool> stop{false};
    pthread_t thread{};bool started=false;
    int input=-1,output=-1;uint64_t deadline=0;
};
}
