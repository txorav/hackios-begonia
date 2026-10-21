/*
 * regs.h — handy system-register / barrier helpers for xnuloader.
 * Everything goes through inline asm; the loader runs from the moment lk
 * hands control until XNU starts, so it must not touch Apple-proprietary
 * implementation-defined registers (S3_x_c15_* — HID/APRR/PPL/AMX/CPM are
 * all ABSENT on MT6785 Cortex-A76/A55; see docs/hardware_map_mt6785.md).
 */
#ifndef _XLDR_REGS_H_
#define _XLDR_REGS_H_

#define REG(name) "#name"

static inline uint64_t r_cntfrq(void)
{
	uint64_t v; asm volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}

static inline uint64_t r_mpidr(void)
{
	uint64_t v; asm volatile("mrs %0, mpidr_el1" : "=r"(v)); return v;
}

static inline uint32_t r_current_el(void)
{
	uint64_t v; asm volatile("mrs %0, CurrentEL" : "=r"(v));
	return (uint32_t)((v >> 2) & 3);
}

static inline uint64_t r_sctlr_el1(void)
{
	uint64_t v; asm volatile("mrs %0, sctlr_el1" : "=r"(v)); return v;
}

static inline void w_sctlr_el1(uint64_t v)
{
	asm volatile("msr sctlr_el1, %0" :: "r"(v));
}

/* Clean+invalidate the artible full D-cache by set/way, invalidate I-cache. */
void cache_flush_all(void);

/* __builtin equivalents */
#define dmb_sys()   asm volatile("dmb sy" ::: "memory")
#define dsb_sys()   asm volatile("dsb sy" ::: "memory")
#define isb_sys()   asm volatile("isb" ::: "memory")

#endif /* _XLDR_REGS_H_ */
