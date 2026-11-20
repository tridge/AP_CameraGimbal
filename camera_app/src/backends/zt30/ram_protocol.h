#pragma once
#include <cstdint>
#include <cstring>
namespace ca_zt30 {
// The RAM experiment permits one named read, never a parameter index or list.
inline bool ram_lens_read(int16_t index,const char (&id)[16]) {
    static const char expected[16]="CAM_LENS";
    return index==-1 && std::memcmp(id,expected,sizeof(expected))==0;
}
}
