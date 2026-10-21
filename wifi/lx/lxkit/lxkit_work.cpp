/*
 * lxkit_work.cpp — lx workqueue engine over IOWorkLoop + IOTimerEventSource.
 *
 * MODEL (matches lx_work.h; one queue per lx kit context/image)
 *   lx_work_schedule(w)             — enqueue into ready; poke drain timer.
 *   lx_delayed_work_schedule(d, ms) — park on the delay list (deadline ns);
 *                                     the heart keeps 1 kHz while non-empty.
 * The drain action runs ON the lxkit IOWorkLoop thread — the same thread
 * that runs interrupt bottom halves, so work functions may touch the
 * storage request engine without new locking (single-thread discipline:
 * see wifi/lx/README.md).
 *
 * ITEMS ARE INTRUSIVE (struct lx_work_struct embeds the list node); the
 * driver keeps the struct valid until cancel_work_sync() succeeds.
 */

#include "lxkit_types.h"

#include "libkern/c++/OSMetaClass.h"
#include "IOKit/IOLib.h"
#include "IOKit/IOWorkLoop.h"
#include "IOKit/IOTimerEventSource.h"

#include "wifi/lx/include/lx/lx_work.h"
#include "wifi/lx/include/lx/lx_time.h"
#include "wifi/lx/include/lx/lx_errno.h"
#include "wifi/lx/include/lx/lx_list.h"

/* --------------------------------------------------------------------------
 * per-image queue state (one per context; ctx->wq holdings)
 * ------------------------------------------------------------------------- */
struct lx_workqueue {
	struct lx_context      *ctx;
	IOWorkLoop             *wl;
	IOTimerEventSource     *tes;         /* drain poke (single instance) */
	lx_lock                *lock;        /* protects the two lists       */
	struct lx_list_head     ready;       /* work loop drop-pending       */
	struct lx_list_head     delayed;     /* deadline-parked items        */
	bool                    heart;       /* 1kHz wake while delayed set  */
};

static struct lx_workqueue *g_wq_singleton = NULL;

/* ==========================================================================
 * The drain action.  Owner is the glue's IOService (LxKitService) so the
 * event source retains a well-known object; sender gives us nothing else.
 * ========================================================================== */
static void
lx_wq_drain_action(OSObject *owner, IOTimerEventSource *sender)
{
	struct lx_workqueue *wq = g_wq_singleton;
	struct lx_work_struct *drain_items[16];
	struct lx_delayed_work *d, *dn;
	struct lx_work_struct *w, *wn;
	unsigned int n = 0;
	u64 now_ns;

	(void)owner;
	(void)sender;
	if (wq == NULL)
		return;

	/* 1. Harvest everything that is ready, plus delayed items whose
	 *    deadline has passed. */
	now_ns = lx_ktime_get_ns();

	lx_spin_lock(wq->lock);

	lx_list_for_each_entry_safe(w, wn, &wq->ready, struct lx_work_struct,
	    node) {
		if (w->cancelled == true) {
			lx_list_del(&w->node);
			w->scheduled = false;
			continue;
		}
		lx_list_del(&w->node);
		w->running = true;
		w->scheduled = false;
		if (n < LX_ARRAY_SIZE(drain_items))
			drain_items[n++] = w;
		else
			w->scheduled = true;  /* retry next pump */
	}

	lx_list_for_each_entry_safe(d, dn, &wq->delayed,
	    struct lx_delayed_work, work.node) {
		if (d->deadline_ns != 0 && d->deadline_ns > now_ns)
			continue;
		if (d->work.cancelled == true) {
			lx_list_del(&d->work.node);
			d->work.scheduled = false;
			continue;
		}
		lx_list_del(&d->work.node);
		d->work.running = true;
		d->work.scheduled = false;
		if (n < LX_ARRAY_SIZE(drain_items))
			drain_items[n++] = &d->work;
		else
			d->work.scheduled = true;
	}

	/* keep the heart alive only while work sits delayed */
	wq->heart = !lx_list_empty(&wq->delayed);
	lx_spin_unlock(wq->lock);

	/* 2. Execute outside the lock (single-thread discipline). */
	for (unsigned int i = 0; i < n; i++) {
		struct lx_work_struct *witem = drain_items[i];

		if (witem->fn != NULL)
			witem->fn(witem->ctx, witem->arg);
		witem->running = false;
	}

	/* 3. Re-arm:
	 *      - more ready items were queued while we ran → pump now;
	 *      - delayed items remain → 1 ms heartbeat. */
	{
		bool need_ready = false;
		bool need_delay = false;

		lx_spin_lock(wq->lock);
		need_ready = !lx_list_empty(&wq->ready);
		need_delay = !lx_list_empty(&wq->delayed);
		lx_spin_unlock(wq->lock);

		if (need_ready == true)
			wq->tes->setTimeoutMS(0);
		else if (need_delay == true)
			wq->tes->setTimeoutMS(1);
		/* else: no re-arm; the next schedule() pokes again */
	}
}

/* ==========================================================================
 * queue lifecycle (called by lxkit_glue_create / lxkit_glue_destroy)
 * ========================================================================== */
void
lxkit_work_attach(struct lxkit_glue *g)
{
	if (g == NULL)
		return;         /* detach handled by detach fn */
	if (g_wq_singleton != NULL)
		return;         /* idempotent */

	struct lx_workqueue *wq =
	    (struct lx_workqueue *)IOMallocZero(sizeof(*wq));
	if (wq == NULL) {
		IOLog("lxkit-work: alloc failed\n");
		return;
	}

	wq->ctx = g->ctx;
	wq->wl = g->wl;
	lx_list_init(&wq->ready);
	lx_list_init(&wq->delayed);
	wq->lock = lx_spin_create(g->ctx, "lx-wq");
	if (wq->lock == NULL) {
		IOFree(wq, sizeof(*wq));
		return;
	}

	wq->tes = IOTimerEventSource::timerEventSource(
	    (OSObject *)g->owner, &lx_wq_drain_action);
	if (wq->tes == NULL) {
		lx_mutex_destroy(wq->lock);
		IOFree(wq, sizeof(*wq));
		IOLog("lxkit-work: timer source failed\n");
		return;
	}
	if (g->wl->addEventSource(wq->tes) != kIOReturnSuccess) {
		wq->tes->release();
		lx_mutex_destroy(wq->lock);
		IOFree(wq, sizeof(*wq));
		IOLog("lxkit-work: addEventSource failed\n");
		return;
	}

	g->wq = wq;
	g_wq_singleton = wq;
}

void
lxkit_work_detach(struct lxkit_glue *g)
{
	struct lx_workqueue *wq;

	if (g == NULL)
		return;
	wq = g->wq;
	if (wq == NULL)
		return;

	if (wq->tes != NULL) {
		wq->wl->removeEventSource(wq->tes);
		wq->tes->release();
		wq->tes = NULL;
	}
	g_wq_singleton = NULL;

	if (wq->lock != NULL)
		lx_mutex_destroy(wq->lock);
	IOFree(wq, sizeof(*wq));
	g->wq = NULL;
}

/* ==========================================================================
 * lx_work.h API
 * ========================================================================== */
void
lx_work_init(struct lx_context *ctx, struct lx_work_struct *w,
    lx_work_fn_t fn, void *arg)
{
	if (w == NULL)
		return;
	lx_memset(w, 0, sizeof(*w));
	w->fn = fn;
	w->arg = arg;
	w->ctx = ctx;
	lx_list_init(&w->node);
}

void
lx_delayed_work_init(struct lx_context *ctx,
    struct lx_delayed_work *d, lx_work_fn_t fn, void *arg)
{
	if (d == NULL)
		return;
	lx_work_init(ctx, &d->work, fn, arg);
	d->deadline_ns = 0;
}

static void
lx_wq_poke_or_park(struct lx_workqueue *wq, struct lx_work_struct *w,
    u64 delay_ns)
{
	struct lx_context *ctx = w->ctx;
	struct lx_workqueue *target = wq;

	if (target == NULL)
		return;

	lx_spin_lock(target->lock);
	if (w->cancelled == true) {
		lx_spin_unlock(target->lock);
		return;
	}
	if (w->scheduled == true || w->running == true) {
		/* already queued or in flight: coalesce */
		lx_spin_unlock(target->lock);
		return;
	}
	w->scheduled = true;
	if (delay_ns != 0) {
		struct lx_delayed_work *d =
		    (struct lx_delayed_work *)w;   /* layout: work first */
		d->deadline_ns = lx_ktime_get_ns() + delay_ns;
		lx_list_add_tail(&d->work.node, &target->delayed);
	} else {
		lx_list_add_tail(&w->node, &target->ready);
	}
	lx_spin_unlock(target->lock);

	/* Poke the drain timer; schedule() may be called from any thread. */
	target->tes->setTimeoutMS(delay_ns == 0 ? 0 : 1);

	(void)ctx;
}

int
lx_work_schedule(struct lx_work_struct *w)
{
	struct lx_workqueue *wq;

	if (w == NULL || w->ctx == NULL)
		return -LX_EINVAL;
	wq = (struct lx_workqueue *)g_wq_singleton;
	if (wq == NULL)
		return -LX_ENODEV;    /* queue not alive (bring-up order) */
	lx_wq_poke_or_park(wq, w, 0);
	return LX_OK;
}

int
lx_delayed_work_schedule(struct lx_delayed_work *d, u32 delay_ms)
{
	if (d == NULL || d->work.ctx == NULL)
		return -LX_EINVAL;
	{
		struct lx_workqueue *wq = (struct lx_workqueue *)g_wq_singleton;
		if (wq == NULL)
			return -LX_ENODEV;

		/* layout check: lx_delayed_work.work MUST be at offset 0 so
		 * the cast in lx_wq_poke_or_park() is exact. */
		if ((void *)&d->work != (void *)d)
			return -LX_EINVAL;   /* layout drift: fail loudly     */

		lx_wq_poke_or_park(wq, &d->work,
		    (u64)delay_ms * 1000000ULL);
	}
	return LX_OK;
}

int
lx_work_cancel_sync(struct lx_work_struct *w)
{
	struct lx_workqueue *wq;
	u64 started;
	bool was_running = false;

	if (w == NULL)
		return -LX_EINVAL;
	wq = (struct lx_workqueue *)g_wq_singleton;
	if (wq == NULL)
		return LX_OK;      /* queue down: nothing to race with */

	lx_spin_lock(wq->lock);
	w->cancelled = true;
	if (w->scheduled == true) {
		lx_list_del(&w->node);
		w->scheduled = false;
	}
	if (w->running == true)
		was_running = true;
	lx_spin_unlock(wq->lock);

	if (was_running == false)
		return LX_OK;

	/* Busy-wait (bounded) for the in-flight fn() to land; the runner
	 * clears `running` immediately upon return.  10 ms ceiling. */
	started = lx_ktime_get_ns();
	while (true) {
		bool r;

		lx_spin_lock(wq->lock);
		r = w->running;
		lx_spin_unlock(wq->lock);
		if (r == false)
			break;
		if (lx_ktime_get_ns() - started > 10ull * 1000ull * 1000ull)
			break;         /* give up; caller tear-down proceeds */
		/* 200 us dwell */
		for (volatile int i = 0; i < 5000; i++)
			;
	}
	return LX_OK;
}
