#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
namespace ca_zt30 {
// A complete byte frame; pixel planes and radiometric units are deliberately unknown.
struct UvcFrameInfo {
    uint64_t first_ns=0,last_ns=0;
    uint32_t pts=0,scr_first=0,scr_last=0;
    uint16_t sof_first=0,sof_last=0;
    bool has_pts=false,has_scr=false;
};
class UvcFrame {
public:
    using Sink=bool(*)(void*,const uint8_t*,size_t,const UvcFrameInfo&);
    UvcFrame(size_t bytes,Sink sink,void *opaque);
    bool ready() const { return bool(data_) && sink_; }
    bool packet(const uint8_t *data,size_t bytes,uint64_t received_ns);
    void lost();
    uint64_t frames() const { return frames_; }
    uint64_t dropped() const { return dropped_; }
    uint64_t malformed() const { return malformed_; }
    uint64_t pts_changes() const { return pts_changes_; }
private:
    bool deliver();
    void invalidate_current();
    std::unique_ptr<uint8_t[]> data_;
    size_t bytes_,used_=0;
    Sink sink_;void *opaque_;
    int fid_=-1;
    bool armed_=false,bad_=false,ended_=false;
    UvcFrameInfo info_{};
    uint64_t frames_=0,dropped_=0,malformed_=0,pts_changes_=0;
};
}
