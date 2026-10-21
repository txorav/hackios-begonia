/*
 * lxkit_mem.cpp — union of the memory engine:
 *   (a) lx_kmem.h   (kmalloc family + freestanding mem ops)
 *   (b) lx_dma.h    (dma_alloc_coherent / free / sync)
 *
 * All DMA memory is cache-inhibited (kIOMapInhibitCache) and physically
 * contiguous — the correct shape for the two engines this layer hosts:
 *
 *   MSDC1 @ 0x11240000 — 32-bit bus DMA (GPDMA descriptor chains).
 *   UFSHCI @ 0x11270000 — UTP UCDL/UTRL slot arrays (64-bit capable).
 *
 * IOBufferMemoryDescriptor plumbing reference (in-tree):
 *   xnu/iokit/IOKit/IOBufferMemoryDescriptor.h
 *      withOptions(options, capacity, alignment)
 *   xnu/iokit/IOKit/IOMemoryDescriptor.h (IOMemoryOptions)
 *      kIODirectionInOut/In/Out, kIOMemoryPhysicallyContiguous
 *   kIOMapInhibitCache — IOMapTypes.h (cache inhibit on the map()).
 */

#include "lxkit_types.h"

#include "libkern/c++/OSMetaClass.h"
#include "IOKit/IOLib.h"
#include "IOKit/IOMemoryDescriptor.h"
#include "IOKit/IOBufferMemoryDescriptor.h"
#include "IOKit/IOMapTypes.h"

#include "wifi/lx/include/lx/lx_kmem.h"
#include "wifi/lx/include/lx/lx_dma.h"
#include "wifi/lx/include/lx/lx_errno.h"
#include "wifi/lx/include/lx/lx_dev.h"
#include "wifi/lx/include/lx/lxkit_api.h"

void lxkit_mem_attach(struct lxkit_glue *g)
{
	(void)g;      /* currently stateless; kept for future pools */
}

/* ==========================================================================
 * kmalloc family on IOMalloc / kalloc heap
 *
 * Size bookkeeping: we keep the allocation size in front of each block
 * (kalloc-size tracking) so lx_ksize works and kfree is size-correct.
 *   layout: [size_t size][user bytes]
 * ========================================================================== */
struct lx_kmem_hdr {
	size_t             size;
	struct lx_context *ctx;
};

static inline struct lx_kmem_hdr * lx_kmem_hdr_of(void *user)
{
	return (struct lx_kmem_hdr *)((u8 *)user - sizeof(struct lx_kmem_hdr));
}

void *
lx_kmalloc(struct lx_context *ctx, size_t size, unsigned int flags)
{
	struct lx_kmem_hdr *h;
	struct lx_context *c = (ctx != NULL) ? ctx : lxkit_default_context();

	(void)(lxkit_check_context(NULL) != LX_OK);   /* context optional */

	if (size == 0)
		return NULL;
	if (size > (128llu * 1024llu * 1024llu)) {
		/* refuse absurd allocations early (bring-up guard) */
		printk(KERN_ERR "lxkmem: refusing size %llu\n",
		    (unsigned long long)size);
		return NULL;
	}

	h = (struct lx_kmem_hdr *)IOMalloc(sizeof(*h) + size);
	if (h == NULL)
		return NULL;
	h->size = size;
	h->ctx = c;

	if ((flags & LX_GFP_ZERO) != 0)
		lx_memset((u8 *)h + sizeof(*h), 0, size);
	return (u8 *)h + sizeof(*h);
}

void *
lx_kzalloc(struct lx_context *ctx, size_t size, unsigned int flags)
{
	return lx_kmalloc(ctx, size, flags | LX_GFP_ZERO);
}

void
lx_kfree(struct lx_context *ctx, void *ptr)
{
	struct lx_kmem_hdr *h;
	(void)ctx;

	if (ptr == NULL)
		return;
	h = lx_kmem_hdr_of(ptr);
	IOFree(h, h->size + sizeof(*h));
}

size_t
lx_ksize(struct lx_context *ctx, void *ptr)
{
	(void)ctx;
	if (ptr == NULL)
		return 0;
	return lx_kmem_hdr_of(ptr)->size;
}

/* ==========================================================================
 * freestanding mem-ops (identical semantics to libc; self-owned)
 * ========================================================================== */
void
lx_memset(void *dst, int c, size_t n)
{
	u8 *d = (u8 *)dst;
	while (n-- != 0)
		*d++ = (u8)c;
}

void
lx_memcpy(void *dst, const void *src, size_t n)
{
	u8 *d = (u8 *)dst;
	const u8 *s = (const u8 *)src;

	if (d == s || n == 0)
		return;
	if (d < s || d > (const u8 *)src + n) {
		/* forward copy when no overlap-danger */
		while (n-- != 0)
			*d++ = *s++;
		return;
	}

	/* overlapping downward: move from the tail */
	d += n;
	s += n;
	while (n-- != 0)
		*--d = *--s;
}

void
lx_memmove(void *dst, const void *src, size_t n)
{
	lx_memcpy(dst, src, n);       /* lx_memcpy is overlap-safe above */
}

int
lx_memcmp(const void *a, const void *b, size_t n)
{
	const u8 *pa = (const u8 *)a;
	const u8 *pb = (const u8 *)b;

	while (n-- != 0) {
		if (*pa != *pb)
			return (int)*pa - (int)*pb;
		pa++;
		pb++;
	}
	return 0;
}

/* ==========================================================================
 * DMA: physically contiguous + cache-inhibited.
 *
 * IOBufferMemoryDescriptor::withOptions() — options bitmask (see
 * IOMemoryDescriptor.h/IOMapTypes.h):
 *    kIODirectionInOut | kIOMemoryPhysicallyContiguous
 * then map(kIOMapInhibitCache) for the CPU view.
 * ========================================================================= */
struct lx_dma_handle {
	IOBufferMemoryDescriptor *md;
	IOMemoryMap              *map;
};

static struct lx_dma_handle *
lx_dma_map_for(struct lx_device *dev, size_t size, size_t alignment)
{
	IOBufferMemoryDescriptor *md;
	IOMemoryMap *m;
	struct lx_dma_handle *h;

	if (dev == NULL)
		return NULL;
	if (alignment == 0)
		alignment = 64;           /* 64 is the nicest min align */

	md = IOBufferMemoryDescriptor::withOptions(
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous,
	    (IOByteCount)size, (IOByteCount)alignment);
	if (md == NULL)
		return NULL;

	m = md->map(kIOMapInhibitCache);
	if (m == NULL) {
		md->release();
		return NULL;
	}

	h = (struct lx_dma_handle *)IOMallocZero(sizeof(*h));
	if (h == NULL) {
		m->release();
		md->release();
		return NULL;
	}
	h->md = md;
	h->map = m;
	return h;
}

struct lx_dma_chunk *
lx_dma_alloc_coherent(struct lx_device *dev, size_t size,
    u64 *dma_addr, unsigned flags)
{
	struct lx_dma_handle *h;
	struct lx_dma_chunk *c;

	if (dev == NULL || size == 0)
		return NULL;

	/* 32-bit-limited engines: MSDC1 uses 32-bit bus addresses.  Enforce
	 * the constraint by asking IOKit for an allocation that satisfies it
	 * (physically contiguous + alignment) and reject if it lands above
	 * the 4 GiB boundary. */
	h = lx_dma_map_for(dev, size,
	    (flags & LX_DMA_ALIGN_32K) ? 32768 :
	    (flags & LX_DMA_ALIGN_4K) ? 4096 : 64);
	if (h == NULL)
		return NULL;

	u64 phys = (u64)h->md->getPhysicalAddress();
	if ((flags & LX_GFP_DMA32) != 0 && phys >= 0x100000000ULL) {
		printk(KERN_ERR "lx-dma: alloc above 4GiB (0x%llx); "
		    "cannot satisfy DMA32\n", (unsigned long long)phys);
		h->map->release();
		h->md->release();
		IOFree(h, sizeof(*h));
		return NULL;
	}

	c = (struct lx_dma_chunk *)IOMallocZero(sizeof(*c));
	if (c == NULL) {
		h->map->release();
		h->md->release();
		IOFree(h, sizeof(*h));
		return NULL;
	}
	c->bus_addr = phys;
	c->phys_addr = phys;                    /* no IOMMU: they are the same */
	c->cpu_addr = (void *)h->map->getVirtualAddress();
	c->size = size;
	c->ctx = dev->ctx;
	c->priv = h;

	if (dma_addr != NULL)
		*dma_addr = c->bus_addr;
	return c;
}

void
lx_dma_free_coherent(struct lx_dma_chunk *chunk)
{
	struct lx_dma_handle *h;

	if (chunk == NULL)
		return;

	h = (struct lx_dma_handle *)chunk->priv;
	if (h != NULL) {
		if (h->map != NULL)
			h->map->release();
		if (h->md != NULL)
			h->md->release();
		IOFree(h, sizeof(*h));
	}
	/* Zero the chunk first to catch use-after-free early in bring-up. */
	lx_memset(chunk, 0, sizeof(*chunk));
	IOFree(chunk, sizeof(*chunk));
}

void
lx_dma_zero(struct lx_dma_chunk *chunk)
{
	if (chunk == NULL || chunk->cpu_addr == NULL)
		return;
	lx_memset(chunk->cpu_addr, 0, chunk->size);
}

void
lx_dma_sync_for_device(struct lx_dma_chunk *chunk)
{
	/* Cache-inhibited => no sync required. */
	(void)chunk;
}

void
lx_dma_sync_for_cpu(struct lx_dma_chunk *chunk)
{
	(void)chunk;
}
