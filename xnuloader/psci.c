/*
 * psci.c — ARM Power State Coordination Interface calls (SMC conduit).
 * MediaTek BL31 (EL3) implements PSCI ≥ 0.2 on mt6785.
 * IDs per docs/hardware_map_mt6785.md §1.
 */
#include "psci.h"

int64_t
psci_call(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 asm("x0") = fid;
	register uint64_t x1 asm("x1") = a1;
	register uint64_t x2 asm("x2") = a2;
	register uint64_t x3 asm("x3") = a3;
	asm volatile("smc #0"
		     : "+r"(x0)
		     : "r"(x1), "r"(x2), "r"(x3)
		     : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
		       "x12", "x13", "x14", "x15", "x16", "x17", "x18");
	return (int64_t)x0;
}

int64_t psci_version(void) { return psci_call(PSCI_VERSION, 0, 0, 0); }

int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t ctx)
{
	return psci_call(PSCI_CPU_ON_SMC64, mpidr, entry, ctx);
}

int64_t psci_system_off(void) { return psci_call(PSCI_SYSTEM_OFF, 0, 0, 0); }
int64_t psci_system_reset(void) { return psci_call(PSCI_SYSTEM_RESET, 0, 0, 0); }
