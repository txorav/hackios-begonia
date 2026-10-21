/*
 * lx_dma.h — DMA buffer allocation for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * MT6785 STORAGE DMA TOPOLOGY (drivers/mt6785 contract)
 * ---------------------------------------------------------------------------
 * MSDC1 (0x11240000) : 32-bit bus DMA; the engine's "descriptor table" is
 *                      a list built in RAM, read by the SDC engine.
 *                      Buffers must be physically contiguous, at most
 *                      32-bit addressed.
 * UFSHCI (0x11270000): UTP transfer-request list (UTCD/UTRL) supports
 *                      scatter-gather via PRDT per command; the command
 *                      list arrays must be 32 slot × 32 bytes aligned (the
 *                      JEDEC UFSHCI 2.1 contract calls for 8-byte
 *                      alignment; we do better and document why).
 * I2C0, touchscreen  : no DMA.
 *
 * On this SoC there is NO IOMMU in front of the storage IPs (no Apple
 * DART-like translation); the "bus address" == physical address, and the
 * "coherent" allocation is a cache-inhibited CPU mapping around the same
 * bytes.  Linux has both `dma_alloc_coherent` (streaming) and
 * `dma_map_single` (one-shot with sync); we only need the coherent
 * path for: msdc descriptor ring, UTP UTRDL/UTMRL arrays, SCSI payloads
 * when we take the PIO path.
 *
 * ---------------------------------------------------------------------------
 * XNU PRIMITIVES IMPLEMENTED IN lxkit/lxkit_mem.cpp
 * ---------------------------------------------------------------------------
 * Linux                                   xnu calls used
 * --------------------------------------- ----------------------------------
 * dma_alloc_coherent(dev, size, &handle)  IOBufferMemoryDescriptor::
 *                                            withOptions(kIOBusMastering
 *                                            | kIOMemoryPhysicallyContiguous
 *                                            | kIODirection... , size, align)
 *                                         + map(kIOMapInhibitCache)
 *                                         + getPhysicalAddress()
 * dma_free_coherent(...)                  free() after complete()/unmapping
 * dma_sync_*_for_cpu/device               cache ops via map semantics
 *                                         (kIOMapInhibitCache avoids this
 *                                         entirely; document in comments).
 * ---------------------------------------------------------------------------
 */

#ifndef LX_DMA_H
#define LX_DMA_H

#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct lx_context;         /* lxkit_api.h */
struct lx_device;          /* lx_dev.h */

/*
 * A coherent DMA region: all three views are pre-computed at allocation
 * time so the drivers do not race the mapper.
 */
struct lx_dma_chunk {
	u64                bus_addr;      /* == physical on MT6785 (no IOMMU) */
	u64                phys_addr;     /* same as bus_addr; kept explicit  */
	void              *cpu_addr;      /* CPU kernel VA, cache-inhibited   */
	size_t             size;
	struct lx_context *ctx;           /* owning context (for malformed-   */
	                                  /* use debugging in lxkit_free)     */
	void              *priv;          /* lxkit-internal mapping records   */
};

/*
 * Allocate `size` bytes (rounded up to 64 alignment) of physically
 * contiguous, cache-inhibited memory.  dma_addr gets the bus address.
 * Drivers must not write the chunk before zeroing if they need determinism.
 */
struct lx_dma_chunk *
lx_dma_alloc_coherent(struct lx_device *dev, size_t size,
    u64 *dma_addr, unsigned flags);

/* Zero-fill after allocation (sugar; avoids caller knowing about branches). */
void lx_dma_zero(struct lx_dma_chunk *chunk);

/* Free a chunk allocated by lx_dma_alloc_coherent. */
void lx_dma_free_coherent(struct lx_dma_chunk *chunk);

/*
 * Sync helpers (act as no-ops on coherent regions; required by mirrored
 * driver code that calls dma_sync_*() on the coherent chunk for clarity).
 */
void lx_dma_sync_for_device(struct lx_dma_chunk *chunk);
void lx_dma_sync_for_cpu(struct lx_dma_chunk *chunk);

/* Linux-name sugar for mirrored drivers. */
#define dma_alloc_coherent(dev, size, dma_addr, flags) \
	lx_dma_alloc_coherent((dev), (size), (dma_addr), (flags))
#define dma_free_coherent(chunk) \
	lx_dma_free_coherent((chunk))

/* Alignment flag used by MSDC1 (descriptor table must be 32 KiB aligned) */
#define LX_DMA_ALIGN_32K  0x0100u
#define LX_DMA_ALIGN_4K   0x0200u

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_DMA_H */
