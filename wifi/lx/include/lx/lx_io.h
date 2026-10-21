/*
 * lx_io.h — block I/O request/queue glue for the lx shim layer ("lx savecz
 * spec: block request/queue glue: cookies").
 *
 * ---------------------------------------------------------------------------
 * REQUEST ENGINE CONTRACT
 * ---------------------------------------------------------------------------
 *   kext (LxStorage)                    lx driver (msdc1 / ufs0)
 *   ------------------------------      --------------------------------
 *   lx_io_queue_create(dev, depth)        (driver calls this in probe)
 *   lx_io_submit(dev, req) -------->   enqueues into driver's request
 *                                      list, kicks hardware (DMA/UIC)
 *   <completion via cl->ops->on_       lx_io_complete(req, err)
 *     request_done(cookie, err)>  <--
 *   (cookie == &req->cookie, resubmit/rerun via lx_io_reinit(req))
 *
 * Request slots are PREALLOCATED (32 deep for each driver — see the
 * per-driver queue size constants in mtk_msdc_msdc1.c/mtk_ufshcd.c).  The
 * kext side (lxkit_api.h) treats `void *cookie` opaquely and never exceeds
 * the queue depth without first receiving a completion.
 *
 * THE BUFFER
 * ----------
 * `req->virt` points to a DMA-coherent buffer allocated by the CONSUMER
 * lxkit client (LxStorage) via lx_dma_alloc_coherent (see lx_dma.h), OR —
 * for interactive reads during bring-up — to an xbypass scratch region
 * living inside the MSDC/UFS driver.  Size == count * block_size.
 *
 * STATUS PATHS
 * ------------
 *   LX_IO_ST_PENDING  queued, not yet started
 *   LX_IO_ST_RUNNING  driver-owned (hardware touch)
 *   LX_IO_ST_DONE     completed, status == 0 on success
 *   LX_IO_ST_ERROR    status < 0 (negated errno, lx_errno.h)
 */

#ifndef LX_IO_H
#define LX_IO_H

#include "lx_types.h"
#include "lx_list.h"
#include "lx_mutex.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct lx_device;      /* lx_dev.h */
struct lx_context;     /* lxkit_api.h */

/* lock token aliases (lx_mutex.h defines struct lx_lock*) */
typedef struct lx_lock lx_lock_ref;

/* request opcode */
enum lx_io_op {
	LX_IO_READ = 0,
	LX_IO_WRITE,
	LX_IO_FLUSH,       /* cache-stabilize (drive-stop barrier)         */
	LX_IO_DISCARD,     /* trim (UFS unmap / msdc reserved; optional)   */
};

/* request lifecycle */
enum lx_io_state {
	LX_IO_ST_PENDING = 0,
	LX_IO_ST_RUNNING,
	LX_IO_ST_DONE,
	LX_IO_ST_ERROR,
};

/*
 * A request slot.  Queue-owned; consumers do NOT allocate this — submit by
 * LOGICAL BODY: `lx_io_submit(dev, op, lba, count, virtbuf, done_cb c...)`.
 * (see below).  `cookie` is a stable user-visible token (pointer value of
 * this struct) so completion callbacks can map back to the right user flow.
 */
struct lx_io_request {
	void               *cookie;    /* == this (opaque to consumer)        */

	enum lx_io_op      op;
	u64                lba;        /* logical block address               */
	u32                count;      /* in *logical blocks*                 */
	u32                block_size; /* copied from device desc at submit   */
	void               *virt;      /* bounce buffer start (DMA-ready)    */
	u64                bus;        /* bus addr of same, for engines       */

	int                status;     /* see enum lx_io_state; value comes   */
	                               /* in the slots (0 = ok, else errno)   */
	int                lx_err;     /* filled by driver on error           */
	                               /* -LX_ETIMEDOUT etc. see lx_errno.h   */

	void               (*done_cb)(void *cb_ctx, void *cookie, int status);
	void               *cb_ctx;

	/* driver bookkeeping (queue life) */
	struct lx_list_head q;
	struct lx_device   *dev;
	/* driver-private scratch (e.g., msdc task slot index, UFS slot no) */
	void               *hw_priv;
	u32                 hw_slot;
};

/* ------------------------------------------------------------------------------
 * The per-device queue. One per storage device; created by the driver
 * during its probe (hardware attached setup) and passed in the storage
 * descriptor (lxkit_api.h) so the kext can queue at the right depth.
 * --------------------------------------------------------------------------- */
struct lx_io_queue {
	struct lx_device   *dev;
	unsigned            depth;        /* size of the preallocated pool     */
	unsigned            inflight;     /* driver-owned: hardware busy range */

	lx_lock_ref        *q_lock;      /* protects the three lists below    */
	struct lx_list_head free;         /* idle slots                        */
	struct lx_list_head ready;        /* filtered by the driver's engine   */
	struct lx_list_head running;      /* currently in hardware for a dev   */

	void               *engine_priv;  /* driver state (opaque)             */
};

/* lock token aliases (lx_mutex.h defines struct lx_lock*) */
typedef struct lx_lock lx_lock_ref;

/* Driver-side queue create/destroy (probe/remove glue). */
struct lx_io_queue *
    lx_io_queue_create(struct lx_device *dev, unsigned depth);
void lx_io_queue_destroy(struct lx_io_queue *q);

/* Consumer-facing submit: allocate a slot from the free list, fill it, and
 * push to the driver's ready list; returns cooked `cookie` (== request ptr)
 * or NULL if there is NO slot (consumer must wait for completions before
 * retrying; the completion callback is ALWAYS delivered for a submitted
 * request, even if the driver engine later fails outright). */
void *
    lx_io_submit(struct lx_device *dev, enum lx_io_op op, u64 lba, u32 count,
    void *virt_buf, void (*done_cb)(void *cb_ctx, void *cookie, int status),
    void *cb_ctx);

/* Driver-side pop: return the request to start next, filling *slot_out.
 * Callback when drivers complete a request (drives consumer callback). */
struct lx_io_request *
    lx_io_pop_next(struct lx_io_queue *q);

/* Drivers mark results via this function: the lxkit dispatch pushes the
 * completion to the consumer (lxkit_api.h registered client callback). */
void lx_io_complete(struct lx_io_request *req, int lx_err);

/* Exit conditions for drivers pulling work: does the queue have anything? */
static inline int
lx_io_queue_drained(const struct lx_io_queue *q)
{
	return lx_list_empty((struct lx_list_head *)&q->ready);
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_IO_H */
