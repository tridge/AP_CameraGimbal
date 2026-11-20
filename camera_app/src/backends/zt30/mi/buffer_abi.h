#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace ca_zt30 {
/* ZT30 20220422 ARM32 SYS input-buffer wrappers. Sizes and positions are
 * corroborated by SDK copy instructions and the vendor software-input caller.
 * Unknown fields stay opaque. Integer address slots are not host pointers.
 * These calls have no SocId prefix. No SDK loading or pointer dereferencing
 * is permitted by this schema alone; mapping/lifetime qualification is separate.
 */
struct InputPort { uint32_t words[4]; };
struct InputBufferConfig {
    uint32_t type;
    uint8_t opaque4[4];
    uint64_t opaque8;
    uint8_t opaque16[8];
    uint16_t width,height;
    uint32_t opaque28,numeric_format;
    uint8_t opaque36[84];
};
struct alignas(8) InputBufferInfo {
    uint8_t opaque0[60];
    uint32_t plane_address[2];
    uint8_t opaque68[28];
    uint32_t stride[2];
    uint8_t opaque104[160];
};
using InputBufferHandle=uint32_t;
struct InputBufferApi {
    // On any nonzero return, Info and handle may still be overwritten with
    // garbage from the wrapper stack. Ignore them and NEVER Put that handle.
    int32_t (*get)(InputPort*,InputBufferConfig*,InputBufferInfo*,InputBufferHandle*,int32_t timeout);
    // Numeric flag values 0/1 appear in the vendor caller. Their semantics
    // and the opaque Info byte20 written on one failure path are not proved.
    int32_t (*put)(InputBufferHandle,InputBufferInfo*,uint8_t flag);
};
static_assert(sizeof(InputPort)==16 && alignof(InputPort)==4,"input port copy16");
static_assert(sizeof(InputBufferConfig)==120 && alignof(InputBufferConfig)==8,"input config copy120");
static_assert(offsetof(InputBufferConfig,type)==0 && offsetof(InputBufferConfig,opaque8)==8,"config type/opaque64");
static_assert(offsetof(InputBufferConfig,width)==24 && offsetof(InputBufferConfig,height)==26,"config geometry");
static_assert(offsetof(InputBufferConfig,opaque28)==28 && offsetof(InputBufferConfig,numeric_format)==32,"config opaque/format");
static_assert(offsetof(InputBufferConfig,opaque36)==36,"config opaque tail");
static_assert(sizeof(InputBufferInfo)==264 && alignof(InputBufferInfo)==8,"input info copy264");
static_assert(offsetof(InputBufferInfo,plane_address)==60 && offsetof(InputBufferInfo,stride)==96,"input plane addresses/strides");
static_assert(offsetof(InputBufferInfo,opaque104)==104,"input opaque tail");
static_assert(sizeof(InputBufferHandle)==4,"input handle32");
static_assert(std::is_standard_layout<InputBufferConfig>::value && std::is_standard_layout<InputBufferInfo>::value,"input flat ABI");
} // namespace ca_zt30
