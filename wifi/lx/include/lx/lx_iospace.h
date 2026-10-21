/*
 * lx_iospace.h — device MMIO accessor layer for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT REPLACES
 * ---------------------------------------------------------------------------
 * Linux                              |  lx equivalent
 * -----------------------------------+-------------------------------------
 * ioremap(phys, len)  -> __iomem*    |  lx_ioremap(ctx, phys, len)
 * iounmap(void __iomem *p)           |  lx_iounmap(ctx, map)
 * writel(val, base + off)            |  lx_writel(val, base, off);
 *                                    |    (base is an lx_iomap* token)
 * readl(base + off)                  |  lx_readl(base, off)
 * readq(base + off) / writeq(...)    |  lx_readq / lx_writeq
 * writel_relaxed / readl_relaxed     |  lx_writel_xor / lx_readl_xor
 *                                    |   ("relaxed" analogy, no barrier)
 * ioread8/16                         |  lx_readb / lx_readw
 * iowrite8/16                        |  lx_writeb / lx_writew
 *
 * ---------------------------------------------------------------------------
 * MMIO MODEL
 * ---------------------------------------------------------------------------
 * Every peripheral the lx layer hosts is a child of MT6785's /arm-io node:
 *
 *   Peripheral     Physical base      Size        (docs/hardware_map_mt6785.md)
 *   -------------- ------------------ -----------
 *   MSDC1 (µSD)    0x11240000         0x10000
 *   UFSHCI (ufs0)  0x11270000         0x100000
 *   I2C0           0x11007000         0x1000
 *   UART0 (debug)  0x11002000         0x1000
 *
 * The MMIO token returned by lx_ioremap is `struct lx_iomap *` — never a raw
 * pointer.  Callers read/write via accessor macros that (a) compose a
 * volatile MMIO load/store against the token's CPU virtual address and
 * (b) put the correct device-side manipulation barriers in place.
 *
 * Barriers (see lx_types.h, "Barrier instructions"):
 *   lx_writel  : write, then `dsb sy` before any following doorbell store on
 *                another device — this is the Linux `writel()` semantics.
 *   lx_readl   : `dsb ish` first then read — Linux `readl()` semantics.
 *   EXCEPT when followed by another barrier (relaxed variants (`_r`)) skip
 *   explicit barriers: open-coded loops (SCSI ring walks, UTP doorbell
 *   step) use relaxed forms and put the one owning barrier themselves.
 */

#ifndef LX_IOSPACE_H
#define LX_IOSPACE_H

#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct lx_context;      /* lxkit_api.h */

/*
 * ---------------------------------------------------------------------------
 * The MMIO token: `struct lx_iomap`:
 *   .phys device physical base (bring-up verification aid)
 *   .len  byte span
 *   .virt CPU kernel VA to compose accessors against
 *   .ctx  owning lxkit context (lifetime hygiene)
 *   .omap OPAQUE: lxkit retains the IOMemoryMap* here; never dereference,
 *         never free manually (lx_iounmap owns it).
 * ------------------------------------------------------------------------- */

/* An established device mapping owned by a driver. */
struct lx_iomap {
	u64   phys;             /* device physical base address              */
	u64   len;              /* mapping length in bytes                   */
	u64   virt;             /* CPU kernel virtual address, valid after   */
	                      /* lxkit maps it (mapDeviceMemoryWithIndex)  */
	struct lx_context *ctx; /* owning lxkit context                      */
	void *omap;             /* OPAQUE: lxkit retains IOMemoryMap here;   */
	                        /* never dereference; NEVER free manually.   */
};

/* ---------------------------------------------------------------------------
 * Mapping
 * ------------------------------------------------------------------------- */
/* phys is physical; len in bytes; virt is 16 KiB page-aligned by contract
 * (XNU maps in 16 KiB chunks on begonia — see porting plan phase 3 §2). */
struct lx_iomap * lx_ioremap(struct lx_context *ctx, u64 phys, u64 len);
void              lx_iounmap(struct lx_context *ctx, struct lx_iomap *iomap);

/* Convenience: offset-in-iomap gateway.  NOT bounds-checked (bring-up code
 * mirrors Linux where __iomem accessors do not check either) — debug builds
 * of the shim (LX_DEBUG_STRICT=1) can turn bounds assertions on; see
 * wifi/lx/README.md "known gaps". */
static inline volatile void * lx_iomap_virt(const struct lx_iomap *iomap)
{
	return (volatile void *)(iomap->virt);
}

/* ---------------------------------------------------------------------------
 * 32-bit accessors (the standard case for every MT6785 peripheral here).
 * ------------------------------------------------------------------------- */
static inline u32 lx_readl(const struct lx_iomap *iomap, u64 off)
{
	volatile u32 *p = (volatile u32 *)((u8 *)iomap->virt + off);
	lx_barrier_dsb();
	return *p;
}

static inline void lx_writel(const struct lx_iomap *iomap, u64 off, u32 v)
{
	volatile u32 *p = (volatile u32 *)((u8 *)iomap->virt + off);
	lx_barrier_dsb();
	*p = v;
}

/* relaxed: no explicit barrier — caller owns ordering. */
static inline u32 lx_readl_r(const struct lx_iomap *iomap, u64 off)
{
	volatile u32 *p = (volatile u32 *)((u8 *)iomap->virt + off);
	return *p;
}

static inline void lx_writel_r(const struct lx_iomap *iomap, u64 off, u32 v)
{
	volatile u32 *p = (volatile u32 *)((u8 *)iomap->virt + off);
	*p = v;
}

/* ---------------------------------------------------------------------------
 * 64-bit accessors — used for UFSHCI UTP list base (L/H pair), DMA engines.
 * arm64 stores 8 bytes atomically if the address is 8-byte-aligned; the
 * drivers only use these on naturally-aligned registers (verifier-checked).
 * ------------------------------------------------------------------------- */
static inline u64 lx_readq(const struct lx_iomap *iomap, u64 off)
{
	volatile u64 *p = (volatile u64 *)((u8 *)iomap->virt + off);
	lx_barrier_dsb();
	return *p;
}

static inline void lx_writeq(const struct lx_iomap *iomap, u64 off, u64 v)
{
	volatile u64 *p = (volatile u64 *)((u8 *)iomap->virt + off);
	lx_barrier_dsb();
	*p = v;
}

/* Both-halves accessors: HCI register pair (low at off, high at off+4) —
 * this mirrors the Linux driver pattern for any 64-bit-wide registration
 * the HCI exposes as two adjacent 32-bit addresses. */
static inline u64
lx_readq_halves(const struct lx_iomap *iomap, u64 lo_off, u64 hi_off)
{
	return (u64)lx_readl(iomap, lo_off) |
	    ((u64)lx_readl(iomap, hi_off) << 32);
}

static inline void
lx_writeq_halves(const struct lx_iomap *iomap, u64 lo_off, u64 hi_off, u64 v)
{
	lx_writel(iomap, hi_off, (u32)(v >> 32));
	lx_writel(iomap, lo_off, (u32)(v & 0xFFFFFFFFUL));
}

/* ---------------------------------------------------------------------------
 * Sub-32-bit accessors — used by I2C (8-bit data register) and UART0 debug.
 * ------------------------------------------------------------------------- */
static inline u8  lx_readb(const struct lx_iomap *iomap, u64 off)
{
	volatile u8 *p = (volatile u8 *)((u8 *)iomap->virt + off);
	lx_barrier_dmb();
	return *p;
}

static inline void lx_writeb(const struct lx_iomap *iomap, u64 off, u8 v)
{
	volatile u8 *p = (volatile u8 *)((u8 *)iomap->virt + off);
	lx_barrier_dmb();
	*p = v;
}

static inline u16 lx_readw(const struct lx_iomap *iomap, u64 off)
{
	volatile u16 *p = (volatile u16 *)((u8 *)iomap->virt + off);
	lx_barrier_dmb();
	return (u16)*p;
}

static inline void lx_writew(const struct lx_iomap *iomap, u64 off, u16 v)
{
	volatile u16 *p = (volatile u16 *)((u8 *)iomap->virt + off);
	lx_barrier_dmb();
	*p = v;
}

/* ---------------------------------------------------------------------------
 * Field-manipulation conveniences used across the register definitions.
 * e.g. lx_field_get(reg, 0x1F, 4) extracts bits [8:4].
 * ------------------------------------------------------------------------- */
static inline u32 lx_field_get(u32 reg, u32 mask, u32 shift)
{
	return (reg >> shift) & ((mask >> shift) | (mask ? (1u) : 0u));
}

static inline u32 lx_field_set(u32 v, u32 mask, u32 shift, u32 val)
{
	return (u32)((v & ~(mask << shift)) | ((val & mask) << shift));
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_IOSPACE_H */
