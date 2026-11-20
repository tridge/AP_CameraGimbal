#pragma once
#include "mi/buffer_abi.h"
#include <cstddef>
namespace ca_zt30 {
constexpr size_t ThermalNv12Bytes=640*512*3/2;
/* Injected memory boundary. map verifies the complete allocation belongs to
 * this successful SDK Get and is writable, then exposes its address. copy_rows
 * uses memcpy-equivalent row writes; it may report a partial failure. Callbacks
 * must not throw, retain pointers, or alter mappings/SDK lifetime. No real map
 * or SDK implementation is provided by this fake-qualified owner.
 */
struct InputBufferMemory {
    bool (*map)(uint32_t address,size_t bytes,uint8_t **pointer);
    bool (*copy_rows)(uint8_t *destination,size_t stride,const uint8_t *source,size_t row_bytes,size_t rows);
};
enum class InputBufferResult {
    Submitted,InvalidSource,InvalidTimeout,MissingApi,GetFailed,InvalidHandle,
    InvalidBuffer,MapFailed,CopyFailed,PutFailed,Unhealthy,Reentrant
};
/* Single-threaded synchronous 640x512 format11 candidate NV12 submission.
 * No borrowed pointer survives submit(). Successful Get with a validated
 * kernel handle gets exactly one Put, flag0 only after all copies succeed,
 * flag1 otherwise. Failed Get outputs are ignored. Invalid success handle or
 * failed Put poisons the owner: never guess/retry a release; process teardown
 * must reclaim ambiguous SDK state. Real mapping/chroma/route remain unqualified.
 */
class InputBufferOwner {
public:
    InputBufferOwner(InputBufferApi api,InputBufferMemory memory,InputPort port):a(api),m(memory),p(port) {}
    InputBufferOwner(const InputBufferOwner&)=delete;
    InputBufferOwner &operator=(const InputBufferOwner&)=delete;
    InputBufferResult submit(const uint8_t *nv12,size_t bytes,uint64_t opaque_config8,int32_t timeout);
    bool healthy() const { return !poisoned; }
    int32_t get_status=0,put_status=0;
private:
    InputBufferApi a;
    InputBufferMemory m;
    InputPort p;
    bool poisoned=false,busy=false;
};
} // namespace ca_zt30
