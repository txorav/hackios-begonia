/*
 * lx_kmem.h — memory allocation + physical/virtual mapping primitives for
 *             the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * ARCHITECTURE DECISION (xnu-backed memory)
 * ---------------------------------------------------------------------------
 * Every allocation in the lx layer advances through IOBufferMemoryDescriptor
 * (IOMemoryDescriptor subclass, in-tree at
 *   xnu/iokit/IOKit/IOBufferMemoryDescriptor.h)
 * so the SAME descriptor can back both:
 *   (a) the CPU view (prepare()/getVirtualAddress()), and
 *   (b) the bus/physical view handed to the MSDC/UFS DMA engines
 *       (getPhysicalAddress() — phys == bus addr on MT6785: no IOMMU is
 *       present for the storage IPs, no DART-remap like Apple A11).
 *
 * The "context" parameter (struct lx_context *) is defined in lxkit_api.h;
 * it carries the per-lxkit glue state.  Kernel-side implementations live in
 * lxkit/lxkit_mem.cpp; host test implementations live in wifi/lx/tests.
 *
 * Linux call used  |  lx call in this file      | xnu primitive used
 * -----------------+----------------------------+--------------------------
 * kmalloc GFP_NOM  | lx_kmalloc(ctx, size,      | IOMalloc / IOMallocZero
 *                  |     lx_flags)              |
 * kzalloc          | lx_kzalloc(ctx, n, gx)     | IOMallocZero
 * kfree            | lx_kfree(ctx, ptr)         | IOFree
 * vmalloc          | lx_vmalloc(ctx, n)         | not needed (contiguous
 *                  |                            | fallback to kmalloc)
 * dma_alloc_       | see lx_dma.h (coherent,    | IOBufferMemoryDescriptor
 *   coherent       |  descriptor-backed)        |
 * page_address     | lx_page_virt(ctx, fi)      | descriptor getVirtualAddr
 * getBilling pages | lx_page_phys(ctx, vi)      | getPhysicalAddress()
 *
 * CALLING RULES:
 *  - No GFP __GFP_ZERO distinct flag; zeroing always requested explicitly.
 *  - Never return NULL from a driver's own "cleanup" path without logging.
 *  - All pointers returned are CPU kernel virtual addresses; mapping to
 *    device bus addresses is the lx_dma.h job.
 */

#ifndef LX_KMEM_H
#define LX_KMEM_H

#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Allocation flags (Linux shapd, subset used by the ported drivers). */
#define LX_GFP_KERNEL  0x01u   /* may block (the xnu IOMalloc path waits) */
#define LX_GFP_NOWAIT  0x02u   /* must not block                          */
#define LX_GFP_ZERO    0x04u   /* zero the allocation                     */
#define LX_GFP_DMA32   0x08u   /* fit in first 4 GiB — required by MSDC1  */
                               /* DMA engine (32-bit bus address)         */

/* Allocation context: comes from lxkit (kernel) or the host test mock. */
struct lx_context;

/* ---------------------------------------------------------------------------
 * kmalloc family
 * ------------------------------------------------------------------------- */

/* kmalloc that may block; guaranteed at least 4-byte alignment. */
void *lx_kmalloc(struct lx_context *ctx, size_t size, unsigned int flags);

/* kzalloc: zeroed kmalloc.  Sugar in Linux, same here. */
void *lx_kzalloc(struct lx_context *ctx, size_t size, unsigned int flags);

/* kfree: size IT must match the allocation's owning context (we tag size
 * internally in kernel builds to catch mismatches — see lxkit_mem.cpp). */
void lx_kfree(struct lx_context *ctx, void *ptr);

/* Returns allocation size if you lost it (kernel side only). */
size_t lx_ksize(struct lx_context *ctx, void *ptr);

/* memset/memcpy/memmove with the Linux names (freestanding impls, shared
 * with host tests; the kernel builds contact the libkern routines? no —
 * small clean-room versions to be CPU-agnostic). */
void lx_memset(void *dst, int c, size_t n);
void lx_memcpy(void *dst, const void *src, size_t n);
void lx_memmove(void *dst, const void *src, size_t n);
int  lx_memcmp(const void *a, const void *b, size_t n);

/* Inline IOMumerator-friendly memcpy/demux helpers for DMA ping-pong. */
void lx_memcpy_from_device(void *cpu_dst, const volatile void *mmio_src,
    size_t n);
void lx_memcpy_to_device(volatile void *mmio_dst, const void *cpu_src,
    size_t n);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_KMEM_H */
