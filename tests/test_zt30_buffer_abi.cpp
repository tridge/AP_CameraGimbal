#ifdef NDEBUG
#error Private test assertions must remain enabled
#endif
#include "../camera_app/src/backends/zt30/mi/buffer_abi.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
using namespace ca_zt30;
namespace {
template<class T> struct Guarded {
    uint8_t before[16];T value;uint8_t after[16];
    Guarded() { std::memset(this,0xa5,sizeof(*this)); }
    void check() const {
        for (uint8_t b:before) assert(b==0xa5);
        for (uint8_t b:after) assert(b==0xa5);
    }
};
InputPort *expected_port;
InputBufferConfig *expected_config;
InputBufferInfo *expected_info;
InputBufferHandle *expected_handle;
int32_t expected_timeout,status;
uint8_t expected_flag;
unsigned get_calls,put_calls;
uint8_t returned_info[264],expected_config_bytes[120];
constexpr uint32_t returned_handle=0xfedcba98;
uint32_t word(const void *p,size_t offset) {
    uint32_t v;std::memcpy(&v,static_cast<const uint8_t*>(p)+offset,4);return v;
}
uint16_t half(const void *p,size_t offset) {
    uint16_t v;std::memcpy(&v,static_cast<const uint8_t*>(p)+offset,2);return v;
}
int32_t fake_get(InputPort *port,InputBufferConfig *config,InputBufferInfo *info,
                 InputBufferHandle *handle,int32_t timeout) {
    assert(port==expected_port && config==expected_config && info==expected_info);
    assert(handle==expected_handle && timeout==expected_timeout);
    assert(std::memcmp(config,expected_config_bytes,120)==0);
    ++get_calls;
    // Deliberately overwrite ALL outputs on failure too: this is the observed
    // wrapper contract, not a guarantee of initialized SDK failure outputs.
    std::memcpy(info,returned_info,264);*handle=returned_handle;
    return status;
}
int32_t fake_put(InputBufferHandle handle,InputBufferInfo *info,uint8_t flag) {
    assert(handle==returned_handle && info==expected_info && flag==expected_flag);
    assert(std::memcmp(info,returned_info,264)==0);
    ++put_calls;return status;
}
}
int main() {
    static_assert(std::is_same<decltype(InputBufferApi::get),int32_t (*)(InputPort*,InputBufferConfig*,InputBufferInfo*,uint32_t*,int32_t)>::value,"Get has five arguments and no chip prefix");
    static_assert(std::is_same<decltype(InputBufferApi::put),int32_t (*)(uint32_t,InputBufferInfo*,uint8_t)>::value,"Put has handle/info/byte flag");
    uint16_t endian=1;assert(*reinterpret_cast<uint8_t*>(&endian)==1);
    Guarded<InputPort> port;Guarded<InputBufferConfig> conf;
    Guarded<InputBufferInfo> info;Guarded<InputBufferHandle> handle;
    expected_port=&port.value;expected_config=&conf.value;
    expected_info=&info.value;expected_handle=&handle.value;
    InputBufferApi api={fake_get,fake_put};unsigned cases=0;
    std::memset(&conf.value,0,120);
    port.value.words[0]=2;port.value.words[1]=0x11223344;
    port.value.words[2]=0x55667788;port.value.words[3]=0;
    conf.value.type=1;conf.value.opaque8=UINT64_C(0x8877665544332211);
    conf.value.width=640;conf.value.height=512;conf.value.numeric_format=11;
    assert(word(&conf.value,0)==1 && word(&conf.value,8)==0x44332211 && word(&conf.value,12)==0x88776655);
    assert(half(&conf.value,24)==640 && half(&conf.value,26)==512);
    assert(word(&conf.value,28)==0 && word(&conf.value,32)==11);
    for(size_t i=36;i<120;++i)assert(reinterpret_cast<uint8_t*>(&conf.value)[i]==0);
    std::memcpy(expected_config_bytes,&conf.value,120);++cases;
    // Confirm raw ARM32 slots and copied opaque bytes, including the unknown
    // frame prefix and byte20, survive without host-pointer interpretation.
    for(size_t i=0;i<264;++i)returned_info[i]=uint8_t(i*37+11);
    uint32_t fields[]={0xf0000040,0x00000004,704,768};
    std::memcpy(returned_info+60,fields,8);std::memcpy(returned_info+96,fields+2,8);
    const int32_t timeouts[]={0,1000,-1,INT32_MAX};
    for(int32_t timeout:timeouts) {
        expected_timeout=timeout;status=0;
        assert(api.get(&port.value,&conf.value,&info.value,&handle.value,timeout)==0);
        assert(info.value.plane_address[0]==fields[0] && info.value.plane_address[1]==fields[1]);
        assert(info.value.stride[0]==704 && info.value.stride[1]==768);
        assert(handle.value==returned_handle && std::memcmp(&info.value,returned_info,264)==0);
        port.check();conf.check();info.check();handle.check();
    }++cases;
    for(uint8_t flag:{uint8_t(0),uint8_t(1),uint8_t(255)}) {
        expected_flag=flag;status=0;assert(api.put(handle.value,&info.value,flag)==0);
        info.check();handle.check();
    }++cases;
    for(int32_t error:{int32_t(-1),int32_t(-110),int32_t(0x40000001)}) {
        status=error;expected_timeout=17;const unsigned old_puts=put_calls;
        std::memset(&info.value,0,264);handle.value=0;
        const int32_t rc=api.get(&port.value,&conf.value,&info.value,&handle.value,17);
        assert(rc==error && handle.value==returned_handle && std::memcmp(&info.value,returned_info,264)==0);
        // Fixture only: no owner exists yet. Failed outputs are never submitted.
        assert(rc!=0 && put_calls==old_puts);
        info.check();handle.check();
    }++cases;
    expected_flag=1;status=-5;
    assert(api.put(returned_handle,&info.value,1)==-5);++cases;
    port.check();conf.check();info.check();handle.check();
    assert(std::memcmp(&conf.value,expected_config_bytes,120)==0);
    assert(get_calls==7 && put_calls==4);++cases;
    assert(cases==6);
    std::printf("ZT30 input buffer ABI private PASS cases=%u no-peripheral-access\n",cases);
}
