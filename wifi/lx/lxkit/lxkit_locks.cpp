/*
 * lxkit_locks.cpp — lx_mutex / lx_spinlock over IOLock / IOSimpleLock.
 *
 * XNU bases (all in-tree, xnu/iokit/IOKit/IOLocks.h):
 *   IOLock        — sleeping lock,      kext-safe KPI  (mutex class)
 *   IOSimpleLock  — primary-ISR-safe    (spin class)
 *   IOLockWakeup  — wakeup helper
 *
 * This file is compiled into both consumer kext images; the lock OBJECT
 * allocation always uses IOMallocZero/IOFree so it is heap-correct for
 * kext builds regardless of caller context.
 */

#include "lxkit_types.h"

#include "libkern/c++/OSMetaClass.h"
#include "IOKit/IOLib.h"
#include "IOKit/IOLocks.h"

#include "wifi/lx/include/lx/lx_mutex.h"
#include "wifi/lx/include/lx/lx_spinlock.h"
#include "wifi/lx/include/lx/lx_errno.h"

/* Attach is a no-op (locks are created per-use); the hook exists for
 * symmetry with the rest of the glue and for future per-context
 * statistics. */
void
lxkit_locks_attach(struct lxkit_glue *g)
{
	(void)g;
}

/* ==========================================================================
 * mutex (IOLock-backed; may block)
 * ========================================================================== */
struct lx_lock {
	bool        is_spin;      /* false: IOLock     true: IOSimpleLock */
	bool        armed;
	union {
		IOLock        *mutex;
		IOSimpleLock  *spin;
	} u;
	char        dbg[16];
};

struct lx_lock *
lx_mutex_create(struct lx_context *ctx, const char *debug_name_hint)
{
	struct lx_lock *l;

	(void)ctx;
	l = (struct lx_lock *)IOMallocZero(sizeof(*l));
	if (l == NULL)
		return NULL;
	l->is_spin = false;
	l->u.mutex = IOLockAlloc();
	if (l->u.mutex == NULL) {
		IOFree(l, sizeof(*l));
		return NULL;
	}
	if (debug_name_hint != NULL) {
		for (unsigned int i = 0;
		    i + 1 < sizeof(l->dbg) && debug_name_hint[i] != '\0'; i++)
			l->dbg[i] = debug_name_hint[i];
		l->dbg[sizeof(l->dbg) - 1] = '\0';
	}
	l->armed = true;
	return l;
}

void
lx_mutex_lock(struct lx_lock *m)
{
	if (m == NULL || m->armed == false)
		return;
	IOLockLock(m->u.mutex);
}

int
lx_mutex_lock_try(struct lx_lock *m)
{
	if (m == NULL || m->armed == false)
		return 0;
	return IOLockTryLock(m->u.mutex) ? 1 : 0;
}

void
lx_mutex_unlock(struct lx_lock *m)
{
	if (m == NULL || m->armed == false)
		return;
	IOLockUnlock(m->u.mutex);
}

void
lx_mutex_destroy(struct lx_lock *m)
{
	if (m == NULL)
		return;
	if (m->armed == false)
		return;
	if (m->is_spin == false)
		IOLockFree(m->u.mutex);
	else
		IOSimpleLockFree(m->u.spin);
	m->armed = false;
	IOFree(m, sizeof(*m));
}

/* ==========================================================================
 * spinlocks (IOSimpleLock-backed; never block)
 * ========================================================================= */
void
lx_spin_lock(struct lx_lock *m)
{
	if (m == NULL || m->armed == false)
		return;
	if (m->is_spin == false) {
		/* Wrong-class use: degrade into a mutex lock (bring-up). */
		IOLockLock(m->u.mutex);
		return;
	}
	IOSimpleLockLock(m->u.spin);
}

void
lx_spin_unlock(struct lx_lock *m)
{
	if (m == NULL || m->armed == false)
		return;
	if (m->is_spin == false) {
		IOLockUnlock(m->u.mutex);
		return;
	}
	IOSimpleLockUnlock(m->u.spin);
}

u32
lx_spin_lock_irqsave(struct lx_lock *m)
{
	if (m == NULL || m->armed == false)
		return 0;
	if (m->is_spin == false)
		return 0;         /* degenerate path; document in README */
	return (u32)IOSimpleLockLockDisableInterrupt(m->u.spin);
}

void
lx_spin_unlock_irqrestore(struct lx_lock *m, u32 flags)
{
	if (m == NULL || m->armed == false)
		return;
	if (m->is_spin == false)
		return;
	IOSimpleLockUnlockEnableInterrupt(m->u.spin, (IOInterruptState)flags);
}

/* Create a spin-class lock (drivers use this via spin_lock_init). */
struct lx_lock *
lx_spin_create(struct lx_context *ctx, const char *debug_name_hint)
{
	struct lx_lock *l = (struct lx_lock *)IOMallocZero(sizeof(*l));

	(void)ctx;
	if (l == NULL)
		return NULL;
	l->is_spin = true;
	l->armed = true;
	if (debug_name_hint != NULL) {
		for (unsigned int i = 0;
		    i + 1 < sizeof(l->dbg) && debug_name_hint[i] != '\0'; i++)
			l->dbg[i] = debug_name_hint[i];
		l->dbg[sizeof(l->dbg) - 1] = '\0';
	}
	l->u.spin = IOSimpleLockAlloc();
	if (l->u.spin == NULL) {
		IOFree(l, sizeof(*l));
		return NULL;
	}
	return l;
}
