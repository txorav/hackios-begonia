/*
 * lxkit_irq.cpp — interrupt plumbing for lxkit: hard SPIs through the
 *                  IOInterruptControllers property path, plus a 1 kHz
 *                  soft-poll fallback and Linux-shaped tasklets.
 *
 * ---------------------------------------------------------------------------
 * THE THREE DELIVERY PATHS (see lx_irq.h for the contract narrative)
 * ---------------------------------------------------------------------------
 *  (H) HARD:   lx_request_irq() tries IOInterruptEventSource(owner,
 *              action, provider = dev nub, 0).  The nub carries
 *              "IOInterruptControllers"/"IOInterruptSpecifiers"
 *              (installed by lxkit_service.cpp at enumerate time), so
 *              the event source resolves through the standard
 *              IOService::resolveInterrupt property path to the GIC kext
 *              (kexts/MT6785GICv3, kernel-port team).  If the GIC kext is
 *              not registered yet, or the nub has no interrupt property,
 *              the factory returns NULL and we fall through to (S).
 *              The action runs ON the lxkit workloop thread (threaded
 *              bottom half; primary interrupt context only inside the
 *              GIC kext's own handler).
 *
 *  (S) SOFT:   the same lx_irq_fn_t signature, driven by a 1 kHz
 *              IOTimerEventSource on the same workloop.  Bring-up only:
 *              produced by lx_softirq_attach() or by the automatic
 *              fallback when (H) cannot resolve.  Never persisted once a
 *              device wires a real SPI via (H).
 *
 *  (T) TASKLETS: deferred single-shot runs on the lxkit workloop,
 *              triggered with a poke to the same timer.  Serialized
 *              (list order) like a Linux tasklet; a schedule() while
 *              pending is coalesced, a schedule() while running re-runs.
 *
 * THREAD SAFETY MODEL
 * -------------------
 * All registry lists are protected by ONE interrupt-context-safe spin
 * lock per glue (state->lock, an IOSimpleLock).  Handlers and tasklet
 * bodies run outside the lock.  Teardown counters ("busy") make
 * lx_irq_synchronize() / lx_tasklet_kill() independent of the caller's
 * thread (they busy-wait with a hard ceiling rather than sleep, because
 * the caller may BE the workloop thread).
 */

#include "lxkit_types.h"

#include "libkern/c++/OSMetaClass.h"
#include "IOKit/IOLib.h"
#include "IOKit/IOWorkLoop.h"
#include "IOKit/IOTimerEventSource.h"
#include "IOKit/IOInterruptEventSource.h"

#include "wifi/lx/include/lx/lx_irq.h"
#include "wifi/lx/include/lx/lx_dev.h"
#include "wifi/lx/include/lx/lx_time.h"
#include "wifi/lx/include/lx/lx_list.h"
#include "wifi/lx/include/lx/lx_errno.h"

/* --------------------------------------------------------------------------
 * Per-glue IRQ engine state.  Allocated by lxkit_irq_attach(), freed by
 * lxkit_irq_detach().  A pointer to it is stashed in glue->irq_state so it
 * dies with its owning lx context (the touch image never shares state with
 * the host image even if both were loaded in one cache — separate images,
 * separate statics, separate allocations).
 * ------------------------------------------------------------------------- */
struct lxkit_irq_cookie {
	lx_irq_fn_t         fn;        /* the lx driver handler             */
	void               *arg;       /* its cookie                        */
	struct lx_device   *dev;       /* owner device (debug/helios tag)   */
	u32                 spi;       /* GIC SPI requested (or fake, soft) */
	unsigned            flags;     /* LX_IRQF_*                         */
	const char         *who;       /* "msdc1" etc.                      */

	/* hard wiring */
	IOInterruptEventSource *ies;   /* non-NULL => hard path active      */

	/* soft wiring */
	bool                soft;      /* polled by the 1 kHz timer         */
	struct lx_list_head slist;     /* hooks into state->soft list       */

	/* live telemetry used by lx_irq_synchronize() */
	u32                 busy;      /* in-handler count (0/1)            */
	u32                 enabled;   /* 1 = armed, 0 = gated              */

	struct lx_list_head list;      /* hooks into state->all list        */
};

struct lxkit_tasklet {
	lx_tasklet_fn_t     fn;
	unsigned long       data;
	bool                sched;
	bool                busy;
	struct lx_list_head list;      /* hooks into state->tasklets when
	                                    scheduled                  */
};

struct lxkit_irq_state {
	struct lx_context  *ctx;
	struct lxkit_glue  *glue;
	IOWorkLoop         *wl;
	IOTimerEventSource *tes;       /* the SOFT + tasklet tick source    */

	struct lx_lock     *lock;      /* one spin lock guards everything   */

	struct lx_list_head cookies;   /* every live cookie (hard + soft)   */
	struct lx_list_head tasklets;  /* tasklets currently scheduled      */
	bool                soft_clients;   /* any soft polling active       */
	u32                 busy_bound;     /* teardown telemetry            */
};

/* ==========================================================================
 * dispatch helpers (shared by all three paths)
 * ========================================================================== */
static struct lxkit_irq_cookie *
lxkit_irq_cookie_of(IS *ignored, IOTimerEventSource *sender);

/* Find the cookie whose hard event source is `ies` under state->lock. */
static struct lxkit_irq_cookie *
lxkit_irq_find_by_es(struct lxkit_irq_state *st, IOInterruptEventSource *ies)
{
	struct lxkit_irq_cookie *c;

	lx_list_for_each_entry(c, &st->cookies, struct lxkit_irq_cookie, list) {
		if (c->ies == ies)
			return c;
	}
	return NULL;
}

/* Dispatch ONE hard interrupt: bump the busy counter, run the lx driver
 * handler, drop the counter.  Handler errors other than the Linux-shaped
 * IRQ return values are logged once (bring-up aid). */
static void
lxkit_irq_run(struct lxkit_irq_cookie *c)
{
	int rc;

	c->busy++;
	lx_barrier_dmb();          /* producer/store ordering for busy  */
	rc = c->fn((int)c->spi, c->arg);
	lx_barrier_dmb();
	c->busy--;
	if (rc != LX_IRQ_HANDLED && rc != LX_IRQ_NONE && rc != LX_IRQ_WAKEUP)
		printk(KERN_WARNING "lxkit-irq(%s): bogus irqreturn %d\n",
		    c->who != NULL ? c->who : "?", rc);
}

/* (H) action: called ON the lxkit workloop thread with (owner, ies,
 * count).  owner is the glue's IOService; disambiguation is via the
 * ies pointer scanned against the state's cookie list. */
static void
lxkit_irq_hard_action(OSObject *owner, IOInterruptEventSource *ies, int count)
{
	struct lxkit_irq_state *st = g_irq_state_of(owner);
	struct lxkit_irq_cookie *c;

	(void)count;
	if (st == NULL || ies == NULL)
		return;

	lx_spin_lock(st->lock);
	c = lxkit_irq_find_by_es(st, ies);
	lx_spin_unlock(st->lock);
	if (c == NULL)
		return;            /* racing teardown; drop the twitch */

	lxkit_irq_run(c);
}

/* (S)+(T) tick: on every fire that matters, call every armed soft cookie
 * then every scheduled tasklet.  Re-arms itself only while work remains:
 *   - soft clients exist  -> keep 1 ms cadence,
 *   - tasklets still pend -> 1 ms grace and finish them,
 *   - neither             -> idle (next poke comes from the API users). */
static void
lxkit_irq_soft_action(OSObject *owner, IOTimerEventSource *sender)
{
	struct lxkit_irq_state *st = g_irq_state_of(owner);
#define LXKIT_SOFT_BATCH 8
	struct lxkit_irq_cookie *batch[LXKIT_SOFT_BATCH];
	struct lxkit_tasklet   *tb[LXKIT_SOFT_BATCH];
	struct lxkit_irq_cookie *c, *cn;
	struct lxkit_tasklet   *t, *tn;
	unsigned int ncookies = 0, ntask = 0;
	unsigned int i;

	if (st == NULL)
		return;

	/* harvest armed soft cookies */
	lx_spin_lock(st->lock);
	lx_list_for_each_entry_safe(c, cn, &st->cookies,
	    struct lxkit_irq_cookie, slist) {
		if (c->enabled == 0)
			continue;
		if (ncookies < LXKIT_SOFT_BATCH)
			batch[ncookies++] = c;
		else
			break;    /* the timer reschedules; catch next tick */
	}
	lx_spin_unlock(st->lock);

	for (i = 0; i < ncookies; i++)
		lxkit_irq_run(batch[i]);

	/* harvest scheduled tasklets */
	lx_spin_lock(st->lock);
	lx_list_for_each_entry_safe(t, tn, &st->tasklets,
	    struct lxkit_tasklet, list) {
		if (ntask < LXKIT_SOFT_BATCH)
			tb[ntask++] = t;
	}
	for (i = 0; i < ntask; i++) {
		lx_list_del(&tb[i]->list);
		tb[i]->sched = false;
		tb[i]->busy = true;
	}
	lx_spin_unlock(st->lock);

	for (i = 0; i < ntask; i++) {
		tb[i]->fn(tb[i]->data);
		lx_barrier_dmb();
		tb[i]->busy = false;
	}

	/* re-arm policy */
	if (st->soft_clients == true || ntask == LXKIT_SOFT_BATCH)
		sender->setTimeoutMS(1);   /* continue polling / draining */
	else if (ntask != 0)
		sender->setTimeoutMS(0);   /* spill tasklets this run      */
	/* else: timer goes quiet; next schedule()/poke re-arms it        */
}

/* ==========================================================================
 * owner -> state resolution
 *
 * In one kext image there is exactly ONE live lxkit IRQ engine: the state
 * is registered as a per-image singleton here AND mirrored into its glue
 * (glue->irq_state).  Actions reach the state through the singleton; the
 * glue mirror exists so teardown finds its own state even if a stale
 * singleton value races LxKitService::stop.
 * ========================================================================== */
static struct lxkit_irq_state *g_irq_state_singleton = NULL;

static struct lxkit_irq_state *
g_irq_state_of(OSObject *owner PXN)
{
	(void)owner;
	return g_irq_state_singleton;
}

/* ==========================================================================
 * attach / detach (called from lxkit_glue_create / LXKitService stop)
 * ========================================================================== */
void
lxkit_irq_attach(struct lxkit_glue *g)
{
	struct lxkit_irq_state *st;

	if (g == NULL)
		return;                   /* detach is a separate entry */
	if (g_irq_state_singleton != NULL)
		return;                   /* idempotent */

	st = (struct lxkit_irq_state *)IOMallocZero(sizeof(*st));
	if (st == NULL) {
		IOLog("lxkit-irq: state alloc failed\n");
		return;
	}
	st->ctx = g->ctx;
	st->glue = g;
	st->wl = g->wl;
	lx_list_init(&st->cookies);
	lx_list_init(&st->tasklets);
	st->lock = lx_spin_create(g->ctx, "lx-irq");
	if (st->lock == NULL) {
		IOFree(st, sizeof(*st));
		IOLog("lxkit-irq: lock alloc failed\n");
		return;
	}

	st->tes = IOTimerEventSource::timerEventSource((OSObject *)g->owner,
	    &lxkit_irq_soft_action);
	if (st->tes == NULL) {
		lx_mutex_destroy(st->lock);
		IOFree(st, sizeof(*st));
		IOLog("lxkit-irq: timer event source alloc failed\n");
		return;
	}
	if (g->wl->addEventSource(st->tes) != kIOReturnSuccess) {
		st->tes->release();
		lx_mutex_destroy(st->lock);
		IOFree(st, sizeof(*st));
		IOLog("lxkit-irq: addEventSource failed\n");
		return;
	}

	/* the event source stays gated until a soft client or tasklet pokes */
	st->tes->disable();

	g->soft_irq_timer = st->tes;      /* documented in lxkit_types.h    */
	g->soft_irq_timer_live = false;
	g->irq_state = st;
	g_irq_state_singleton = st;

	printk(KERN_INFO "lxkit-irq: engine up (soft fallback %u Hz)\n",
	    1000u);
}

void
lxkit_irq_detach(struct lxkit_glue *g)
{
	struct lxkit_irq_state *st;

	if (g == NULL)
		return;
	st = (struct lxkit_irq_state *)g->irq_state;
	if (st == NULL)
		return;

	/* Drivers are required to have released their cookies; anything left
	 * over is already dead at this point (LxKitService::stop runs after
	 * lxkit_unregister_storage_drivers).  We only tear the tick source. */
	if (st->tes != NULL) {
		st->tes->disable();
		st->wl->removeEventSource(st->tes);
		st->tes->release();
		st->tes = NULL;
	}
	if (g->soft_irq_timer == st->tes)
		g->soft_irq_timer = NULL;
	g->soft_irq_timer_live = false;
	g->irq_state = NULL;
	if (g_irq_state_singleton == st)
		g_irq_state_singleton = NULL;
	lx_mutex_destroy(st->lock);
	IOFree(st, sizeof(*st));
}