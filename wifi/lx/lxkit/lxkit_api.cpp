/*
 * lxkit_api.cpp — implementations of the lxkit_api.h consumer contract
 * (storage publish/submit/client registry, touch client registry,
 * boot-medium info).
 *
 * Nothing here allocates beyond the fixed-size tables (bring-up
 * philosophy): the storage client list and per-device publish records are
 * static arrays sized by the device counts the /arm-io walker can produce
 * (msdc1, ufs0 → MAX 2 storage; one i2c touchscreen).
 */

#include "lxkit_types.h"

#include "IOKit/IOLib.h"                     /* IOMalloc / IOFree          */

extern "C" {
#include "wifi/lx/include/lx/lxkit_api.h"
#include "wifi/lx/include/lx/lx_dev.h"
#include "wifi/lx/include/lx/lx_io.h"
#include "wifi/lx/include/lx/lx_errno.h"
#include "wifi/lx/include/lx/lx_printk.h"
}

/* ==========================================================================
 * helpers
 * ========================================================================== */
static inline bool
lxkit_sane_ctx(const struct lx_context *ctx)
{
	return lxkit_check_context(ctx) == 0;
}

/* ==========================================================================
 * 1. storage publish registry (per device)
 *
 * A record is what lxkit remembers after a driver's publish; consumers get
 * a COPY of the descriptor via their client ops, submit routes through here.
 * ========================================================================== */
#define LXKIT_MAX_STORAGE_DEVS     4
#define LXKIT_MAX_STORAGE_CLIENTS  4

struct lxkit_api_dev_rec {
	struct lx_device         *dev;        /* the lx device (lookup key) */
	struct lxkit_storage_desc desc;       /* cooked descriptor          */
	bool                      live;
};

static struct lxkit_api_dev_rec    g_storage_devs[LXKIT_MAX_STORAGE_DEVS];
static struct lxkit_storage_client *g_storage_cxs[LXKIT_MAX_STORAGE_CLIENTS];
static u32                          g_storage_cl_count;

/* --------------------------------------------------------------------------
 * Touch client: single slot (the touchscreen device).
 * ------------------------------------------------------------------------ */
struct lxkit_api_touch_rec {
	struct lx_device     *dev;
	lx_touch_client_fn    cb;
	void                 *consumer_ctx;
	bool                  live;
};
static struct lxkit_api_touch_rec g_touch_client;

/* ==========================================================================
 * 2. publish / unpublish (driver side, called from *probe/*remove)
 * ========================================================================== */
extern "C" int
lxkit_storage_publish(struct lx_device *dev, u32 medium, u64 block_size,
    u64 num_blocks, void *queue, void *drv_priv)
{
	int  ti   = -1;
	unsigned i, j;

	if (dev == NULL || !lxkit_sane_ctx(dev->ctx))
		return -LX_EINVAL;

	for (i = 0; i < LXKIT_MAX_STORAGE_DEVS; i++) {
		if (g_storage_devs[i].live && g_storage_devs[i].dev == dev) {
			ti = (int)i;                     /* re-publish: update */
			break;
		}
		if (!g_storage_devs[i].live && ti < 0)
			ti = (int)i;
	}
	if (ti < 0) {
		pr_err("lxkit: storage publish table full (%u)\n",
		    (unsigned)LXKIT_MAX_STORAGE_DEVS);
		return -LX_ENOSPC;
	}

	struct lxkit_storage_desc *d = &g_storage_devs[ti].desc;
	d->abi_revision = LXKIT_ABI_VERSION;
	d->medium       = medium;
	d->block_size   = block_size;
	d->num_blocks   = num_blocks;
	d->rw_posdd     = 0;                      /* write-allowed */
	d->features     = LXKIT_ST_F_DMA;
	d->queue_fwd    = queue;
	d->driver_priv  = drv_priv;
	for (i = 0; i < LXKIT_NAME_LEN - 1 && dev->name[i] != '\0'; i++)
		d->name[i] = dev->name[i];
	d->name[i] = '\0';

	g_storage_devs[ti].dev  = dev;
	g_storage_devs[ti].live = true;

	pr_info("lxkit: published '%s' medium=%u blocks=%llu\n", d->name,
	    medium, (unsigned long long)num_blocks);

	/* push the cooked descriptor to all registered clients */
	for (j = 0; j < g_storage_cl_count; j++) {
		if (g_storage_cxs[j]->ops.on_storage_attached != NULL)
			g_storage_cxs[j]->ops.on_storage_attached(g_storage_cxs[j],
			    d, dev);
	}
	return 0;
}

extern "C" void
lxkit_storage_unpublish(struct lx_device *dev)
{
	unsigned i, j;

	if (dev == NULL)
		return;

	for (i = 0; i < LXKIT_MAX_STORAGE_DEVS; i++) {
		if (g_storage_devs[i].live && g_storage_devs[i].dev == dev) {
			for (j = 0; j < g_storage_cl_count; j++) {
				if (g_storage_cxs[j]->ops.on_storage_detached != NULL)
					g_storage_cxs[j]->ops.on_storage_detached(
					    g_storage_cxs[j], dev);
			}
			g_storage_devs[i].live = false;
			g_storage_devs[i].dev  = NULL;
			pr_info("lxkit: unpublished '%s'\n", dev->name);
			return;
		}
	}
}

/* ==========================================================================
 * 3. storage client subscription (consumer: kexts/LxStorage)
 * ========================================================================== */
extern "C" int
lxkit_storage_subscribe(struct lxkit_storage_client *cl, struct lx_context *ctx)
{
	unsigned i;

	if (cl == NULL || cl->ops.on_storage_attached == NULL ||
	    cl->ops.on_request_done == NULL || !lxkit_sane_ctx(ctx))
		return -LX_EINVAL;
	if (g_storage_cl_count >= LXKIT_MAX_STORAGE_CLIENTS)
		return -LX_ENOSPC;

	cl->lxkit_priv = NULL;
	g_storage_cxs[g_storage_cl_count++] = cl;

	/* late bind: if any device is already live, replay attach */
	for (i = 0; i < LXKIT_MAX_STORAGE_DEVS; i++) {
		if (g_storage_devs[i].live)
			cl->ops.on_storage_attached(cl, &g_storage_devs[i].desc,
			    g_storage_devs[i].dev);
	}
	return 0;
}

/* ==========================================================================
 * 4. submit path
 *
 * lxkit_storage_submit(dev, op, lba, count, buf, cookie, done_cb, cb_ctx)
 * lands on the lx engine (drivers/mt6785/*) as an lx_io_request.  We wrap
 * (cookie, done_cb, cb_ctx) into a carry struct that lx_io's completion
 * hands back to us so we can deliver BOTH:
 *   - the client's ops->on_request_done(cl, user_cookie, err)   (contract)
 *   - the submitter's own done_cb(cb_ctx, cookie, err)          (engine path)
 * and then reclaim the carry.
 * ========================================================================== */
struct lxkit_submit_carry {
	struct lxkit_storage_client *cl;
	void                          *user_cookie;
	void                          (*user_done)(void *, void *, int);
	void                          *user_ctx;
};

static void
lxkit_req_done(void *cb_ctx, void *cookie, int status)
{
	struct lxkit_submit_carry *c = (struct lxkit_submit_carry *)cb_ctx;

	if (c == NULL)
		return;

	if (c->cl != NULL && c->cl->ops.on_request_done != NULL)
		c->cl->ops.on_request_done(c->cl, c->user_cookie, status);
	if (c->user_done != NULL)
		c->user_done(c->user_ctx, c->user_cookie, status);

	IOFree(c, sizeof(struct lxkit_submit_carry));
}

extern "C" int
lxkit_storage_submit(void *lx_dev, unsigned op, u64 lba, u32 count,
    void *virt_buf, void *cookie, void (*done_cb)(void *cb, void *ck, int err),
    void *cb_ctx)
{
	struct lx_device  *dev = (struct lx_device *)lx_dev;
	struct lxkit_submit_carry *carry;
	struct lxkit_api_dev_rec *rec = NULL;
	enum lx_io_op lxop;
	void *req_cookie;
	unsigned i;

	switch (op) {
	case 0:  lxop = LX_IO_READ;    break;
	case 1:  lxop = LX_IO_WRITE;   break;
	case 2:  lxop = LX_IO_FLUSH;   break;
	case 3:  lxop = LX_IO_DISCARD; break;
	default: return -LX_EINVAL;
	}

	for (i = 0; i < LXKIT_MAX_STORAGE_DEVS; i++)
		if (g_storage_devs[i].live && g_storage_devs[i].dev == dev) {
			rec = &g_storage_devs[i];
			break;
		}
	if (rec == NULL)
		return -LX_ENOENT;
	if (g_storage_cl_count == 0)
		return -LX_EPERM;          /* no consumer ever subscribed   */

	carry = (struct lxkit_submit_carry *)
	    IOMalloc(sizeof(struct lxkit_submit_carry));
	if (carry == NULL)
		return -LX_ENOMEM;
	carry->cl         = g_storage_cxs[0];   /* single host-kext client */
	carry->user_cookie = cookie;
	carry->user_done   = done_cb;
	carry->user_ctx    = cb_ctx;

	req_cookie = lx_io_submit(dev, lxop, lba, count, virt_buf,
	    lxkit_req_done, carry);
	if (req_cookie == NULL) {
		IOFree(carry, sizeof(struct lxkit_submit_carry));
		return -LX_EAGAIN;         /* queue full: retry after drain  */
	}
	return 0;
}

/* ==========================================================================
 * 5. touch client registry
 * ========================================================================== */
extern "C" int
lx_touch_register_client(void *lx_dev, lx_touch_client_fn cb, void *ctx)
{
	struct lx_device *dev = (struct lx_device *)lx_dev;

	if (dev == NULL || cb == NULL || !lxkit_sane_ctx(dev->ctx))
		return -LX_EINVAL;
	if (g_touch_client.live && g_touch_client.dev != dev)
		return -LX_EEXIST;         /* one authoritative touch path  */

	g_touch_client.dev = dev;
	g_touch_client.cb  = cb;
	g_touch_client.consumer_ctx = ctx;
	g_touch_client.live = true;
	return 0;
}

extern "C" int
lx_touch_events_push_events(void *lx_dev, const void *events, u32 nr_events,
    u32 datalen)
{
	if (!g_touch_client.live)
		return -LX_EBUSY;
	if (g_touch_client.dev != (struct lx_device *)lx_dev)
		return -LX_ENOTSUP;        /* only the fts device has a path */
	if (events == NULL || nr_events == 0)
		return -LX_EINVAL;

	/*
	 * `events` points at struct lxkit_touch_event[nr_events] owned by
	 * the driver's deferred work — FocalTouchIOHID's callback runs on
	 * the SAME lxkit workloop and must not block or free it.
	 */
	g_touch_client.cb(g_touch_client.consumer_ctx, events, nr_events,
	    datalen);
	return 0;
}

/* ==========================================================================
 * 6. boot medium info (host context: prefer UFS — begonia boot device)
 * ========================================================================== */
extern "C" int
lxkit_get_boot_medium_info(struct lx_context *ctx,
    struct lxkit_boot_medium_info *out)
{
	struct lxkit_api_dev_rec *best = NULL;
	unsigned i, j;

	if (out == NULL || !lxkit_sane_ctx(ctx))
		return -LX_EINVAL;

	for (i = 0; i < LXKIT_MAX_STORAGE_DEVS; i++) {
		if (!g_storage_devs[i].live)
			continue;
		if (best == NULL ||
		    g_storage_devs[i].desc.medium == LXKIT_MEDIUM_UFS)
			best = &g_storage_devs[i];
	}
	if (best == NULL)
		return -LX_ENOENT;

	out->medium     = best->desc.medium;
	for (j = 0; j < LXKIT_NAME_LEN - 1 && best->desc.name[j] != '\0'; j++)
		out->name[j] = best->desc.name[j];
	out->name[j]      = '\0';
	out->num_blocks   = best->desc.num_blocks;
	out->block_size   = best->desc.block_size;
	out->root_lba     = 0;

	return 0;
}

} /* extern "C" */
