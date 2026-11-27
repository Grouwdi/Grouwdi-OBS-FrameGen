/* compile-check only: MSVC intrinsics used by OBS's threading-windows.h */
#ifdef __MINGW32__
#include <intrin.h>
static inline long __iso_volatile_load32(const volatile int *p) { return *p; }
static inline char __iso_volatile_load8(const volatile char *p) { return *p; }
static inline char _InterlockedExchange8(volatile char *p, char v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static inline char _InterlockedCompareExchange8(volatile char *p, char e, char c) { __atomic_compare_exchange_n(p, &c, e, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c; }
#endif
