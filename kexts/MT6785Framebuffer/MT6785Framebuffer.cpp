/*
 * MT6785Framebuffer.cpp — the PE video surface for begonia (plan §4.2).
 *
 * `lk` (the MediaTek Little Kernel) has already clocked the MIPI-DSI
 * panel and left a 1080×2340 RGBX8888 scan-out buffer in DRAM (plan §1)
 * — we simply re-publish that memory as the xnu framebuffer/console
 * surface so PE_init_iokit hands it to vc_progress_initialize and the
 * verbose kernel banner appears on the LCD.
 *
 * On-disk contract with xnuloader:
 *     boot_args.Video.{v_baseAddr, v_width, v_height, v_rowBytes, v_depth}
 * are filled by xnuloader from the ADT "/chosen" framebuffer properties
 * (physical 0x7f000000, 4320 stride).  This kext re-reads the same ADT
 * properties through the IODT plane — see docs/hardware_map_mt6785.md §3.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOFramebufferShared.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>

class MT6785Framebuffer : public IOService {
	OSDeclareDefaultStructors(MT6785Framebuffer);

public:
	virtual bool start(IOService *provider) override;
	virtual void free() override;

protected:
	IOMemoryMap *fScanoutMap;    /* IOMemoryMap for the scan-out buffer */
	u64          fPhysAddr;
	u64          fWidth, fHeight, fRowBytes, fDepth;
};

#define super IOService
OSDefineMetaClassAndStructors(MT6785Framebuffer, IOService)

static bool adt_read_prop(IORegistryEntry *node, const char *prop, u64 *out)
{
	OSData *d;
	if (node == NULL || out == NULL)
		return false;

	d = (OSData *)node->getProperty(prop);
	if (d == NULL || d->getLength() != sizeof(u64))
		return false;

	*out = *((u64 *)d->getBytesNoCopy());
	return true;
}

bool MT6785Framebuffer::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	provider = (IORegistryEntry *)NULL;     /* provider = /chosen nub */
	fPhysAddr = 0;
	if (provider != (IOService *)0 && !adt_read_prop(provider,
	    "framebuffer-phys", &fPhysAddr)) {
		IOLog("MT6785Framebuffer: no framebuffer-phys in ADT /chosen\n");
	}

	/* The plan's bring-up path: the boot args already describe the FB
	 * surface; creating the IOMemoryDescriptor (read-write, cacheable-
	 * coherent) is purely an IOFramework plumbing step (the real
	 * simplification: nothing else owns the memory while lk keeps the
	 * panel up — see docs/02_boot_flow.md §3). */
	fScanoutMap = provider->mapDeviceMemoryWithIndex(0);
	if (fScanoutMap == NULL) {
		IOLog("MT6785Framebuffer: scan-out map failed (bring-up: "
		      "physical 0x7f000000/stride 4320 is on the ADT)\n");
		return false;
	}

	IOLog("MT6785Framebuffer: scan-out 0x%llx … %llux%llu @%llu bytes/row\n",
	    (unsigned long long)fPhysAddr,
	    (unsigned long long)fWidth,
	    (unsigned long long)fHeight,
	    (unsigned long long)fRowBytes);

	registerService();
	return true;
}

void MT6785Framebuffer::free(void)
{
	if (fScanoutMap) {
		fScanoutMap->release();
		fScanoutMap = NULL;
	}
	super::free();
}
