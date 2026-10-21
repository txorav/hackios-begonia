/*
 * hackios-begonia: xnu-deps os/internal/internal_shared.h
 * -------------------------------------------------------
 * Tiny fallback for the libplatform internal-shared header.
 *
 * xnu-8796 evidence: libkern/firehose/tracepoint_private.h:31-35 does
 *
 *     #if __has_include(<os/atomic_private.h>)
 *     #include <os/atomic_private.h>
 *     #else
 *     #include <os/internal/internal_shared.h>
 *     #endif
 *
 * i.e. the Apple SDK's os/atomic_private.h supplies atomics; the fallback
 * header (this one, normally from libplatform) covers the same atomic +
 * inline-swap surface for builds where only libplatform internals exist.
 * In an xnu build on Linux, `os/atomic_private.h` IS reachable (in-tree,
 * exported from libkern), so this file is mostly exercised by libplatform
 * style headers compiled without the xnu exports.  We provide the small
 * atomic helpers firehose/tracepoint code uses, delegating to
 * os/atomic.h when present so no behaviour is re-invented twice.
 */

#ifndef _OS_INTERNAL_INTERNAL_SHARED_H
#define _OS_INTERNAL_INTERNAL_SHARED_H

#include <stdint.h>

#if defined(__has_include)
#if __has_include(<os/atomic.h>)
#include <os/atomic.h>
#define FIREHOSE_INTERNAL_ATOMIC_MAPPED 1
#endif
#endif

#ifndef FIREHOSE_INTERNAL_ATOMIC_MAPPED

/* C11 minimally-faithful fallback when neither libplatform's exactly matching
 * os/atomic.h nor the xnu kernel export is present (the canonical in-tree
 * definition is libkern/os/atomic.h:78:
 * `#define os_atomic(type) type volatile _Atomic`). */
#ifndef __cplusplus
#define os_atomic(type) type volatile _Atomic
#else
#define os_atomic(type) volatile type
#endif

/* minimal OsAtomic semantic shims used by libkern/os code compiled
 * directly; see libkern/os/atomic.h for the canonical versions in-tree. */
typedef volatile long   os_atomic_long_t;
typedef volatile void   *os_atomic_ptr_t;

static inline uint32_t os_atomic_swap_uint32(volatile uint32_t *v, uint32_t n)
{
	return __atomic_exchange_n(v, n, __ATOMIC_SEQ_CST);
}

static inline uint64_t os_atomic_swap_uint64(volatile uint64_t *v, uint64_t n)
{
	return __atomic_exchange_n(v, n, __ATOMIC_SEQ_CST);
}

static inline void os_atomic_fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#endif /* mapped */

#endif /* _OS_INTERNAL_INTERNAL_SHARED_H */
