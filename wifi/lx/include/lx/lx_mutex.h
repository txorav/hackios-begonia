/*
 * lx_mutex.h — mutual exclusion for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * MAPPING DECISION
 * ---------------------------------------------------------------------------
 * Linux mutex (sleeping lock, no-IRQ-context)          ->  IOLock (XNU).
 * Linux spin_lock (raw_spin_lock_t; IRQ disabled)      ->  IOSimpleLock.
 *
 * The distinction matters *by contract*:
 *   - lx_mutex_* calls may:  block (IOLockLock waits, kext context)
 *   - lx_spinlock_* calls may never block and must be wrapped in
 *     lx_irqsave where they pair with tasklet/interrupt receivers
 *     (lx_spinlock.h).
 *
 * IOLock / IOSimpleLock come from xnu/iokit/IOKit/IOLocks.h which is
 * linked into every lOKit kext — this file only declares the *API shape*
 * the mirrored drivers call; the actual type definitions live in lxkit/
 * (C++ glue, lxkit_locks.cpp) because IOLocks.h is a kernel-only header.
 *
 * On host tests both are emulated with a small POSIX-free monster
 * (LX_HOST_TEST creates an atomic spin with __sync ops — see
 * wifi/lx/tests/lx_host_impl.c).
 */

#ifndef LX_MUTEX_H
#define LX_MUTEX_H

#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct lx_context;       /* lxkit_api.h */

/* Opaque lock token — created via lxkit (kernel) or test stubs (host). */
struct lx_lock;

/* mutex: may block. */
struct lx_lock * lx_mutex_create(struct lx_context *ctx,
    const char *debug_name_hint);
void  lx_mutex_lock(struct lx_lock *m);
int   lx_mutex_lock_try(struct lx_lock *m);   /* 1 on success, 0 on busy */
void  lx_mutex_unlock(struct lx_lock *m);
void  lx_mutex_destroy(struct lx_lock *m);

/* Linux-shaped macros for mirrored drivers (struct mutex is embedded in the
 * driver struct): all of them take the lock TOKEN pointer (lx_lock*). */
#define DEFINE_LX_MUTEX(name)  struct lx_lock *name
#define mutex_init(m, ctx, nm)     ({ (m) = lx_mutex_create((ctx),(nm)); (m) != NULL ? LX_OK : -LX_ENOMEM; })
#define mutex_lock(m)              lx_mutex_lock(m)
#define mutex_unlock(m)            lx_mutex_unlock(m)
#define mutex_trylock(m)           lx_mutex_lock_try(m)
#define mutex_destroy(m)           lx_mutex_destroy(m)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_MUTEX_H */
