#pragma once
/* Shared ABI headers remain usable from C test tools and vendor adapters. */
#ifdef __cplusplus
#define APC_STATIC_ASSERT static_assert
#define APC_ALIGNAS alignas
#else
#define APC_STATIC_ASSERT _Static_assert
#define APC_ALIGNAS _Alignas
#endif
