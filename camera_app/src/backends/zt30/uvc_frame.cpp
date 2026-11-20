#include "uvc_frame.h"
#include <cstring>
#include <new>
namespace ca_zt30 {
UvcFrame::UvcFrame(size_t bytes,Sink sink,void *opaque):
    data_(bytes && bytes<=1310720 ? new(std::nothrow) uint8_t[bytes]:nullptr),
    bytes_(bytes),sink_(sink),opaque_(opaque) {}
void UvcFrame::invalidate_current() {
    if(armed_ && !bad_ && !ended_)++dropped_;
    bad_=true;used_=0;
}
void UvcFrame::lost() {
    // A transport error or malformed header has no trustworthy FID. Discard
    // the next observed FID too; it may already have lost its first packet.
    invalidate_current();fid_=-1;armed_=false;ended_=false;info_={};
}
bool UvcFrame::deliver() {
    if(!sink_(opaque_,data_.get(),bytes_,info_))return false;
    ++frames_;used_=0;ended_=true;return true;
}
bool UvcFrame::packet(const uint8_t *p,size_t n,uint64_t ns) {
    if(!ready())return false;
    if(!p || n<2 || p[0]<2 || p[0]>n || !ns) {
        ++malformed_;lost();return true;
    }
    const uint8_t flags=p[1];
    const unsigned needed=2+((flags&4)?4:0)+((flags&8)?6:0);
    if(p[0]<needed) { ++malformed_;lost();return true; }
    const int fid=flags&1;
    if(fid!=fid_) {
        if(armed_ && !bad_ && !ended_) {
            if(used_==bytes_) { if(!deliver())return false; }
            else ++dropped_;
        }
        armed_=fid_!=-1;fid_=fid;used_=0;bad_=false;ended_=false;info_={};
    }
    if(flags&0x40) { invalidate_current();return true; }
    if(!armed_ || bad_ || ended_)return true;
    if(!info_.first_ns)info_.first_ns=ns;
    if(ns<info_.last_ns) { ++malformed_;invalidate_current();return true; }
    info_.last_ns=ns;
    if(flags&4) {
        const uint32_t pts=uint32_t(p[2])|(uint32_t(p[3])<<8)|(uint32_t(p[4])<<16)|(uint32_t(p[5])<<24);
        if(info_.has_pts && info_.pts!=pts)++pts_changes_;
        else { info_.pts=pts;info_.has_pts=true; }
    }
    if(flags&8) {
        const size_t at=(flags&4)?6:2;
        const uint32_t scr=uint32_t(p[at])|(uint32_t(p[at+1])<<8)|(uint32_t(p[at+2])<<16)|(uint32_t(p[at+3])<<24);
        const uint16_t sof=uint16_t(p[at+4])|(uint16_t(p[at+5])<<8);
        if(!info_.has_scr) { info_.scr_first=scr;info_.sof_first=sof;info_.has_scr=true; }
        info_.scr_last=scr;info_.sof_last=sof;
    }
    const size_t amount=n-p[0];
    if(amount>bytes_-used_) { invalidate_current();return true; }
    std::memcpy(data_.get()+used_,p+p[0],amount);used_+=amount;
    if(flags&2) {
        if(used_==bytes_)return deliver();
        invalidate_current();ended_=true;
    }
    return true;
}
}
