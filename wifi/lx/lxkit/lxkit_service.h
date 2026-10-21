/*
 * lxkit_service.h — the LxKitService IOService bridge (lxkit ↔ IOKit).
 *
 * ---------------------------------------------------------------------------
 * ROLE (see wifi/lx/README.md §ARCHITECTURE)
 * ---------------------------------------------------------------------------
 * LxKitService is the ONLY IOService that lxkit instantiates.  It lives in
 * the host kext image (kexts/LxStorage) and:
 *
 *   (a) walks the Apple DeviceTree (ADT) /arm-io children published in the
 *       IODT plane ("msdc1", "ufs0", "i2c0", "touchscreen") — see
 *       docs/hardware_map_mt6785.md §3 — and synthesizes a `struct
 *       lx_device` per child with resources and platform properties
 *       (lxkit_armio.cpp, lxkit_synthesize_device());
 *
 *   (b) runs the lx core: workloop + workqueue (lxkit_work.cpp), locks
 *       (lxkit_locks.cpp), DMA/kmem (lxkit_mem.cpp), MMIO
 *       (lxkit_iospace.cpp — mapDeviceMemoryWithIndex on the synthesized
 *       nub) and the console sink (lxkit_console.cpp);
 *
 *   (c) executes the storage drivers compiled into the host kext image
 *       (drivers/mt6785/mtk_msdc_msdc1.c and mtk_ufshcd.c — registration
 *       glue in lxkit_drivers_glue.c).  The touch driver
 *       (drivers/mt6785/focaltech_fts.c) is executed by
 *       kexts/FocalTouchIOHID inside that kext's own image against the
 *       LxKitTouchNub exported here;
 *
 *   (d) publishes, in the IOService plane, the child nubs:
 *          - LxKitDeviceNub(kind = "msdc1" | "ufs0")   — storage nubs
 *          - LxKitTouchNub(i2c0 MMIO + touchscreen SPI) — match target for
 *            FocalTouchIOHID (IOProviderClass = "LxKitTouchNub");
 *
 *   (e) exposes the debug path used by kexts/LxStorage's GetBootMediumInfo
 *       selector (lxkit_api.h §MISC, lxkit_bootime.cpp glue).
 *
 * ---------------------------------------------------------------------------
 * IOService CONTRACTS OBSERVED  (all signatures verified in-tree)
 * ---------------------------------------------------------------------------
 *  - IOService::start(provider) is called once by the IOCatalogue
 *    matching flow; registerService() is called at the end of start().
 *  - We override IOService::getWorkLoop() so interrupt event sources and
 *    command gates attached to our nubs find the correct loop
 *    (iface at xnu/iokit/IOKit/IOWorkLoop.h: addEventSource()).
 *  - Interrupts flow through the standard IOService::resolveInterrupt
 *    property path ("IOInterruptControllers"+"IOInterruptSpecifiers",
 *    see xnu/iokit/Kernel/IOService.cpp:8730).  The GIC kext is
 *    kexts/MT6785GICv3 (kernel-port team) — an IOInterruptController
 *    subclass registered with the platform by its own code; the nub
 *    property "IOInterruptSpecifiers" carries the SPI number the GIC
 *    decodes (see README "GIC contract note").
 *  - BSD publishing (/dev/diskNs...) happens through IOKitBSDInit only
 *    for IOMedia-class nubs; our LxMedia (kexts/LxStorage) documents the
 *    expectation there.
 */

#ifndef __LXKIT_SERVICE_H
#define __LXKIT_SERVICE_H

#include "IOKit/IOService.h"

extern "C" {
#include "wifi/lx/include/lx/lxkit_api.h"
#include "wifi/lx/include/lx/lx_dev.h"
}

/* ==========================================================================
 * LxKitService
 * ========================================================================== */
class LxKitService : public IOService {
	OSDeclareDefaultStructors(LxKitService);

public:
	/* ---- IOService lifecycle --------------------------------------- */
	virtual bool init(OSDictionary *dictionary = NULL) override;
	virtual void free() override;
	virtual bool start(IOService *provider) override;
	virtual void stop(IOService *provider) override;
	virtual IOWorkLoop *getWorkLoop() const override;

	/* ---- lx context accessors --------------------------------------- */
	static LxKitService *current_service(void);
	struct lx_context * service_lx_context(void)  { return &fLXC; }

protected:
	/* ADT walker + nub construction (lxkit_service.cpp). */
	bool           enumerate_arm_io(IORegistryEntry *adtRoot);
	void           teardown_devices(void);

	IOService      *fPlatform;      /* the IOPlatformExpertDevice provider    */
	IOWorkLoop     *fWorkLoop;      /* owned workloop (IOWorkLoop::workLoop)  */
	bool           fDevicesLive;    /* enumerate_arm_io() succeeded           */

	struct lx_context  fLXC;        /* lxkit context (lxkit_api.h)            */
	bool               fLXCInited;
	int                fStartCount; /* start() re-entry guard                 */
};

/* ==========================================================================
 * LxKitDeviceNub — the generic "lx device in the IOKit plane" nub.
 *
 * Published registry properties:
 *   "lx-device-kind"         OSString : "msdc1"/"ufs0"/"i2c0"/"touchscreen"/...
 *   "lx-device-name"         OSString : mirror
 *   "IOInterruptControllers" OSArray of OSSymbol     (see resolveInterrupt)
 *   "IOInterruptSpecifiers"  OSArray of OSData(4) — the GIC SPI (BE cells)
 * Device memory            : setDeviceMemory(IOArray of IODeviceMemory*) so
 *                             consumers can mapDeviceMemoryWithIndex(0) it.
 * ========================================================================== */
class LxKitDeviceNub : public IOService {
	OSDeclareDefaultStructors(LxKitDeviceNub);

public:
	virtual bool init(struct lx_device *dev, const char *kind,
    const char *name, OSDictionary *dictionary = NULL) override;
	virtual void free() override;
	virtual bool start(IOService *provider) override;
	virtual void stop(IOService *provider) override;

	virtual IOWorkLoop *getWorkLoop() const override;

	/* The nub carries no interrupt overrides — the BASE IOService path +
	 * the two array properties installed in start() do it all (standard
	 * dispatch through IOService::lookupInterrupt → controller). */

	/* same-image consumers go through GetLxDevice(); cross-kext consumers
	 * via IOKit properties only */
	struct lx_device * GetLxDevice(void)  { return fDev; }
	bool               SetInterruptProps(u32 spi);  /* lxkit_service.cpp  */

protected:
	struct lx_device * fDev;
};

/* ==========================================================================
 * LxKitStorageNub — the concrete storage child nub (kinds: msdc1, ufs0).
 *
 * This is the class LxStorage's own LxBlockService personality matches:
 *      IOProviderClass = "LxKitStorageNub"
 * (kept as its own class, not the generic nub, so a future touch-storage
 * confusion is impossible at the matching layer; the generic
 * LxKitDeviceNub stays as the base class for anything else the walker
 * may produce later, e.g. a debug device.)
 * ========================================================================== */
class LxKitStorageNub : public LxKitDeviceNub {
	OSDeclareDefaultStructors(LxKitStorageNub);

public:
	/* No behavioural overrides — the base class does everything
	 * (DeviceMemory + IOInterrupt* properties from the lx_device).     */
};

/* ==========================================================================
 * LxKitTouchNub — dedicated provider nub published for FocalTouchIOHID
 * (class name is the match key: "IOProviderClass" = "LxKitTouchNub").
 * The nub carries:
 *   - the i2c0 DeviceMemory (i2c0 @ 0x11007000, size 0x1000)
 *   - one level-triggered interrupt (the touchscreen SPI)
 *   - matching registry props for the touchscreen style
 * Consumer contract constants (also duplicated in
 * kexts/FocalTouchIOHID/FocalTouchService.cpp)
 * ========================================================================== */
class LxKitTouchNub : public LxKitDeviceNub {
	OSDeclareDefaultStructors(LxKitTouchNub);

public:
	virtual bool init(struct lx_device *dev, const char *kind,
    const char *name, OSDictionary *dictionary = NULL) override;

	/* Verify-on-device: these are DEFAULTS; the ADT is authoritative. */
	static const u32 kTouchSpiDefault = 0x78;      /* provisional   */
	static const u32 kTouchI2cAddress = 0x38;      /* focaltech,fts */
};

#endif /* __LXKIT_SERVICE_H */
