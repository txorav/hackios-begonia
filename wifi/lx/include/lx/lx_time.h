/*
 * lx_time.h — time source for the lx shim layer (jiffies, ktime, delays).
 *
 * ---------------------------------------------------------------------------
 * TIME MODEL
 * ---------------------------------------------------------------------------
 * The MT6785 has the ARM Architectural Generic Timer (docs
 * hardware_map_mt6785.md §1): CNTFRQ_EL0 == 13_000_000 Hz on MediaTek parts.
 * We therefore expose:
 *
 *   lx_ktime_get_ns()      — nanoseconds since kernel boot (read from the
 *                            generic counter via CNTVCT_EL0/CNTFRQ_EL0;
 *                            routed through a function pointer so that
 *                            lxkit can swap in XNU clock primitives later
 *                            without a driver rebuild).
 *   lx_ktime_get_raw_ns()  — same clock, guaranteed unadjusted (same impl,
 *                            but no offset correction).
 *   jiffies                — global tick counter at LX_HZ (250 Hz), a
 *                            *derived* quantity: jiffies = uptime_ns * HZ
 *                            / 1e9.  We keep the Linux variable shape for
 *                            the mirrored drivers, but never free-run a
 *                            tick source of our own (lxkit's timer event
 *                            source is the only consumer).
 *
 * Frequency notes:
 *  - HZ 250 was chosen to match typical recent MTK Android configs and
 *    reduces timer fire overhead vs HZ 1000 on single-big-core critical
 *    path (the A76 pair is the CPU that runs the lxkit workloop — see the
 *    "affinity note" in wifi/lx/README.md).
 *  - The only *hard* requirement from XNU: the panic/debug subsystem can be
 *    sensitive to self-check loops that must NOT consume CPU forever —
 *    lx_mdelay()/lx_udelay() spin on the counter, never on jiffies, so a
 *    dead timer cannot hang an mdelay.
 */

#ifndef LX_TIME_H
#define LX_TIME_H

#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Tick frequency: match the XNU port target (see porting plan §3.3). */
#define LX_HZ 250
#define LX_JIFFIES_FREQ (1ULL * LX_HZ)

/* MediaTek generic timer frequency — read from CNTFRQ_EL0 at runtime. */
#define LX_MTK_TIMER_HZ_DEFAULT  13000000ULL

/* ---------------------------------------------------------------------------
 * Underlying clock provider injection
 * ------------------------------------------------------------------------- */

/* The producer of uptime ns.  Instanced by lxkit (kernel):
 *    lx_time_set_provider(lx_mtk_generic_timer_ns?)
 * The provider MUST be monotonic.  NULL provider ⇒ 0 (callers must treat
 * a zero uptime as "clock not initialized" and bail out of polling, never
 * loop).  Host tests install a fake provider with a controlled advance. */
typedef u64 (*lx_time_provider_t)(void);

void lx_time_set_provider(lx_time_provider_t provider);
u64  lx_ktime_get_ns(void);       /* ns since boot                       */
u64  lx_ktime_get_raw_ns(void);   /* same clock, raw value               */
u64  lx_time_get_timer_hz(void);  /* CNTFRQ value in use, for debug      */
void lx_time_set_timer_hz(u64 hz);

/* ---------------------------------------------------------------------------
 * jiffies (the mirror-shaped global; see block comment)
 * ------------------------------------------------------------------------- */
extern volatile unsigned long jiffies;

static inline unsigned long lx_jiffies(void)
{
	return (unsigned long)jiffies;
}

/* jiffies delta helpers used by every timeout in the mirrored drivers. */
static inline int lx_time_after(unsigned long a, unsigned long b)
{
	/* signed wraparound-safe compare (classic kernel Jiffies compare) */
	return (long)(b - a) < 0;
}
static inline int lx_time_before(unsigned long a, unsigned long b)
{
	return lx_time_after(b, a);
}

/* Convert to/from jiffies */
#define LX_MSEC_TO_JIFFIES(ms)  ((unsigned long)((ms) * LX_HZ / 1000UL))
#define LX_JIFFIES_TO_MSEC(j)   ((unsigned long)((j) * 1000UL / LX_HZ))
#define LX_USEC_TO_JIFFIES(us)  ((unsigned long)((us) * LX_HZ / 1000000UL))

/* Deadline helper: out is "expiry" value to compare against lx_time_after(). */
#define lx_jiffies_deadline_after(ms) \
	((unsigned long)jiffies + LX_MSEC_TO_JIFFIES((ms)))

/* ---------------------------------------------------------------------------
 * ktime helpers (Linux shape; 64-bit ns basis)
 * ------------------------------------------------------------------------- */
static inline u64
lx_ktime_add_ns(u64 base_ns, u64 add_ns)
{
	return base_ns + add_ns;
}

static inline u64
lx_ktime_sub_ns(u64 base_ns, u64 sub_ns)
{
	return (base_ns > sub_ns) ? (base_ns - sub_ns) : 0;
}

/* ---------------------------------------------------------------------------
 * Sleeping/spinning delays
 * ------------------------------------------------------------------------- */

/* For bring-up only: spin-wait for `us` using the counter.  Drivers must */
/* never call this for us >= 1000 inside the interrupt path (use the      */
/* workqueue instead, lx_work.h).                                          */
void lx_udelay(u32 us);
void lx_mdelay(u32 ms);
void lx_msleep(u32 ms);     /* available, but prefer lx_work for real sleep */

/* --------------------------------------------------------------------------- */
/* "Check clock before use" helper — mirrors the uboot-style sanity guard.     */
static inline int lx_time_ready(void)
{
	lx_barrier_dmb();
	return jiffies != 0;   /* jiffies fed => a provider is installed */
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_TIME_H */
