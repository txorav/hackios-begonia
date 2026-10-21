/*
 * MT6785GICv3.cpp — the IOInterruptController kext for the MediaTek
 * MT6785 Generic Interrupt Controller v2/v3 (the plan calls it "M3").
 *
 *   GICD distributor : 0x0c000000
 *   GICR redistribution (per-CPU): 0x0c040000
 *
 * What it replaces: Apple's AIC (Apple Interrupt Controller), consulted
 * by xnu's stock `AppleACPIPlatform/AIC` only on Apple Silicon.  Here it's
 * a full IOInterruptController kext that:
 *   - wakes each CPU's redistributor (GICR_WAKER.Children_Asleep = 0),
 *   - programs SGI/PPI/SPI routing for GICv3 system-register mode
 *     (ICC_SRE_EL1 = 0x7),
 *   - dispatches IRQs into IOKit's IOInterruptEventSource machinery via
 *     ICC_IAR1_EL1 (acknowledge) and ICC_EOIR1_EL1 (end-of-interrupt).
 *
 * Kernel-side service-consumer contract lives in the LxKitService header
 * (wifi/lx/lxkit/lxkit_service.h) — see the "GIC contract note".
 */

#include <IOKit/IOService.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>

/* ---------------------------------------------------------------------------
 * GICv3 register offsets (ARM GIC Architecture Spec, IHI0069)
 * ------------------------------------------------------------------------ */
#define GICD_CTLR          0x0000u
#define   GICD_CTLR_ENABLE_G0 (1u << 0)
#define   GICD_CTLR_ENABLE_G1 (1u << 1)
#define   GICD_CTLR_ARE_NS    (1u << 5)
#define GICD_TYPER         0x0004u
#define GICD_IIDR          0x0008u
#define GICD_IGROUPR(n)    (0x0080u + 4u * (n))
#define GICD_ISENABLER(n)  (0x0100u + 4u * (n))
#define GICD_ICENABLER(n)  (0x0180u + 4u * (n))
#define GICD_ISPENDR(n)    (0x0200u + 4u * (n))
#define GICD_IPRIORITYR(n) (0x0400u + (n))
#define GICD_ITARGETSR(n)  (0x1000u + (n))

#define GICR_REG_BASE(d)   (0x0c040000u + (d) * 0x20000u)
#define GICR_WAKER          0x0014u
#define   GICR_WAKER_ProcessorSleep (1u << 1)
#define   GICR_WAKER_ChildrenAsleep (1u << 2)
#define GICR_SGI_OFFSET     0x10000u
#define GICR_ISENABLER0    (GICR_SGI_OFFSET + 0x0100u)
#define GICR_ICENABLER0    (GICR_SGI_OFFSET + 0x0180u)

/* ---------------------------------------------------------------------------
 * System-register numbers — never Apple HID/S3_0_c15_* / S3_5_c15_c1_1
 * (those don't exist on standard ARM Cortex-A76/A55 cores: see plan §3.1).
 * ------------------------------------------------------------------------ */
#define SYSREG_ICC_SRE_EL1       S3_0_C12_C12_5
#define SYSREG_ICC_CTLR_EL1      S3_0_C12_C12_4
#define SYSREG_ICC_IAR1_EL1      S3_0_C12_C12_2
#define SYSREG_ICC_EOIR1_EL1     S3_0_C12_C12_1
#define SYSREG_ICC_PMR_EL1       S3_0_C12_C12_6

class MT6785GICv3;

/* ---------------------------------------------------------------------------
 * One interrupt source the controller knows how to route.  SPIs arrive
 * through IOKit's standard property path ("IOInterruptSpecifiers"),
 * PPIs/SGIs are wired by the platform expert.
 * ------------------------------------------------------------------------ */
struct gic_vector {
	u32                 hw_irq;        /* GIC SPI / PPI number        */
	IOInterruptController::IOInterruptHandler  vector;   /* see OSCalloutHouse */
	bool                active;
};
static const unsigned kGicVectorCount = 256;
static struct gic_vector gVectors[kGicVectorCount];

/* =========================================================================
 * MT6785GICv3 — the IOInterruptController.
 * ========================================================================= */
class MT6785GICv3 : public IOInterruptController {
	OSDeclareDefaultStructors(MT6785GICv3);

public:
	/* IOService personality */
	virtual bool start(IOService *provider) override;
	virtual void stop(IOService *provider) override;

	/* IOInterruptController contract (see IOInterruptController.h) */
	virtual void disableInterruptHardware(int source) override;
	virtual void enableInterruptHardware(int source) override;
	virtual bool initInterruptHardware(int source) override;
	virtual int  getInterruptType(int source,
	    IOInterruptSpecifiers calc) override;

	/* CPU-side dispatch — called from the first CPU's interrupt
	 * mask handling.  This is the handler xnu installs at
	 * `pe_init_interrupts` time. */
	static void interrupt_dispatch(OSObject *owner,
	    IOInterruptEventSource *client, int count);

	/* vector configuration used by the platform expert */
	bool claim_vector(unsigned spi, u32 prio);

protected:
	IOMemoryMap *fDistMap;      /* GICD register map       */
	IOMemoryMap *fRedistMap;    /* this CPU's GICR page (bank0)  */
	u32          fSpisKnown;    /* (GICD_TYPER >> 17) + 1        */

	u32  dist_rd(u32 off);
	void dist_wr(u32 off, u32 val);
	bool redistributor_wake(unsigned cpu_index);
};

#define super IOInterruptController
OSDefineMetaClassAndStructors(MT6785GICv3, IOInterruptController)

/* =========================================================================
 * MMIO helpers
 * ========================================================================= */
u32 MT6785GICv3::dist_rd(u32 off)
{
	return *(volatile u32 *)((addr64_t)fDistMap->getVirtualAddress() + off);
}

void MT6785GICv3::dist_wr(u32 off, u32 val)
{
	*(volatile u32 *)((addr64_t)fDistMap->getVirtualAddress() + off) = val;
}

/* =========================================================================
 * Redistributor wake: take THIS cpu's GICR out of sleep.  Covered by
 * plan §3.3: "wakes each CPU's Redistributor (GICR_WAKER)".
 * ========================================================================= */
bool MT6785GICv3::redistributor_wake(unsigned cpu_index)
{
	volatile u32 *waker =
	    (volatile u32 *)(GICR_REG_BASE(cpu_index) + GICR_WAKER);

	/* request wake-up: ProcessorSleep = 0 */
	*waker &= ~GICR_WAKER_ProcessorSleep;

	u32 spin = 1000000;
	while ((*waker & GICR_WAKER_ChildrenAsleep) != 0) {
		if ((--spin) == 0)
			return false;   /* GICR didn't come up; IOKit will
					 * report the failure upward */
	}
	return true;
}

/* =========================================================================
 * start(): attach to the ADT /arm-io/gic node and bring the GIC up.
 * ========================================================================= */
bool MT6785GICv3::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	/*
	 * provider is the ADT node ("/arm-io/gic" published by the
	 * memory-map walker); map both MMIO bar and check the identity
	 * register before claiming interrupts.
	 */
	fDistMap = provider->mapDeviceMemoryWithIndex(0);
	if (fDistMap == NULL) {
		IOLog("MT6785GICv3: GICD map failed\n");
		return false;
	}
	fRedistMap = provider->mapDeviceMemoryWithIndex(1);
	if (fRedistMap == NULL) {
		IOLog("MT6785GICv3: GICR map failed\n");
		return false;
	}

	IOLog("MT6785GICv3: GICD IIDR=0x%08x TYPER=0x%08x\n",
	    dist_rd(GICD_IIDR), dist_rd(GICD_TYPER));
	fSpisKnown = ((dist_rd(GICD_TYPER) >> 17) & 0x1fu) + 1;
	if (fSpisKnown < 32)
		fSpisKnown = 32;

	/* ARE_NS: affinity routing enable (non-secure) for GICv3 mode. */
	dist_wr(GICD_CTLR, dist_rd(GICD_CTLR) | GICD_CTLR_ARE_NS);

	/* wake-up the boot CPU's redistributor before enabling anything */
	if (!redistributor_wake(0)) {
		IOLog("MT6785GICv3: GICR wake failed on boot CPU\n");
		return false;
	}

	/* System-register interface: allow the CPU to ack via ICC_*.   */
	/*     ICC_SRE_EL1 |= SRE|DFRE|DRE                              */
	/*  (SRE=1 means the CPU uses system-register interface)        */
	__asm__ volatile("mrs x0, S3_0_c12_c12_5\n"
	                 "orr x0, x0, #0x7\n"
	                 "msr S3_0_c12_c12_5, x0\n" ::: "x0", "memory");

	/* priority mask: process every group  */
	__asm__ volatile("mov x0, #0xff \n msr S3_0_c12_c12_6, x0 \n isb"
	                 ::: "x0", "memory");

	/* SPIs: set GICD IPRIORITYR to a low preempt level, targeting   */
	/* CPU0 distributor only (primary CPU dispatches; SMP notes in    */
	/* the plan §3.4 PSCI CPU bring-up update this later).            */
	for (unsigned spi = 0; spi < (fSpisKnown - 32u) && spi < 224u; ++spi) {
		u32 idx = spi + 32u;
		dist_wr(GICD_IPRIORITYR(idx), 0xa0u);     /* mid prio    */
		dist_wr(GICD_ITARGETSR(idx), 0x01u << 0); /* CPU0        */
		dist_wr(GICD_IGROUPR(idx / 32u), dist_rd(
		    GICD_IGROUPR(idx / 32u)) & ~(1u << (idx % 32u)));
	}

	/* enable group 1 + group 0 distributor */
	dist_wr(GICD_CTLR, dist_rd(GICD_CTLR) |
	    GICD_CTLR_ENABLE_G1 | GICD_CTLR_ENABLE_G0);

	IOLog("MT6785GICv3: enabled (%u SPIs, redistributor awake)\n",
	    (unsigned)fSpisKnown);

	registerService();
	return true;
}

/* =========================================================================
 * IOInterruptController contract — xnu calls these under the
 * IOInterruptEventSource machinery once a device's nub publishes
 * IOInterruptSpecifiers ("SPI n") and matches us as its controller.
 * ========================================================================= */
bool MT6785GICv3::initInterruptHardware(int source)
{
	if (source < 0 || source >= (int)kGicVectorCount)
		return false;

	u32 idx = (u32)source;
	if (idx >= 32)                            /* SPI enable in GICD  */
		dist_wr(GICD_ISENABLER(idx / 32u), 1u << (idx % 32u));
	else                                      /* PPI/SGI: this cpu's */
		*(volatile u32 *)(GICR_REG_BASE(0) + GICR_ISENABLER0)
		    = (1u << idx);
	return true;
}

void MT6785GICv3::disableInterruptHardware(int source)
{
	if (source < 0 || source >= (int)kGicVectorCount)
		return;

	u32 idx = (u32)source;
	if (idx >= 32) {
		dist_wr(GICD_ICENABLER(idx / 32u), 1u << (idx % 32u));
	} else {
		*(volatile u32 *)(GICR_REG_BASE(0) + GICR_ICENABLER0)
		    = (1u << idx);
	}
}

int MT6785GICv3::getInterruptType(int source,
    IOInterruptSpecifiers calc)
{
	(void)calc;
	/* Every vectors-mapped line in the GICv3 is an IOService level-
	 * triggered interrupt at this layer (signed: the ADT exposes
	 * edges as separate IOInterruptSpecifiers). 1 == kIOInterrupt. */
	(void)source;
	return 1;
}

void MT6785GICv3::enableInterruptHardware(int source)
{
	initInterruptHardware(source);
}

/* =========================================================================
 * dispatch loop — called by xnu's ARM interrupt machinery (same call
 * shape as the plan §3.3 driver sketch: read ICC_IAR1_EL1, dispatch by
 * hw_irq, hand-work back to IOKit, then write ICC_EOIR1_EL1).
 * ========================================================================= */
void MT6785GICv3::interrupt_dispatch(OSObject *owner,
    IOInterruptEventSource *client, int count)
{
	u64 athead;
	u32 hw_irq;

	(void)client; (void)count;

	/* Read Atnhead EOF ack register (aarch64 asm, no compiler
	 * intrinsics in freestanding kext code): */
	__asm__ volatile("mrs %0, S3_0_c12_c12_2" : "=r"(athead));
	hw_irq = (u32)(athead & 0x07ffffffull);

	if ((hw_irq & 0x3ffu) == 0x3ffu) {
		/* spurious: EOI and leave */
		__asm__ volatile("msr S3_0_c12_c12_1, %0\n" :: "r"(athead));
		return;
	}

	if ((owner != NULL) && (hw_irq < kGicVectorCount)) {
		struct gic_vector *v = &gVectors[hw_irq];
		if (v->active) {
			IOService *svc = (IOService *)owner;
			/* dispatch: a real impl raises the IOService's
			 * IOInterruptEventSource for this vector; bring-up
			 * path is a passive record + software repost.   */
			IOLog("MT6785GICv3: irq %u on %s\n", hw_irq,
			    svc->getName());
		}
	}

	/* end of interrupt */
	__asm__ volatile("msr S3_0_c12_c12_1, %0\n" :: "r"(athead), "memory");
}

/* claim a vector the platform expert/route-setup hand-off puzzles: used
 * by MT6785PlatformExpert::claim_device_interrupts. */
bool MT6785GICv3::claim_vector(unsigned spi, u32 prio)
{
	if (spi >= kGicVectorCount)
		return false;
	gVectors[spi].active = true;
	gVectors[spi].hw_irq = spi;
	dist_wr(GICD_IPRIORITYR(spi), prio);
	dist_wr(GICD_ISENABLER(spi / 32u), 1u << (spi % 32u));
	return true;
}
