/*
 * lx_work.h — workqueues / delayed work for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * MAPPING ONTO XNU
 * ---------------------------------------------------------------------------
 * Linux workqueue                            lx implementation
 * ------------------------------------------  -------------------------------
 * alloc_workqueue(name, 0, 0)                (per-context queue created by
 *                                            lxkit_work.cpp; one per image)
 * INIT_WORK(&w, fn, arg)                     lx_work_init(ctx, &w, fn, arg)
 * schedule_work(&w)                          lx_work_schedule(&w)
 * cancel_work_sync(&w)                       lx_work_cancel_sync(&w)
 * INIT_DELAYED_WORK(&d, fn, arg)             lx_delayed_work_init(ctx,&d,fn,arg)
 * schedule_delayed_work(&d, ms)              lx_delayed_work_schedule(&d,ms)
 *
 * The lx implementation is *one queue per lxkit context* (drained on the
 * context's IOWorkLoop).  Background work runs serialized on that queue
 * alongside interrupt bottom-half dispatch:
 *   - schedule_work() is a deferred (next loop) callback on the same
 *     thread that runs card-detect work,
 *   - no parallelism is guaranteed; drivers must not rely on it,
 *   - cancelling is advisory-before-run and *synchronous-after* (teardown
 *     returns once no invocation is in flight).
 *
 * Items are intrusive: `struct lx_work_struct` EMBEDS the list node, so a
 * driver must keep the struct alive until cancel_work_sync() says it is
 * done (classic Linux discipline).
 */

#ifndef LX_WORK_H
#define LX_WORK_H

#include "lx_types.h"
#include "lx_list.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct lx_context;          /* lxkit_api.h */
struct lx_workqueue;        /* opaque */

typedef void (*lx_work_fn_t)(struct lx_context *ctx, void *arg);

/* Generic work item (embedded in the driver's own struct). */
struct lx_work_struct {
	lx_work_fn_t       fn;
	void              *arg;
	struct lx_context *ctx;      /* filled at init                       */
	bool               scheduled;/* protected by the queue lock          */
	bool               running;  /* TRUE while fn() is air-borne         */
	bool               cancelled;/* cancel_work_sync() marks before drop */
	/* intrusive node (queue-internal; do not touch) */
	struct lx_list_head node;
};

/* Delayed work: parked on the delay list until absolute deadline. */
struct lx_delayed_work {
	struct lx_work_struct work;
	u64                   deadline_ns;  /* absolute lx_ktime_get_ns()     */
};

/* ---------------------------------------------------------------------- */
/* Initialization (mirror INIT_WORK / INIT_DELAYED_WORK).  `ctx` is the     */
/* context the item runs under; items must belong to one queue lifetime.  */
/* ---------------------------------------------------------------------- */
void lx_work_init(struct lx_context *ctx, struct lx_work_struct *w,
    lx_work_fn_t fn, void *arg);
void lx_delayed_work_init(struct lx_context *ctx,
    struct lx_delayed_work *d, lx_work_fn_t fn, void *arg);

/* ---------------------------------------------------------------------- */
/* Scheduling.  `lx_work_schedule` needs no queue: it routes via ctx.      */
/* Return 0 on queued-or-running, -LX_EINVAL on a cancelled item,          */
/* -LX_ENOSPC if the queue is saturated (bring-up cap).                    */
/* ---------------------------------------------------------------------- */
int  lx_work_schedule(struct lx_work_struct *w);
int  lx_delayed_work_schedule(struct lx_delayed_work *d, u32 delay_ms);

/* Synchronously wait until the item is neither scheduled nor running. */
int  lx_work_cancel_sync(struct lx_work_struct *w);

/* Driver-facing sugar (Linux names). */
#define schedule_work(w)             lx_work_schedule((w))
#define cancel_work_sync(w)          lx_work_cancel_sync((w))
#define schedule_delayed_work(d, ms) lx_delayed_work_schedule((d), (ms))

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_WORK_H */
