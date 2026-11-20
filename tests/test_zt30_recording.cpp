#include "camera_app/src/backends/zt30/recording.h"
#include "camera_app/video_metadata.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
struct ca_mp4 { FILE *file;unsigned stream; };
static unsigned opens=0,closes=0,writes[2]{},fail_open=0;
static bool fail_write=false,raw_active=false;
static std::vector<std::string> files;
static const char *extension="mp4";
const char *ca_video_recording_extension() { return extension; }
int ca_mp4_open(ca_mp4 **out,const char *path,unsigned w,unsigned h,unsigned fps) {
    assert(w && h && (fps==30 || fps==15));
    if(++opens==fail_open) { errno=EIO;return -1; }
    auto f=fopen(path,"wbx");if(!f)return -1;
    *out=new ca_mp4{f,fps==30?0U:1U};files.emplace_back(path);return 0;
}
int ca_mp4_close(ca_mp4 *writer) { assert(writer);closes++;fclose(writer->file);delete writer;return 0; }
int ca_mp4_write_h264(ca_mp4 *writer,const uint8_t *p,size_t n,uint64_t,bool,float) {
    assert(writer && p && n);writes[writer->stream]++;
    if(fail_write) { errno=ENOSPC;return -1; }
    return fwrite(p,1,n,writer->file)==n?0:-1;
}
int ca_thermal_stream_recording(ca_thermal_stream*,bool active,const char*) { raw_active=active;return 0; }
int main(int argc,char **argv) {
    assert(argc==2);ca_zt30::Recording recorder;
    recorder.configure(argv[1],1920,1080,nullptr);
    uint8_t bytes[]{0,0,0,1,0x65};ca_zt30::EncodedFrame frame;frame.data=bytes;frame.bytes=sizeof(bytes);
    assert(recorder.set(true)==0 && recorder.active() && raw_active);
    std::string first=recorder.path();assert(!first.empty());
    recorder.frame(0,frame,1,false,58);assert(writes[0]==0);
    recorder.frame(1,frame,1,true,0);assert(writes[1]==1 && writes[0]==0);
    recorder.frame(0,frame,2,true,58);assert(writes[0]==1);
    recorder.frame(0,frame,3,false,58);assert(writes[0]==2);
    assert(recorder.set(false)==0 && !recorder.active() && !raw_active && closes==2);
    assert(recorder.set(true)==0 && recorder.path()!=first);
    fail_write=true;recorder.frame(0,frame,4,true,58);
    assert(!recorder.active() && !raw_active && closes==4);
    fail_write=false;fail_open=opens+2;
    assert(recorder.set(true)<0 && !recorder.active() && !raw_active && closes==5);
    fail_open=0;assert(recorder.set(true)==0);
    std::thread consumer([&] { uint64_t pts=10;while(recorder.active())recorder.frame(0,frame,pts++,true,58); });
    assert(recorder.set(false)==0);consumer.join();
    assert(!recorder.active() && closes==7);
    extension="ts";
    assert(recorder.set(true)==0);
    const std::string misb_path=recorder.path();
    assert(misb_path.size()>3 && misb_path.substr(misb_path.size()-3)==".ts");
    assert(recorder.set(false)==0 && closes==9);
    puts("PASS ZT30 recording: keyframe gates, paired writers, unique paths, I/O rollback and concurrent stop");
}
