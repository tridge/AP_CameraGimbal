#pragma once
#include "mi/buffer_abi.h"
#include "mi/encoder_abi.h"
namespace ca_zt30 {
/* Standalone diagnostic API: no RGB, binds, clock, output streams or VUI. */
struct InputMetadataApi {
    int (*sys_init)(uint16_t);
    int (*sys_exit)(uint16_t);
    int (*create_device)(uint32_t,EncoderDevice*);
    int (*destroy_device)(uint32_t);
    int (*create_channel)(uint32_t,uint32_t,H26xChannel*);
    int (*destroy_channel)(uint32_t,uint32_t);
    int (*set_input_source)(uint32_t,uint32_t,const uint32_t*);
    int (*start)(uint32_t,uint32_t);
    int (*stop)(uint32_t,uint32_t);
    InputBufferApi buffer;
};
enum class InputSnapshot { BeforeGet,Leased,AfterPut,AfterExit };
struct InputMetadataObserver {
    void *context;
    uint64_t (*now_ns)(void*);
    bool (*cancelled)(void*);
    // Must not throw, retain/mutate SDK outputs, dereference plane addresses,
    // or perform SDK calls. deadline is absolute monotonic time. AfterPut is
    // emitted only after successful Put; no release claim after failure.
    bool (*snapshot)(void*,InputSnapshot,const InputBufferInfo*,uint32_t,uint64_t deadline);
    void (*event)(void*,const char*,int);
};
/* One run per process/owner. Acquires once, always flag1-discard for a valid
 * successful Get, preserves all 264 bytes. Failed Get outputs are ignored.
 * Ambiguous handles or failed Put are never retried; external process teardown
 * and strict driver/vendor restoration remain mandatory. No pointer survives.
 * Deadlines are cooperative: external watchdog bounds blocked SDK/proc calls.
 */
class InputMetadata {
public:
    static constexpr uint64_t BudgetNs=UINT64_C(6000000000);
    static constexpr uint64_t SnapshotNs=UINT64_C(1000000000);
    InputMetadata(InputMetadataApi api,InputMetadataObserver observer):a(api),o(observer) {}
    InputMetadata(const InputMetadata&)=delete;
    InputMetadata &operator=(const InputMetadata&)=delete;
    bool run();
    const char *failure=nullptr;
    int raw_status=0,cleanup_status=0;
private:
    InputMetadataApi a;
    InputMetadataObserver o;
    bool used=false;
    uint64_t begin=0,deadline=0;
    bool check(int,const char*);
    bool within_budget();
    bool observe(InputSnapshot,const InputBufferInfo*,uint32_t);
};
} // namespace ca_zt30
