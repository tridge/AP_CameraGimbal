#include "recording.h"
#include "camera_app/log.h"
#include "camera_app/video_metadata.h"
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
namespace ca_zt30 {
void Recording::configure(const char *directory,unsigned w,unsigned h,ca_thermal_stream *stream) {
    root=directory?directory:"";width=w;height=h;raw=stream;
}
int Recording::paths_for_capture() {
    if(root.empty() || !width || !height) { errno=EINVAL;return -1; }
    if(mkdir(root.c_str(),0700) && errno!=EEXIST)return -1;
    struct statvfs space{};
    if(statvfs(root.c_str(),&space) || !space.f_frsize || uint64_t(space.f_bavail)*space.f_frsize<32U*1024*1024) {
        errno=ENOSPC;return -1;
    }
    const time_t now=time(nullptr);tm local{};localtime_r(&now,&local);
    char day[16];strftime(day,sizeof(day),"%Y-%m-%d",&local);
    const std::string directory=root+"/"+day;
    if(mkdir(directory.c_str(),0700) && errno!=EEXIST)return -1;
    for(unsigned suffix=0;suffix<1000;suffix++) {
        const std::string stem=directory+"/"+day+"_"+std::to_string(int64_t(now))+"_"+std::to_string(suffix);
        paths[0]=stem+"_A."+ca_video_recording_extension();
        paths[1]=stem+"_I."+ca_video_recording_extension();
        struct stat st{};
        if(!lstat(paths[0].c_str(),&st) || errno!=ENOENT)continue;
        if(!lstat(paths[1].c_str(),&st) || errno!=ENOENT)continue;
        return 0;
    }
    errno=EEXIST;return -1;
}
int Recording::set(bool active) {
    std::lock_guard<std::mutex> guard(lock);
    if(active==enabled.load())return 0;
    if(!active)return stop_locked();
    if(paths_for_capture())return -1;
    if(ca_mp4_open(&writers[0],paths[0].c_str(),width,height,30))return -1;
    if(ca_mp4_open(&writers[1],paths[1].c_str(),640,512,15)) {
        const int saved=errno;(void)ca_mp4_close(writers[0]);writers[0]=nullptr;
        (void)unlink(paths[0].c_str());errno=saved;return -1;
    }
    if(ca_thermal_stream_recording(raw,true,paths[0].c_str())) {
        const int saved=errno;
        for(unsigned i=0;i<2;i++) { (void)ca_mp4_close(writers[i]);writers[i]=nullptr;(void)unlink(paths[i].c_str()); }
        errno=saved;return -1;
    }
    waiting[0]=waiting[1]=true;enabled.store(true);
    ca_log("ZT30 recording started: %s",paths[0].c_str());return 0;
}
int Recording::stop_locked() {
    enabled.store(false);int result=0;
    if(ca_thermal_stream_recording(raw,false,paths[0].c_str()))result=-1;
    for(auto &writer:writers)if(writer) { if(ca_mp4_close(writer))result=-1;writer=nullptr; }
    ca_log("ZT30 recording stopped%s",result?" with an I/O error":"");
    return result;
}
void Recording::frame(unsigned stream,const EncodedFrame &frame,uint64_t pts,bool key,float hfov) {
    if(stream>1 || !enabled.load())return;
    std::lock_guard<std::mutex> guard(lock);
    if(!enabled.load())return;
    if(key)waiting[stream]=false;
    if(waiting[stream])return;
    if(ca_mp4_write_h264(writers[stream],frame.data,frame.bytes,pts,key,hfov)) {
        ca_log("ZT30 recording stream=%u I/O failure errno=%d",stream,errno);
        (void)stop_locked();
    }
}
}
