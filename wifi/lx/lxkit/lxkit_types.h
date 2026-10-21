/*
 * lxkit_types.h — internal lxkit glue types (NOT part of the consumer ABI).
 *
 * Only lxkit/*.cpp and the host kext (kexts/LxStorage) include this file;
 * the shared contract stays in lxkit_api.h.
 */

#ifndef __LXKIT_TYPES_INTERNAL_H
#define __LXKIT_TYPES_INTERNAL_H

#include "IOKit/IOService.h"
#include "IOKit/IOWorkLoop.h"
#include "IOKit/IOCommandGate.h"
#include "IOKit/IOInterruptEventSource.h"
#include "IOKit/IOTimerEventSource.h"
#include "IOKit/IOMemoryDescriptor.h"
#include "IOKit/IOLocks.h"

extern "C" {
#include "wifi/lx/include/lx/lxkit_api.h"
#include "wifi/lx/include/lx/lx_dev.h"
#include "wifi/lx/include/lx/lx_list.h"
#include "wifi/lx/include/lx/lx_io.h"
#include "wifi/lx/include/lx/lx_irq.h"
#include "wifi/lx/include/lx/lx_work.h"
#include "wifi/lx/include/lx/lx_mutex.h"
#include "wifi/lx/include/lx/lx_spinlock.h"
}

/* ==========================================================================
 * lxkit glue bundle — the "engine room" behind a lx_context.
 * ========================================================================== */
struct lxkit_glue {
	struct lx_context   *ctx;
	IOService           *owner;     /* LxKitService (host) or provider nub */
	IOWorkLoop          *wl;

	/* console */
	lx_console_sink_t    console_sink_installed;
	int                  console_ceiling;    /* max level still printed */

	/* workqueue (lx_work) state */
	struct lx_workqueue *wq;

	/* locks (lx_mutex/lx_spinlock) state — see lxkit_locks.cpp structures */
	void                *_pad_locks_unused;

	/* irq soft fallback timer */
	IOTimerEventSource  *soft_irq_timer;
	bool                 soft_irq_timer_live;
};

/* lxkit help */
IOReturn lxkit_glue_create(struct lx_context *ctx, IOService *owner,
    IOWorkLoop *wl);
void     lxkit_glue_destroy(struct lx_context *ctx);
struct lxkit_glue * lxkit_glue_of(struct lx_context *ctx);

/* ==========================================================================
 * Cross-file lxkit services (implemented next to their lx_ C API).
 * ========================================================================== */

/* lxkit_core.cpp */
int        lxkit_core_init(struct lx_context *ctx, const char *name);
void       lxkit_core_fini(struct lx_context *ctx);
int        lxkit_check_context(const struct lx_context *ctx);
struct lx_context * lxkit_default_context(void);
void       lxkit_set_default_ctx(struct lx_context *ctx);

/* lxkit_armio.cpp (C++: IOKit DT walking; exposes device synthesis) */
bool       lxkit_synthesize_device(struct lx_context *ctx,
    IOService *svc, IORegistryEntry *dtChild);
void       lxkit_deactivate_devices(struct lx_context *ctx);
void       lxkit_iomap_release_for_dev(struct lx_device *dev);

/* lxkit_console.cpp */
void       lxkit_console_attach(struct lxkit_glue *g);

/* lxkit_mem.cpp (lx_kmem.h + lx_dma.h impls) */
void       lxkit_mem_attach(struct lxkit_glue *g);

/* lxkit_iospace.cpp (lx_iospace impls; ioremap via IODeviceMemory map) */
void       lxkit_iospace_attach(struct lxkit_glue *g);
void       lxkit_iomap_release_for_dev(struct lx_device *dev);

/* lxkit_work.cpp (lx_work impls; ONE workqueue per context) */
void       lxkit_work_attach(struct lxkit_glue *g);
void       lxkit_work_detach(struct lxkit_glue *g);

/* lxkit_locks.cpp (lx_mutex + lx_spinlock impls over IOLock/IOSimpleLock) */
void       lxkit_locks_attach(struct lxkit_glue *g);

/* lxkit_irq.cpp (lx_irq impls + soft fallback) */
void       lxkit_irq_attach(struct lxkit_glue *g);

/* lxkit_bootime.cpp — boot medium / root detection helpers. */
int        lxkit_boot_medium_fill(const struct lx_context *ctx,
    struct lxkit_boot_medium_info *out);

#endif /* __LXKIT_TYPES_INTERNAL_H */
