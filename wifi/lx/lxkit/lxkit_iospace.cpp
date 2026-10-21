/*
 * lxkit_iospace.cpp — MMIO engine: ioremap-backed `struct lx_iomap`.
 *
 * Maps the /arm-io device register ranges into CPU kernel VA space:
 *
 *   MSDC1  0x11240000 (64 KiB)   UFSHCI 0x11270000 (1 MiB)
 *   I2C0   0x11007000 (4 KiB)    UART0  0x11002000 (debug)
 *
 * The owning IOService (LxKitService in the host image, or the bound
 * LxKitTouchNub in the touch image) provides the IODeviceMemory context
 * so the map participates in IOKit bookkeeping (IOService::mapDevice…).
 * The retained IOMemoryMap lands in lx_iomap.omap; release is symmetric.
 */

#include "lxkit_types.h"

#include "libkern/c++/OSMetaClass.h"
#include "IOKit/IOLib.h"
#include "IOKit/IOMemoryDescriptor.h"
#include "IOKit/IOMapTypes.h"
#include "IOKit/IODeviceMemory.h"
#include "IOKit/IOService.h"

#include "wifi/lx/include/lx/lx_iospace.h"
#include "wifi/lx/include/lx/lx_dev.h"
#include "wifi/lx/include/lx/lx_errno.h"

void lxkit_iospace_attach(struct lxkit_glue *g)
{
	(void)g;     /* stateless; mappings live in each lx_device */
}

/* ==========================================================================
 * lx_ioremap / lx_iounmap
 * ========================================================================= */
struct lx_iomap *
lx_ioremap(struct lx_context *ctx, u64 phys, u64 len)
{
	struct lxkit_glue *g = lxkit_glue_of(ctx);
	struct lx_iomap *iom;
	IODeviceMemory *dm;
	IOMemoryMap *map = NULL;

	if (g == NULL || g->owner == NULL || len == 0)
		return NULL;

	dm = IODeviceMemory::withRange(
	    (IOPhysicalAddress)phys, (IOPhysicalLength)len);
	if (dm == NULL)
		return NULL;

	map = dm->map(kIOMapAnywhere);
	dm->release();                    /* mapping now owns the descriptor */
	if (map == NULL)
		return NULL;

	iom = (struct lx_iomap *)IOMallocZero(sizeof(*iom));
	if (iom == NULL) {
		map->release();
		return NULL;
	}
	iom->phys = phys;
	iom->len = len;
	iom->virt = map->getVirtualAddress();
	iom->ctx = ctx;
	iom->omap = (void *)map;          /* retained; released in unmap */
	return iom;
}

void
lx_iounmap(struct lx_context *ctx, struct lx_iomap *iomap)
{
	IOMemoryMap *map;

	if (iomap == NULL)
		return;

	map = (IOMemoryMap *)iomap->omap;
	iomap->omap = NULL;

	if (map != NULL)
		map->release();
	/* Zero before free to surface use-after-free during bring-up. */
	lx_memset(iomap, 0, sizeof(*iomap));
	IOFree(iomap, sizeof(*iomap));
}

/* release-by-device: symmetry for LxKitDeviceNub::free() and FocalTouch
 * glue teardown.  (see lxkit_service/lxkit touch glue callers) */
void
lxkit_iomap_release_for_dev(struct lx_device *dev)
{
	unsigned i;

	if (dev == NULL)
		return;
	for (i = 0; i < dev->niomaps; i++) {
		if (dev->iomaps[i] == NULL)
			continue;
		lx_iounmap(dev->ctx, dev->iomaps[i]);
		dev->iomaps[i] = NULL;
	}
	dev->niomaps = 0;
}
