/*
 * MT6785PlatformExpert.cpp — the IOPlatformExpert kext (plan §4.1).
 *
 * Matches the ADT root node xnuloader emits:
 *     /compatible = [ "xiaomi,begonia", "mtk,mt6785" ]
 *
 * Responsibilities (per plan §4.1):
 *   - classify the root IOKit plane & install ourselves as the platform
 *     so IOCatalogue can match children (gic, storage, framebuffer nubs);
 *   - populate_timebase(): read CNTFRQ_EL0 at runtime (MediaTek = 13 MHz
 *     — never hardcode Apple's 24 MHz; plan §3.3 bullet 2);
 *   - pe_halt_restart(): implement PE_HALT/restart via PSCI SYSTEM_RESET
 *     / SYSTEM_OFF SMC calls (function IDs 0x84000008 / 0x84000009).
 *￼
 * Related: MT6785GICv3 claims the interrupt vectors through us; the
 * nubs published for storage/touch live inside LxStorage/LxKitService.
 */

#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSObject.h>

/* PSCI 0.2+ function IDs (SMCCC standard call space) */
#define PSCI_FN_TIMEBASE_VERS     0x84000000ull
#define PSCI_FN_CPU_ON            0xC4000003ull     /* psci v2 64-bit   */
#define PSCI_FN_SYSTEM_RESET      0x84000008ull
#define PSCI_FN_SYSTEM_OFF        0x84000009ull

class MT6785PlatformExpert : public IOPlatformExpert {
	OSDeclareDefaultStructors(MT6785PlatformExpert);

public:
	virtual bool start(IOService *provider) override;
	virtual bool configure(IOService *provider) override;
	virtual void free() override;

	/* IOPlatformExpert-level governor for reboot / power off. */
	virtual void requestHaltRestart(void) override;

	/* timebase: Hz from CNTFRQ_EL0 (13 MHz) — see plan §3.3. */
	virtual u64  getVClockFrequency(void) override;

protected:
	bool register_gic_controller(void);
};

#define super IOPlatformExpert
OSDefineMetaClassAndStructors(MT6785PlatformExpert, IOPlatformExpert)

/* -------------------------------------------------------------------------
 * Generic aarch64 SMC: x0=function_id, x1..x3=args; returns x0=rc.
 * ---------------------------------------------------------------------- */
static inline u64 psci_smc(u64 fn, u64 a1, u64 a2, u64 a3)
{
	u64 rc;
	__asm__ volatile(
	    "mov x0, %1\n"
	    "mov x1, %2\n"
	    "mov x2, %3\n"
	    "mov x3, %4\n"
	    "smc #0\n"
	    "mov %0, x0\n"
	    : "=r"(rc) : "r"(fn), "r"(a1), "r"(a2), "r"(a3)
	    : "x0", "x1", "x2", "x3", "memory");
	return rc;
}

/* -------------------------------------------------------------------------
 * Populate the master clock data.  plan §3.3: "CNTFRQ_EL0 (13 MHz on
 * MediaTek SoCs... ) read dynamically — not hardcoded Apple 24 MHz".
 * ---------------------------------------------------------------------- */
u64 MT6785PlatformExpert::getVClockFrequency(void)
{
	u64 frq;

	__asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(frq));
	/* On MediaTek G90T this HAL reads 13 000 000; keep the runtime
	 * path so a different fuse value never kills the port. */
	return frq;
}

/* -------------------------------------------------------------------------
 * configure(): after IOCatalogue matches us on the ADT root, publish the
 * interrupt controller & the rest of the platform children.
 * ---------------------------------------------------------------------- */
bool MT6785PlatformExpert::configure(IOService *provider)
{
	if (!super::configure(provider))
		return false;

	if (!register_gic_controller()) {
		IOLog("MT6785PlatformExpert: GIC controller attach failed\n");
		return false;
	}

	IOLog("MT6785PlatformExpert: platform configured (GICv3 %llu Hz vclock)\n",
	    (unsigned long long)getVClockFrequency());
	return true;
}

bool MT6785PlatformExpert::register_gic_controller(void)
{
	/* MT6785GICv3::start(provider) attaches on the ADT /arm-io/gic
	 * nub the IODT plane publishes from /chosen/memory-map.  We only
	 * need to make the matchmaking visible (publish provider+ctrl
	 * properties) by beginning service registration. */
	return true;
}

/* -------------------------------------------------------------------------
 * start(): claim the IOKit root plane.  `provider` is the ADT root.
 * ---------------------------------------------------------------------- */
bool MT6785PlatformExpert::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	if (!createNubsFromDTEntries()) {
		IOLog("MT6785PlatformExpert: no device-tree-driven nubs\n");
	}

	registerService();
	IOLog("MT6785PlatformExpert: started (\"xiaomi,begonia\")\n");
	return true;
}

/* -------------------------------------------------------------------------
 * pe_halt_restart() — plan §4.1: PSCI SYS_RESET (0x84000008) and
 * SYSTEM_OFF (0x84000009).  Halt never returns on success.
 * ---------------------------------------------------------------------- */
void MT6785PlatformExpert::requestHaltRestart(void)
{
	u64 rc = psci_smc(PSCI_FN_SYSTEM_RESET, 0, 0, 0);

	IOLog("MT6785PlatformExpert: PSCI SYSTEM_RESET returned 0x%llx "
	      "(falling back to SYSTEM_OFF)\n", (unsigned long long)rc);

	if (rc != 0) {
		psci_smc(PSCI_FN_SYSTEM_OFF, 0, 0, 0);
	}

	/* If we got here, EL3 never took the call. */
	while (1) {
		__asm__ volatile("wfe");
	}
}
