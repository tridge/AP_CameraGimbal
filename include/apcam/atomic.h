#pragma once
#ifdef __cplusplus
#include <atomic>
using std::atomic_bool;
using std::atomic_int;
using std::atomic_uint;
using std::atomic_load;
using std::atomic_store;
using std::atomic_init;
using std::atomic_exchange;
using std::atomic_fetch_add;
using std::memory_order_seq_cst;
#define APC_ATOMIC(type) std::atomic<type>
#else
#include <stdatomic.h>
#define APC_ATOMIC(type) _Atomic(type)
#endif
