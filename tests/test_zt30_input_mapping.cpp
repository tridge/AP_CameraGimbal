#include "../camera_app/src/backends/zt30/input_mapping.h"
#include <cassert>
#include <cstring>
using namespace ca_zt30;
static bool parse(const char *s,std::vector<InputMapping>&out) {
    FILE *f=tmpfile();assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));rewind(f);
    bool ok=read_input_mappings(f,out);fclose(f);return ok;
}
int main() {
    std::vector<InputMapping> before,leased;
    assert(parse("10000-20000 r-xp 00000000 00:00 0 /app\n",before));
    assert(parse("10000-20000 r-xp 00000000 00:00 0 /app\nb6b5c000-b6bd4000 rw-s 00000000 00:05 123 /dev/zero (deleted)\n",leased));
    InputBufferInfo info{};info.plane_address[0]=0xb6b5c000;info.plane_address[1]=0xb6bac000;
    info.stride[0]=info.stride[1]=640;
    uint32_t bytes=491520;uint64_t py=0x2837000,pu=py+327680;
    auto *raw=reinterpret_cast<uint8_t*>(&info);
    memcpy(raw+108,&bytes,4);memcpy(raw+72,&py,8);memcpy(raw+80,&pu,8);
    assert(new_input_mapping(before,leased,info));
    assert(!new_input_mapping(leased,leased,info)); // already mapped, no provenance
    auto bad=leased;bad.back().shared_zero=false;assert(!new_input_mapping(before,bad,info));
    bad=leased;bad.back().last+=4096;assert(!new_input_mapping(before,bad,info));
    bad=leased;bad.push_back(bad.back());assert(!new_input_mapping(before,bad,info));
    // another thread freed malloc memory between the snapshots and the SDK reused its address
    bad=before;bad.push_back({info.plane_address[0]-0x29000,info.plane_address[0]+bytes,false,true});assert(new_input_mapping(bad,leased,info));
    bad=before;bad.push_back({info.plane_address[0]-4096,info.plane_address[0]+4096,false,true});assert(new_input_mapping(bad,leased,info));
    // but an SDK mapping or a file that was already there is not a new lease
    bad=before;bad.push_back({info.plane_address[0]-4096,info.plane_address[0]+4096,true,false});assert(!new_input_mapping(bad,leased,info));
    bad=before;bad.push_back({info.plane_address[0]-4096,info.plane_address[0]+4096,false,false});assert(!new_input_mapping(bad,leased,info));
    std::vector<InputMapping> kinds;
    assert(parse("10000-20000 rw-p 00000000 00:00 0 \n20000-30000 rw-p 00000000 00:00 0 [heap]\n"
                 "30000-40000 r-xp 00000000 b3:01 77 /lib/libc.so\n40000-50000 rw-s 00000000 00:05 9 /dev/zero (deleted)\n",kinds));
    assert(kinds[0].anonymous && kinds[1].anonymous && !kinds[2].anonymous && !kinds[3].anonymous && kinds[3].shared_zero);
    pu++;memcpy(raw+80,&pu,8);assert(!new_input_mapping(before,leased,info));
    assert(!parse("10000-20000 rw-s 0 00:00 0 /dev/zero (deleted)",bad));
    assert(!parse("20000-10000 rw-s 0 00:00 0 /dev/zero (deleted)\n",bad));
    assert(parse("10000-20000 rw-p 0 00:00 0 /dev/zero (deleted)\n",bad) && !bad[0].shared_zero);
    assert(parse("10000-20000 rw-s 0 00:00 0 /dev/zero (deleted) extra\n",bad) && !bad[0].shared_zero);
    puts("ZT30 input mapping checks PASS");
}
