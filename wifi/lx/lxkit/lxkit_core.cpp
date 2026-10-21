/*
 * lxkit_core.cpp — lx_context lifecycle + the lx time source on XNU.
 *
 * Part of the lxkit glue compiled into BOTH consumer kext images that host
 * lx drivers (kexts/LxStorage is the master host; kexts/FocalTouchIOHID
 * re-links this file against its own context bound to LxKitTouchNub).
 */

#include "lxkit_types.h"

#include "libkern/c++/OSMetaClass.h"
#include "IOKit/IOLib.h"

#include "wifi/lx/include/lx/lxkit_api.h"
#include "wifi/lx/include/lx/lx_time.h"
#include "wifi/lx/include/lx/lx_printk.h"
#include "wifi/lx/include/lx/lx_errno.h"

/* ==========================================================================
 * Global default context (per kext-image).
 *
 * Exported through lxkit_default_context() so the sibling glue files can
 * route context-free driver calls (kmalloc(NULL,…), printk()) to the live
 * engine; lxkit_set_default_ctx() is glue-internal (glue_create).
 * ========================================================================== */
static struct lx_context *g_lx_default_ctx = NULL;

struct lx_context *
lxkit_default_context(void)
{
	return g_lx_default_ctx;
}

void lxkit_set_default_ctx(struct lx_context *ctx)   /* used by glue_create */
{
	g_lx_default_ctx = ctx;
}

/* ==========================================================================
 * lx time source (see lx_time.h for the model)
 *
 * Kernel: read the ARM Generic Timer (cntvct_el0 / cntfrq_el0) — this is
 * exactly the clock the architecture owns and the one the port keeps
 * running (porting plan §3.3); the frequency is read at runtime (never
 * hardcode Apple's 24 MHz; MediaTek = 13 MHz).
 *
 * Host tests install either their own provider (deterministic clocks) or
 * use the real clock via the weak shotgun into lx_host_impl.c.
 * ========================================================================== */
static u64 g_timer_hz = LX_MTK_TIMER_HZ_DEFAULT;
static lx_time_provider_t g_time_provider = NULL;

void lx_time_set_provider(lx_time_provider_t provider)
{
	lx_console_write(LX_LOGLEVEL_DEBUG, "lx-time: provider set\n", 15 + 3);
	g_time_provider = provider;
}

void lx_time_set_timer_hz(u64 hz)
{
	g_timer_hz = hz;
}

u64 lx_time_get_timer_hz(void)
{
	return g_timer_hz;
}

#if defined(LX_TARGET_KERNEL) && defined(__aarch64__)

static inline u64 lx_read_cntvct(void)
{
	u64 v;
	__asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r" (v) :: "memory");
	return v;
}

static inline u64 lx_read_cntfrq(void)
{
	u64 v;
	__asm__ volatile ("isb; mrs %0, cntfrq_el0" : "=r" (v) :: "memory");
	return v;
}

static u64 lx_time_generic_timer_ns(void)
{
	u64 ticks = lx_read_cntvct();
	u64 hz = lx_read_cntfrq();
	if (hz == 0)
		return 0;
	/* scale to ns without 128-bit overflow: split quotient/remainder */
	u64 sec = ticks / hz;
	u64 rem = ticks % hz;
	return sec * 1000000000ULL + (rem * 1000000000ULL) / hz;
}

#endif /* LX_TARGET_KERNEL && arm64 */

u64
lx_ktime_get_ns(void)
{
#if defined(LX_HOST_TEST)
	if (g_time_provider != NULL)
		return g_time_provider();
	return 0;
#else
	if (g_time_provider != NULL)
		return g_time_provider();
	return lx_time_generic_timer_ns();
#endif
}

u64
lx_ktime_get_raw_ns(void)
{
	return lx_ktime_get_ns();
}

/* jiffies lives in memory (lx_time.h declares it) */
volatile unsigned long jiffies = 0;

/* ==========================================================================
 * core context init/fini
 * =========================================================================*/
int
lxkit_core_init(struct lx_context *ctx, const char *name)
{
	if (ctx == NULL || name == NULL)
		return -LX_EINVAL;

	/* Zero the context. */
	for (unsigned int i = 0; i < sizeof(*ctx) / 4; i++)
		((u32 *)ctx)[i] = 0;

	ctx->abi_version = LXKIT_ABI_VERSION;
	ctx->magic = LXKIT_MAGIC;
	ctx->name = name;
	ctx->priv = NULL;            /* filled by lxkit_glue_create()   */
	ctx->wq = NULL;              /* filled by lxkit_work_attach()   */
	ctx->ctx_lock = NULL;        /* filled by lxkit_locks_attach()  */

	/* Per-image default context: set on master glue creation; see
	 * lxkit_glue_create() in lxkit_core.cpp — for the touch-only image
	 * this is set by its own bind. */
	return lxkit_check_context(ctx);
}

void
lxkit_core_fini(struct lx_context *ctx)
{
	if (ctx == NULL)
		return;
	if (ctx->priv != NULL) {
		lxkit_glue_destroy(ctx);
		ctx->priv = NULL;
	}
	if (g_lx_default_ctx == ctx)
		g_lx_default_ctx = NULL;
	ctx->abi_version = 0;
	ctx->magic = 0;
}

int
lxkit_check_context(const struct lx_context *ctx)
{
	if (ctx == NULL)
		return -LX_EINVAL;
	if (ctx->magic != LXKIT_MAGIC || ctx->abi_version != LXKIT_ABI_VERSION)
		return -LX_EINVAL;
	return LX_OK;
}

/* ==========================================================================
 * lx_dev_dump — bring-up friendliness for `struct lx_device` instances.
 * (Called from LxKitService::start after enumeration.)
 * ========================================================================== */
void
lx_dev_dump(struct lx_device *dev)
{
	if (dev == NULL)
		return;
	printk(KERN_INFO "lx-dev: %s res=%d plat=%d iomaps=%d drv=%s\n",
	    dev->name, dev->nres, dev->nplatform, dev->niomaps,
	    dev->drv != NULL ? dev->drv->name : "(none)");
}

/* ==========================================================================
 * Glue bundle lifecycle (lxkit_types.h::struct lxkit_glue).
 *
 * lxkit_glue_create() assembles the per-image engine:
 *   console → locks → mem → iospace → time → work → irq
 * Each *_attach() installs the lx_* C-API implementations bound to THIS
 * context (functions in the sibling lxkit_*.cpp files).
 * ========================================================================== */
struct lxkit_glue *
lxkit_glue_of(struct lx_context *ctx)
{
	if (ctx == NULL)
		return NULL;
	if (lxkit_check_context(ctx) != LX_OK)
		return NULL;
	return (struct lxkit_glue *)ctx->priv;
}

IOReturn
lxkit_glue_create(struct lx_context *ctx, IOService *owner, IOWorkLoop *wl)
{
	struct lxkit_glue *g;
	int rc;

	if (ctx == NULL || owner == NULL || wl == NULL)
		return kIOReturnBadArgument;

	if (lxkit_check_context(ctx) != LX_OK)
		return kIOReturnBadArgument;
	if (ctx->priv != NULL)
		return kIOReturnSuccess;      /* idempotent */

	g = (struct lxkit_glue *)IOMallocZero(sizeof(*g));
	if (g == NULL)
		return kIOReturnNoMemory;
	g->ctx = ctx;
	g->owner = owner;
	g->wl = wl;
	g->console_ceiling = lx_console_max_level;
	ctx->priv = g;

	/* default-context wiring for the driver glue (kmalloc(NULL,..) etc.)*/
	lxkit_set_default_ctx(ctx);

	/* Pick the install order that keeps dependencies satisfied:
	 * console first (everything logs), locks (no dependencies), then the
	 * allocators (io work queue items allocate), io space (needs the
	 * DeviceMemory helpers of the nub owner), work queue, IRQ engine. */
	lxkit_console_attach(g);
	lxkit_locks_attach(g);
	lxkit_mem_attach(g);
	lxkit_iospace_attach(g);
	lxkit_work_attach(g);
	lxkit_irq_attach(g);

	(void)rc;
	return kIOReturnSuccess;
}

void
lxkit_glue_destroy(struct lx_context *ctx)
{
	struct lxkit_glue *g = lxkit_glue_of(ctx);

	if (g == NULL)
		return;
	lxkit_irq_detach(g);             /* per-glue: timer stop + free */
	lxkit_work_detach(g);
	IOFree(g, sizeof(*g));
	ctx->priv = NULL;
	if (g_lx_default_ctx == ctx)
		g_lx_default_ctx = NULL;
}
