/*
 * lx_spinlock.h — code-context spinlocks for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * RULES
 * ---------------------------------------------------------------------------
 * A spinlock is used for:
 *   - driver-internal state shared between an interrupt receiver (the
 *     workloop-thread-safe lxkit ISR path) and the block/touch submission
 *     threads, and
 *   - queue head/list mutation where no sleeping (IOLock) is legal.
 *
 * The lxkit implementation uses IOSimpleLock (IOLocks.h family), which is
 * safe to take from primary interrupt context (via lxkit's
 * IOInterruptEventSource callbacks).
 *
 * IRQ-save/restore (`lx_spin_lock_irqsave`) matches the Linux
 * `spin_lock_irqsave` pattern the mirrored drivers rely on:
 *   unsigned long flags;
 *   spin_lock_irqsave(&lock, flags);
 *   ...
 *   spin_unlock_irqrestore(&lock, flags);
 *
 * On kernel builds this maps to:
 *   flags = ml_set_interrupts_enabled(FALSE);  (arm: ml_spin_lock is not
 *                                              exposed to kexts)  TODO/NOT:
 * ...
 * The kernel side (lxkit_locks.cpp) uses:
 *    IOSimpleLockTake / IOSimpleLockRestore around ml_disable_ie()
 * so interrupt state round-trips exactly.
 */

#ifndef LX_SPINLOCK_H
#define LX_SPINLOCK_H

#include "lx_types.h"
#include "lx_mutex.h"        /* lx_lock token type shared */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* raw spin acquire — safe in primary interrupt context. */
void lx_spin_lock(struct lx_lock *m);
void lx_spin_unlock(struct lx_lock *m);

/* save + disable local interrupts, take spinlock; flags is 32-bit opaque. */
u32  lx_spin_lock_irqsave(struct lx_lock *m);
void lx_spin_unlock_irqrestore(struct lx_lock *m, u32 flags);

/* spin-class allocation (IOSimpleLock-backed; interrupt-context safe). */
struct lx_lock *
lx_spin_create(struct lx_context *ctx, const char *debug_name_hint);

/* Linux-alias-lint macros for mirrored drivers */

#define spin_lock_init(mm)     do { (mm) = lx_spin_create(NULL, "spin"); } while (0)
#define spin_lock(m)           lx_spin_lock(m)
#define spin_unlock(m)         lx_spin_unlock(m)
#define spin_lock_irqsave(m, fl)  ((fl) = lx_spin_lock_irqsave((m)))
#define spin_unlock_irqrestore(m, fl)  lx_spin_unlock_irqrestore((m), (fl))
#define spin_trylock(m)        lx_mutex_lock_try(m)
#define spin_unlock_wait(m)    do { /* deprecated; unused */ lx_barrier_dmb(); } while (0)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_SPINLOCK_H */
