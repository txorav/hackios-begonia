/*
 * lx_irq.h — interrupt plumbing for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * HOW IRQs FLOW (the contract with MT6785GICv3, owned by kernel-port)
 * ---------------------------------------------------------------------------
 * The interrupt owned by an lx device (MSDC1 SDCMD/DMA-done, UFSHCI UTP/task
 * completion, touchscreen report-ready) is an ARM GICv3 SPI.  On XNU the
 * reachable interrupt controller is the `MT6785GICv3` kext, which implements
 * the in-tree base class `IOInterruptController`
 * (xnu/iokit/IOKit/IOInterruptController.h).
 *
 *   lx driver requests lx_request_irq(dev, handler, arg)
 *        |
 *        v
 *   lxkit_irq.cpp :: on the LxKitDeviceNub it sets the standard IOKit
 *   properties
 *       - "IOInterruptControllers"  (OSArray of OSSymbol; the IC name)
 *       - "IOInterruptSpecifiers"   (OSArray of OSData; vector = SPI number)
 *   then wires an IOInterruptEventSource(owner, action, provider = nub,
 *   intIndex = 0).  IOInterruptEventSource::init resolves the interrupt
 *   through IOService::resolveInterrupt(nub, source), which reads those two
 *   properties and looks the controller up via
 *   IOService::getPlatform()->lookUpInterruptController(name)
 *   (see xnu/iokit/Kernel/IOService.cpp, IOService::resolveInterrupt).
 *
 *   The lx driver's handler is delivered ON the lxkit workloop thread
 *   (IOInterruptEventSource::checkForWork -> action -> lx handler), i.e. it
 *   qualifies as a "threaded IRQ" bottom half.  Primary-interrupt-context
 *   work (reading MSDC_INT, acking, etc.) happens inside the same callback
 *   and must not loop waiting for workqueue completion — see lx_work.h.
 *
 * The GIC kext stores the vector (SPI number) in its own IOInterruptVector
 * table; it must decode the SPI from the nub's "IOInterruptSpecifiers"
 * property.  This is documented in both README files (shims + kernel-port).
 *
 * ---------------------------------------------------------------------------
 * SOFT IRQ BRING-UP FALLBACK
 * ---------------------------------------------------------------------------
 * Before the GIC kext is running (checkpoint M3 in the plan), lxkit can
 * drive handlers from an IOTimerEventSource at 1 kHz ("soft irq").  It is
 * only established when real hardware attach fails, and it is never used
 * after GIC becomes available except for devices explicitly flagged
 * LX_IRQ_SOFT in lx_dev.h.
 */

#ifndef LX_IRQ_H
#define LX_IRQ_H

#include "lx_types.h"
#include "lx_list.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct lx_context;          /* lxkit_api.h */
struct lx_device;           /* lx_dev.h */
struct lx_irq_cookie;       /* opaque registration token */

typedef int (*lx_irq_fn_t)(int irq_no, void *dev_arg);

/* irqreturn values (Linux-shaped; 0 = handled, 1 = none). */
#define LX_IRQ_HANDLED   0
#define LX_IRQ_NONE      1
#define LX_IRQ_WAKEUP    2
typedef int lx_irqreturn_t;

/* Registration flags (subset of Linux IRQF_* used by the ported drivers). */
#define LX_IRQF_LEVEL    0x0000u  /* level-triggered (the default on GICv3 SPIs) */
#define LX_IRQF_ONESHOT  0x0001u  /* handler must re-enable itself after work */
#define LX_IRQF_SOFT     0x0002u  /* force the soft-poll path (bring-up only) */

/* Register a hardware SPI for this device.  Returns an opaque token for
 * lx_free_irq()/lx_irq_enable()/lx_irq_disable()/lx_irq_synchronize().
 * NULL means registration failed (bring-up glue should fall back to
 * lx_softirq_attach()); `who` is a debug hint like "msdc1".  `irq_no` is
 * the GIC SPI number (not an IRQ line number). */
struct lx_irq_cookie *
lx_request_irq(struct lx_device *dev, u32 irq_no, unsigned flags,
    lx_irq_fn_t handler, void *arg, const char *who);

/* Free: synchronizes via lxkit (workloop thread drains), returns 0 ok. */
int lx_free_irq(struct lx_irq_cookie *c);

/* Bring-up fallback: attach a 1 kHz soft poller producing the same
 * lx_irq_fn_t delivery.  No-op if the device already has hard wiring. */
struct lx_irq_cookie *
lx_softirq_attach(struct lx_device *dev, u32 fake_irq_no,
    lx_irq_fn_t handler, void *arg, const char *who);

/* Disable/enable briefly (driver hot-direct). */
void lx_irq_enable(struct lx_irq_cookie *c);
void lx_irq_disable(struct lx_irq_cookie *c);

/* Sync: drain to a safe point (lxkit workloop); never called from a thread
 * running on THAT workloop (deadlock: document or loop, but not both). */
void lx_irq_synchronize(struct lx_irq_cookie *c);

/* tasklets */
struct lx_tasklet;
typedef void (*lx_tasklet_fn_t)(unsigned long data);

/* tasklet resurrects: schedule() from ISR; runs ONCE per schedule on the
 * lxkit workloop (like a Linux tasklet: serialized, not reentrant). */
struct lx_tasklet *
lx_tasklet_create(struct lx_context *ctx, lx_tasklet_fn_t fn,
    unsigned long data);
void lx_tasklet_schedule(struct lx_tasklet *t);
void lx_tasklet_kill(struct lx_tasklet *t);
void lx_tasklet_destroy(struct lx_tasklet *t);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_IRQ_H */
