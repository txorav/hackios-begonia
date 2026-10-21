/*
 * lxkit_service.cpp — LxKitService implementation (lxkit ↔ IOKit bridge).
 *
 * ---------------------------------------------------------------------------
 * COMPILE-TIME REQUIREMENTS
 * ---------------------------------------------------------------------------
 * Compiled as part of kexts/LxStorage (the lxkit "host" kext image) against
 * the in-tree XNU headers (see kexts/LxStorage/Makefile for the exact include
 * floor and the LDFLAGS ordering reserved for the lxkit+driver objects).
 *
 * Every IOKit API used here is verified against this tree:
 *   IOService::start/stop/registerService/getWorkLoop   (IOService.h)
 *   IORegistryIterator::iterateOver/getNextObject/release
 *        (IORegistryEntry.h:1027)      ; getName(plane) (IORegistryEntry.h:812)
 *   getProperty/setProperty (IORegistryEntry.h:415/…)
 *   IODeviceMemory::withRange (IODeviceMemory.h:81)
 *   IOWorkLoop::workLoop()  (IOWorkLoop.h:206)
 *   IOService::serviceMatching/nameMatching/copyMatchingService
 *        (IOService.h:990/1034/977)
 *
 * START-UP FLOW (see header §START-UP FLOW)
 * ---------------------------------------------------------------------------
 */

#include "lxkit_service.h"
#include "lxkit_types.h"

#include "IOKit/IOWorkLoop.h"
#include "IOKit/IOService.h"
#include "IOKit/IODeviceTreeSupport.h"
#include "IOKit/IORegistryEntry.h"
#include "IOKit/IOKitKeys.h"
#include "IOKit/IOLib.h"
#include "libkern/c++/OSMetaClass.h"
#include "libkern/c++/OSData.h"
#include "libkern/c++/OSArray.h"
#include "libkern/c++/OSSymbol.h"
#include "libkern/c++/OSString.h"
#include "libkern/c++/OSDictionary.h"
#include "libkern/c++/OSNumber.h"

/* lx headers (C glue surface) */
#include "wifi/lx/include/lx/lxkit_api.h"
#include "wifi/lx/include/lx/lx_dev.h"
#include "wifi/lx/include/lx/lx_work.h"
#include "wifi/lx/include/lx/lx_printk.h"
#include "wifi/lx/include/lx/lx_kmem.h"
#include "wifi/lx/include/lx/lx_errno.h"

/* The GIC kext (kernel-port) publishes an IOInterruptController under this
 * name; consumers contract with it via the standard IOInterruptControllers
 * property.  This can be overridden per-boot via the lxkit registry prop
 * "lx-gic-name". */
#define LXKIT_GIC_CTRL_NAME_DEFAULT "mtk-gicv3"

/* ==========================================================================
 * LxKitService
 * ========================================================================== */
OSDefineMetaClassAndStructors(LxKitService, IOService);

static LxKitService *g_lxkit_service = NULL;   /* the singleton */

bool
LxKitService::init(OSDictionary *dictionary)
{
	if (super::init(dictionary) != true)
		return false;
	fLXCInited = false;
	fDevicesLive = false;
	fStartCount = 0;
	return true;
}

void
LxKitService::free()
{
	if (fLXCInited == true) {
		lxkit_core_fini(&fLXC);
		fLXCInited = false;
	}
	if (fWorkLoop != NULL) {
		fWorkLoop->release();     /* balance the workLoop() factory */
		fWorkLoop = NULL;
	}
	super::free();
}

IOWorkLoop *
LxKitService::getWorkLoop() const
{
	return fWorkLoop;         /* created in start(); never NULL after */
}

bool
LxKitService::start(IOService *provider)
{
	IOReturn rc;
	IOLog("lxkit: LxKitService starting (provider=%s, abi=%d)\n",
	    provider != NULL ? provider->getName() : "NULL",
	    (int)LXKIT_ABI_VERSION);

	if (fStartCount++ > 0) {
		/* Idempotent on re-match against the same provider. */
		IOLog("lxkit: LxKitService already started; no-op\n");
		return true;
	}
	if (super::start(provider) != true)
		return false;

	fPlatform = provider;

	/* (1) context init (pure C struct) */
	if (lxkit_core_init(&fLXC, "lxkit-host") != LX_OK) {
		IOLog("lxkit: lxkit_core_init failed\n");
		return false;
	}
	fLXCInited = true;

	/* (2) the owned workloop.  All lxkit event sources (interrupt
	 * sources, command gates, timers) attach to THIS loop; consumers
	 * reach it through getWorkLoop().  The workLoop() factory returns
	 * a caller-owned reference (released in free()). */
	fWorkLoop = IOWorkLoop::workLoop();
	if (fWorkLoop == NULL) {
		IOLog("lxkit: IOWorkLoop::workLoop() failed\n");
		return false;
	}

	/* (3) IOKit glue — one call installs console+mem+iospace+irq+work */
	rc = lxkit_glue_create(&fLXC, this, fWorkLoop);
	if (rc != kIOReturnSuccess) {
		IOLog("lxkit: lxkit_glue_create failed: 0x%x\n", rc);
		return false;
	}

	/* (5) the ADT walk.  If nothing exists yet (boot bring-up order),
	 * that is not fatal; the soft-irq fallback keeps the API alive. */
	if (enumerate_arm_io(provider) == true) {
		fDevicesLive = true;
		lx_dev_dump(NULL);      /* logs only live devices (NULL-guarded) */
	} else {
		IOLog("lxkit: no /arm-io children matched; "
		    "lx layer will idle until ADT nodes exist\n");
	}

	/* (6) register the storage drivers compiled into THIS image;
	 *      the drivers are mtk_msdc_msdc1.c and mtk_ufshcd.c. */
	rc = (IOReturn)lxkit_register_storage_drivers(&fLXC);
	if (rc != LX_OK) {
		IOLog("lxkit: storage driver registration failed: %d\n", rc);
		/* not fatal for bring-up: device may not exist */
	}

	/* (7) publish */
	registerService();
	g_lxkit_service = this;

	IOLog("lxkit: LxKitService started; devices=%d\n",
	    (int)fDevicesLive);
	return true;
}

void
LxKitService::stop(IOService *provider)
{
	/* drain drivers first (lxkit_drivers_glue.c implements a fast path
	 * under this symbol) */
	if (fLXCInited == true)
		lxkit_unregister_storage_drivers(&fLXC);

	if (g_lxkit_service == this)
		g_lxkit_service = NULL;

	super::stop(provider);
}

LxKitService *
LxKitService::current_service(void)
{
	return g_lxkit_service;
}

/* ==========================================================================
 * ADT walker: /arm-io children → struct lx_device[].
 *
 * The Apple DeviceTree is exposed in the IODT plane by the in-tree IOKit
 * machinery (IODeviceTreeSupport.h).  The contract in
 * docs/hardware_map_mt6785.md §3 is that `xnuloader/fdt2adt` synthesizes:
 *
 *   /arm-io/msdc1 : reg=0x11240000 0x10000  interrupt=MSDC1_SPI bus-width=<8>
 *   /arm-io/ufs0  : reg=0x11270000 0x100000 interrupt=UFS_SPI
 *   /arm-io/i2c0  : reg=0x11007000 0x1000
 *   /arm-io/touchscreen: i2c-address=<0x38interrupt=TS_PI
 *                                     compatible="focaltech,fts"
 *
 * Any child missing (e.g., display not included in the list) is skipped.
 * The interrupt property is read FIRST from the "interrupt" scalar (per the
 * hardware_map §3) and it falls back to FDT-style "interrupts" arrays.
 * ======================================================================== */
bool
LxKitService::enumerate_arm_io(IORegistryEntry *platform)
{
	IORegistryIterator   *it = NULL;
	IORegistryEntry     *child = NULL;
	IORegistryEntry     *armio = NULL;
	bool            found_any = false;
	bool                  res;

	if (platform == NULL)
		return false;

	/* Level 1: locate the "arm-io" DT node under the platform node. */
	it = IORegistryIterator::iterateOver(platform, gIODTPlane);
	if (it == NULL)
		return false;
	while ((child = it->getNextObject()) != NULL) {
		const char *name = child->getName(gIODTPlane);
		if (name == NULL)
			continue;
		if (strcmp(name, "arm-io") == 0) {
			armio = child;   /* retained by the iterator */
			break;
		}
	}
	it->release();
	if (armio == NULL) {
		IOLog("lxkit: no 'arm-io' node under platform %s\n",
		    platform->getName());
		return false;
	}

	/* Level 2: enumerate the IP children we host. */
	it = IORegistryIterator::iterateOver(armio, gIODTPlane);
	if (it == NULL)
		return false;
	while ((child = it->getNextObject()) != NULL) {
		const char *name = child->getName(gIODTPlane);
		if (name == NULL)
			continue;

		if (strcmp(name, "msdc1") != 0 && strcmp(name, "ufs0") != 0 &&
		    strcmp(name, "i2c0") != 0 && strcmp(name, "touchscreen") != 0)
			continue;      /* not yours (framebuffer is its own kext) */

		res = lxkit_synthesize_device(&fLXC, this, child);
		if (res == true) {
			found_any = true;
			IOLog("lxkit: synthesized device '%s'\n", name);
		} else {
			IOLog("lxkit: FAILED to synthesize '%s'\n", name);
		}
	}
	it->release();

	/* Publish the "gic-entity-name" registry prop (see the contract). */
	if (found_any == true) {
		const char *gicName = LXKIT_GIC_CTRL_NAME_DEFAULT;
		setProperty("lx-gic-name",
		    OSString::withCString(gicName));
	}
	return found_any;
}

void
LxKitService::teardown_devices(void)
{
	/* Reversing the nub attach (children) is done via IOService::stop()
	 * by nub::stop; this only cleans the lx-side device structures.
	 * See lxkit_armio.cpp for the depletion helper used at stop(). */
	lxkit_deactivate_devices(&fLXC);
	fDevicesLive = false;
}

/* ==========================================================================
 * LxKitDeviceNub
 * ========================================================================== */
OSDefineMetaClassAndStructors(LxKitDeviceNub, IOService);

bool
LxKitDeviceNub::init(struct lx_device *dev, const char *kind,
    const char *name, OSDictionary *dictionary)
{
	if (super::init(dictionary) != true)
		return false;
	fDev = dev;

	/* The nub must be addressable by class.  setName is the registry
	 * plane name ("msdc1"/..); the IOService class stays LxKitDeviceNub
	 * so cross-kext match stays stable. */
	if (name != NULL)
		setName(name);
	if (kind != NULL)
		setProperty("lx-device-kind", OSString::withCString(kind));
	if (name != NULL)
		setProperty("lx-device-name", OSString::withCString(name));
	return true;
}

void
LxKitDeviceNub::free()
{
	if (fDev != NULL) {
		lxkit_iomap_release_for_dev(fDev);
		fDev = NULL;
	}
	super::free();
}

static bool
lxkit_nub_set_interrupt_props(LxKitDeviceNub *self, u32 spi)
{
	/*
	 * IOService::resolveInterrupt(IOService.cpp:8730) contract: an OSArray
	 * named "IOInterruptControllers" holds *OSSymbol* entries (IC name),
	 * while "IOInterruptSpecifiers" holds OSData row entries (vectors).
	 * We emit exactly one entry each (one interrupt per device).
	 *
	 * Cell-generation: the ADT is big-endian in cells — the SPI is encoded
	 * as <SPI> big-endian 4 bytes (see docs §3 'interrupt=<UART0_SPI>').
	 */
	OSArray  *controllers = OSArray::withCapacity(1);
	OSArray  *specifiers  = OSArray::withCapacity(1);
	const OSSymbol *icName = OSSymbol::withCString(
	    LXKIT_GIC_CTRL_NAME_DEFAULT);
	u32 be_spi = OSSwapHostToBigInt32(spi);
	OSData  *spec = OSData::withBytes(&be_spi, sizeof(be_spi));
	bool        ok = false;

	if (controllers == NULL || specifiers == NULL || icName == NULL ||
	    spec == NULL)
		goto out;

	controllers->setObject((OSObject *)icName);
	specifiers->setObject((OSObject *)spec);

	self->setProperty(gIOInterruptControllersKey,
	    (OSObject *)controllers);
	self->setProperty(gIOInterruptSpecifiersKey,
	    (OSObject *)specifiers);
	ok = true;

out:
	if (controllers != NULL)
		controllers->release();
	if (specifiers != NULL)
		specifiers->release();
	if (icName != NULL)
		icName->release();
	if (spec != NULL)
		spec->release();
	return ok;
}

bool
LxKitDeviceNub::SetInterruptProps(u32 spi)
{
	return lxkit_nub_set_interrupt_props(this, spi);
}

bool
LxKitDeviceNub::start(IOService *provider)
{
	if (super::start(provider) != true)
		return false;

	/* Install DeviceMemory for the first MMIO resource (the drivers use
	 * only one MMIO region per device on MT6785). */
	if (fDev != NULL && fDev->niomaps > 0) {
		OSArray *dmArr = OSArray::withCapacity(fDev->niomaps);
		unsigned i;
		if (dmArr != NULL) {
			for (i = 0; i < fDev->niomaps; i++) {
				struct lx_iomap *iom = fDev->iomaps[i];
				IODeviceMemory *dm = NULL;
				if (iom == NULL)
					continue;
				dm = IODeviceMemory::withRange(
				    (IOPhysicalAddress)iom->phys,
				    (IOPhysicalLength)iom->len);
				if (dm != NULL) {
					dmArr->setObject((OSObject *)dm);
					dm->release();  /* owned by array now */
				}
			}
			setDeviceMemory(dmArr);
			dmArr->release();
		}
	}

	/* Interrupt props only when the ADT provided at least one SPI. */
	if (fDev != NULL && lx_dev_res_irq(fDev, 0) != 0) {
		if (lxkit_nub_set_interrupt_props(this,
		    lx_dev_res_irq(fDev, 0)) == false) {
			IOLog("lxkit-nub(%s): could not set interrupt props"
			    " (soft IRQ path will be used)\n",
			    fDev->name);
		}
	}
	return true;
}

void
LxKitDeviceNub::stop(IOService *provider)
{
	super::stop(provider);
}

IOWorkLoop *
LxKitDeviceNub::getWorkLoop() const
{
	/* the nub shares the LxKitService workloop */
	LxKitService *svc = LxKitService::current_service();
	if (svc != NULL)
		return svc->getWorkLoop();
	return NULL;
}

/* ==========================================================================
 * LxKitStorageNub
 * ========================================================================== */
OSDefineMetaClassAndStructors(LxKitStorageNub, LxKitDeviceNub);

/* ==========================================================================
 * LxKitTouchNub
 * ========================================================================== */
OSDefineMetaClassAndStructors(LxKitTouchNub, LxKitDeviceNub);

bool
LxKitTouchNub::init(struct lx_device *dev, const char *kind,
    const char *name, OSDictionary *dictionary)
{
	return super::init(dev, kind, name, dictionary);
}
