#pragma once
#include "encoder_owner.h"
#include "camera_app/mp4.h"
#include "camera_app/thermal_stream.h"
#include <atomic>
#include <mutex>
#include <string>
namespace ca_zt30 {
// Shared H264/metadata MP4 writers. The RGB and thermal encoder consumers
// serialize writes with start/stop; no SDK lease survives into filesystem I/O.
class Recording {
public:
    ~Recording() { close(); }
    void configure(const char *root,unsigned width,unsigned height,ca_thermal_stream *raw);
    int set(bool active);
    int close() { return set(false); }
    bool active() const { return enabled.load(); }
    const char *path() const { return active()?paths[0].c_str():nullptr; }
    void frame(unsigned,const EncodedFrame&,uint64_t,bool,float);
private:
    int stop_locked();
    int paths_for_capture();
    std::mutex lock;
    std::atomic<bool> enabled{false};
    std::string root,paths[2];
    unsigned width=0,height=0;
    ca_mp4 *writers[2]{};
    bool waiting[2]{true,true};
    ca_thermal_stream *raw=nullptr;
};
}
