#ifdef NDEBUG
#error Private test assertions must remain enabled
#endif
#include "../camera_app/src/backends/zt30/input_buffer.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
using namespace ca_zt30;
using R=InputBufferResult;
namespace {
constexpr uint32_t base=0x10000000;
constexpr size_t ys=704,us=736,uv_offset=ys*512,allocation=uv_offset+us*256;
InputBufferInfo returned{},put_info{};
uint32_t handle_value=64;
int32_t get_code,put_code;
unsigned get_calls,map_calls,copy_calls,put_calls,fail_copy;
uint8_t put_flag;
bool fail_map,reenter;
std::vector<uint8_t> source(ThermalNv12Bytes),destination(allocation+32,0xcc);
uint8_t *map_pointer;
InputBufferOwner *current;
uint64_t timestamp=UINT64_C(0x8877665544332211);
void set_word(size_t offset,uint32_t v) { std::memcpy(reinterpret_cast<uint8_t*>(&returned)+offset,&v,4); }
void reset() {
    returned={};put_info={};
    set_word(16,1);set_word(36,11);set_word(52,2);set_word(56,uint32_t(640)|(uint32_t(512)<<16));
    returned.plane_address[0]=base;returned.plane_address[1]=base+uv_offset;
    returned.stride[0]=ys;returned.stride[1]=us;set_word(108,allocation);
    // Unconsumed bytes must survive exactly, including unknown Info byte20.
    auto *b=reinterpret_cast<uint8_t*>(&returned);b[20]=7;
    for(size_t i=112;i<264;++i)b[i]=uint8_t(i*23+3);
    handle_value=64;get_code=0;put_code=0;fail_copy=0;fail_map=false;reenter=false;
    get_calls=map_calls=copy_calls=put_calls=0;put_flag=255;
    std::fill(destination.begin(),destination.end(),0xcc);map_pointer=destination.data()+16;
}
int32_t fake_get(InputPort *p,InputBufferConfig *c,InputBufferInfo *i,uint32_t *h,int32_t timeout) {
    assert(p->words[0]==2 && p->words[1]==0 && p->words[2]==3 && p->words[3]==0);
    assert(timeout==1000);InputBufferConfig expected{};
    expected.type=1;expected.opaque8=timestamp;expected.width=640;expected.height=512;expected.numeric_format=11;
    assert(std::memcmp(c,&expected,120)==0);++get_calls;
    if(reenter)assert(current->submit(source.data(),source.size(),timestamp,1000)==R::Reentrant);
    std::memcpy(i,&returned,264);*h=handle_value;
    p->words[2]=99; // Owner must preserve its original port between frames.
    return get_code;
}
bool fake_map(uint32_t address,size_t bytes,uint8_t **pointer) {
    uint32_t expected_bytes=0;std::memcpy(&expected_bytes,reinterpret_cast<const uint8_t*>(&returned)+108,4);
    assert(address==returned.plane_address[0] && bytes==expected_bytes);++map_calls;
    *pointer=map_pointer;return !fail_map;
}
bool fake_copy(uint8_t *d,size_t stride,const uint8_t *s,size_t width,size_t rows) {
    ++copy_calls;assert(width==640);
    assert((copy_calls%2==1 && rows==512 && stride==ys && s==source.data() && d==map_pointer) ||
           (copy_calls%2==0 && rows==256 && stride==us && s==source.data()+640*512 && d==map_pointer+uv_offset));
    if(copy_calls==fail_copy) { d[0]=s[0];return false; }
    for(size_t y=0;y<rows;++y)std::memcpy(d+y*stride,s+y*width,width);
    return true;
}
int32_t fake_put(uint32_t handle,InputBufferInfo *info,uint8_t flag) {
    assert(handle==handle_value);assert(flag<=1);++put_calls;put_flag=flag;
    put_info=*info;assert(std::memcmp(info,&returned,264)==0);return put_code;
}
InputBufferApi api() { return {fake_get,fake_put}; }
InputBufferMemory memory() { return {fake_map,fake_copy}; }
InputPort port() { return {{2,0,3,0}}; }
void rejected(R expected,bool put_expected) {
    InputBufferOwner owner(api(),memory(),port());current=&owner;
    assert(owner.submit(source.data(),source.size(),timestamp,1000)==expected);
    assert(!owner.healthy() && get_calls==1 && put_calls==unsigned(put_expected));
    assert(owner.get_status==get_code && owner.put_status==(put_expected?put_code:0));
    if(put_expected)assert(put_flag==1);
    const unsigned gc=get_calls,pc=put_calls,mc=map_calls,cc=copy_calls;
    assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::Unhealthy);
    assert(get_calls==gc && put_calls==pc && map_calls==mc && copy_calls==cc);
}
void check_pixels() {
    for(size_t i=0;i<16;++i)assert(destination[i]==0xcc && destination[allocation+16+i]==0xcc);
    for(size_t r=0;r<512;++r)for(size_t c=0;c<ys;++c)
        assert(destination[16+r*ys+c]==(c<640?source[r*640+c]:0xcc));
    for(size_t r=0;r<256;++r)for(size_t c=0;c<us;++c)
        assert(destination[16+uv_offset+r*us+c]==(c<640?source[640*512+r*640+c]:0xcc));
}
}
int main() {
    for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*31+i/640+17);
    const auto saved_source=source;unsigned cases=0;
    reset();
    {
        InputBufferOwner owner(api(),memory(),port());current=&owner;
        assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::Submitted);
        assert(owner.healthy() && get_calls==1 && map_calls==1 && copy_calls==2 && put_calls==1 && put_flag==0);
        check_pixels();
        assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::Submitted);
        assert(get_calls==2 && put_calls==2 && copy_calls==4);check_pixels();
    }++cases;
    reset();set_word(108,uv_offset+255*us+640);
    { InputBufferOwner owner(api(),memory(),port());current=&owner;
      assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::Submitted);check_pixels();
    }++cases;
    reset();reenter=true;
    { InputBufferOwner owner(api(),memory(),port());current=&owner;assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::Submitted);assert(get_calls==1 && put_calls==1); }++cases;
    for(int32_t error:{int32_t(-1),int32_t(-110),int32_t(0x40000001)}) {
        reset();get_code=error;handle_value=0xfedcba98;rejected(R::GetFailed,false);
        assert(map_calls==0 && copy_calls==0);
    }++cases;
    for(uint32_t h:{uint32_t(0),uint32_t(63),uint32_t(8192),UINT32_MAX}) {
        reset();handle_value=h;rejected(R::InvalidHandle,false);assert(map_calls==0 && copy_calls==0);
    }++cases;
    // Every consumed metadata field gets a separately corrupt fixture. No
    // mapping or copy is attempted; the owned valid handle is discarded once.
    const size_t offsets[]={16,32,36,40,44,48,52,56,68,104};
    for(size_t offset:offsets) {
        reset();set_word(offset,0xdeadbeef);rejected(R::InvalidBuffer,true);
        assert(map_calls==0 && copy_calls==0);
        assert(std::all_of(destination.begin(),destination.end(),[](uint8_t v){return v==0xcc;}));
    }++cases;
    for(uint32_t stride:{uint32_t(0),uint32_t(639),uint32_t(4097),UINT32_MAX}) {
        for(unsigned plane=0;plane<2;++plane) { reset();returned.stride[plane]=stride;rejected(R::InvalidBuffer,true);assert(map_calls==0 && copy_calls==0); }
    }++cases;
    for(uint32_t address:{uint32_t(0),uint32_t(base-1),uint32_t(base+639),uint32_t(base+511*ys+639),uint32_t(base+allocation),UINT32_MAX}) {
        reset();returned.plane_address[1]=address;rejected(R::InvalidBuffer,true);assert(map_calls==0 && copy_calls==0);
    }++cases;
    for(unsigned plane=0;plane<2;++plane) {
        reset();returned.plane_address[plane]+=1;rejected(R::InvalidBuffer,true);assert(map_calls==0 && copy_calls==0);
    }++cases;
    reset();returned.plane_address[0]=UINT32_MAX-15;returned.plane_address[1]=UINT32_MAX-7;
    rejected(R::InvalidBuffer,true);assert(map_calls==0 && copy_calls==0);++cases;
    for(uint32_t size:{uint32_t(0),uint32_t(ThermalNv12Bytes-1),uint32_t(uv_offset+255*us+639),uint32_t(16*1024*1024+1),UINT32_MAX}) {
        reset();set_word(108,size);rejected(R::InvalidBuffer,true);assert(map_calls==0 && copy_calls==0);
    }++cases;
    reset();fail_map=true;rejected(R::MapFailed,true);assert(map_calls==1 && copy_calls==0);++cases;
    for(uint8_t *pointer:{static_cast<uint8_t*>(nullptr),reinterpret_cast<uint8_t*>(std::numeric_limits<uintptr_t>::max()-15),source.data(),source.data()+source.size()-1}) {
        reset();map_pointer=pointer;rejected(R::MapFailed,true);assert(copy_calls==0);
    }++cases;
    reset();
    { InputBufferOwner owner(api(),memory(),port());current=&owner;
      map_pointer=reinterpret_cast<uint8_t*>(&owner);
      assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::MapFailed);
      assert(copy_calls==0 && put_calls==1 && put_flag==1);
    }++cases;
    for(unsigned nth:{1U,2U}) { reset();fail_copy=nth;rejected(R::CopyFailed,true);assert(map_calls==1 && copy_calls==nth); }++cases;
    reset();put_code=-5;
    {
        InputBufferOwner owner(api(),memory(),port());current=&owner;
        assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::PutFailed);
        assert(owner.put_status==-5 && put_flag==0 && put_calls==1 && copy_calls==2);
        assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::Unhealthy && put_calls==1);
    }++cases;
    reset();fail_map=true;put_code=-5;
    rejected(R::PutFailed,true);assert(put_calls==1 && copy_calls==0);++cases;
    reset();
    {
        InputBufferOwner owner(api(),memory(),port());current=&owner;
        assert(owner.submit(nullptr,source.size(),timestamp,1000)==R::InvalidSource);
        assert(owner.submit(source.data(),source.size()-1,timestamp,1000)==R::InvalidSource);
        assert(owner.submit(source.data(),source.size()+1,timestamp,1000)==R::InvalidSource);
        auto *far=reinterpret_cast<const uint8_t*>(std::numeric_limits<uintptr_t>::max()-15);
        assert(owner.submit(far,source.size(),timestamp,1000)==R::InvalidSource);
        assert(owner.submit(source.data(),source.size(),timestamp,-1)==R::InvalidTimeout);
        assert(owner.submit(source.data(),source.size(),timestamp,1001)==R::InvalidTimeout);
        assert(get_calls==0 && put_calls==0 && owner.healthy());
    }++cases;
    for(unsigned missing=0;missing<4;++missing) {
        reset();auto a=api();auto m=memory();
        if(missing==0)a.get=nullptr;
        if(missing==1)a.put=nullptr;
        if(missing==2)m.map=nullptr;
        if(missing==3)m.copy_rows=nullptr;
        InputBufferOwner owner(a,m,port());
        assert(owner.submit(source.data(),source.size(),timestamp,1000)==R::MissingApi);
        assert(get_calls==0 && put_calls==0 && owner.healthy());
    }++cases;
    assert(source==saved_source);++cases;
    assert(cases==20);
    std::printf("ZT30 input buffer owner private PASS cases=%u no-peripheral-access\n",cases);
}
