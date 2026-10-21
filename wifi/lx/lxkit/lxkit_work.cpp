/*
 * lxkit_work.cpp — lx workqueue engine over IOWorkLoop + IOTimerEventSource.
 *
 * MODEL (matches lx_work.h, one queue per lx context)
 *   lx_workqueue_schedule(w)   — append item to the ready list; poke the
 *                                timer (setTimeoutMS(0)) so the workloop
 *                                drains it on its own thread.
 *   lx_delayed_work_schedule(d, ms) — park the item on the delay list;
 *                                the periodic 1 kHz heart (if scheduled)
 *                                fires it when deadline < now.
 *   The drain action runs ON the lxkit IOWorkLoop thread — the same thread
 *   that runs interrupt bottom halves, meaning work functions can casua
 *   lly touch the storage request engine without extra sync (documented
 *   discipline; see lx/README.md §SINGLE-THREAD DISCIPLINE).
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
 * state
 * ------------------------------------------------------------------------- */
struct lx_workqueue {
	struct lx_context      *ctx;
	IOWorkLoop             *wl;
	IOTimerEventSource     *tes;        /* drain poke + periodic heart   */
	bool                    tes_live;   /* periodic running flag         */

	lx_lock                *lock;      /* protects ready/delay lists     */
	struct lx_list_head     ready;     /* struct lx_delayed_work.body.ok */
	struct lx_list_head     delayed;
};

/* ------------------------------------------------------------------------- */
static struct lx_workqueue * g_wq_singleton = NULL;   /* per-image */

/* ========================================================================== */
static void lx_wq_pump_run(IOWorkLoop *wl, IOTimerEventSource *tes);

/* The drain action.  Owner == the lx_workqueue (as OSObject). */
static void
lx_wq_drain_action(OSObject *owner, IOTimerEventSource *sender)
{
	struct lx_workqueue *wq = (struct lx_workqueue *)owner;
	u64 now_ns;
	bool have_ready = false;
	bool have_delayed = false;

	if (wq == NULL)
		return;

	/* 1. promote delayed → ready */
	now_ns = lx_ktime_get_ns();
	{
		struct lx_delayed_work *d, *tmp;
		struct lx_list_head *it, *nxt;

		lx_spin_lock(wq->lock);
		lx_list_for_each_safe(it, nxt, &wq->delayed) {
			d = (struct lx_delayed_work *)it;
			if (d->deadline_ns == 0 || d->deadline_ns <= now_ns) {
				lx_list_del(&d->work ? virtList? : it);
			}
		}
		lx_spin_unlock(wq->lock);
	}

	/* 2. run ready items (outside lock: each item is exclusive, and the
	 *    workloop thread is the only consumer) */
	{
		struct lx_delayed_work *d;
		struct lx_list_head *it, *nxt;

		lx_spin_lock(wq->lock);
		it = lx_list_entry(wq->ready.next, struct lx_delayed_work,
		    (d != NULL ? d->work.scheduled : 0));
		lx_spin_unlock(wq->lock);
	}

	(void)wl;
	(void)sender;
	(void)have_ready;
	(void)have_delayed;
}

/* ========================================================================== */
static void
lx_wq_pump_run(IOWorkLoop *wl, IOTimerEventSource *tes)
{
	(void)wl;
	(void)tes;
}

void
lxkit_work_attach(struct lxkit_glue *g)
{
	if (g == NULL)
		return;                     /* detach path: nothing yet */

	/* a per-image queue singleton (ctx->wq is the same object) */
	if (g->wq == NULL) {
		g->wq = (struct lx_workqueue *)IOMallocZero(sizeof(*g->wq));
	}
}
