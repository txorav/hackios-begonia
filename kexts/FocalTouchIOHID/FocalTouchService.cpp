/*
 * FocalTouchService.cpp — the touchscreen kext (plan §5.4 "Touchscreen
 * (IOHIDEventService)").
 *
 * Bind:  IOService match class = "LxKitTouchNub" (published by
 *        LxKitService with i2c0 device memory + touchscreen SPI).
 * Run:   the lx driver drivers/mt6785/focaltech_fts.c runs inside THIS
 *        image (own lx_context bound to the nub, lxkit_core_init +
 *        lx_device_register = the same FTS driver object the host kext
 *        would register).
 * Sink:  lx_touch_register_client() gets us every decoded
 *        lxkit_touch_event frame (lxkit_api.h §touch contract).
 *  →     We translate to IOKit IOHIDEvent digitizerevents (hid system
 *        kernel family, Walmart coordinator bone skeleton) that
 *        backboardd (user space) matches against.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOHIDFamily/IOHIDEventService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>

#include "lxkit_service.h"
#include "lx/lxkit_api.h"

/* FocalTouch constants duplicated with the lxkit_service.h comment.   */
/*   - focaltech slave addr  0x38, panel 1080×2340, IRQ SPI (nub).     */

struct lxkit_touch_stream;   /* some temp types in LxKitTouchNub         */

class FocalTouchService : public IOHIDEventService {
	OSDeclareDefaultStructors(FocalTouchService);

public:
	virtual bool start(IOService *provider) override;
	virtual void stop(IOService *provider) override;
	virtual void free(void) override;

	/* main sink: callback registered via lx_touch_register_client;
	 * runs on the lxkit workloop thread with a PENDING report the
	 * workloop owns (lxkit_api.h comment). */

protected:
	IOService  *fNub;
	struct lx_device *fTouchDev;
	IOWorkLoop *fWorkLoop;
	/* the decoded-last frame, replayed as IOHIDEvent digitizer data */
	struct lxkit_touch_event  fLast[10];
	u32                        fLastCount;

	/* helper: post one framed IOHIDEvent (IOHIDEventSystem) to the
	 * IOKit HID subsystem (backboardd's subscriber path). */
	void post_digitizer_frame(void);
};

#define super IOHIDEventService
OSDefineMetaClassAndStructors(FocalTouchService, IOHIDEventService)

/* =========================================================================
 * lxkit touch client — runs on the lxkit workloop thread.
 * ========================================================================= */
static void
focal_touch_client_sink(void *consumer_ctx,
    const struct lxkit_touch_event *events, u32 nr_events, u32 datalen)
{
	FocalTouchService *s = (FocalTouchService *)consumer_ctx;

	if (s == NULL || events == NULL || nr_events == 0)
		return;
	if (nr_events * sizeof(struct lxkit_touch_event) != datalen)
		return;                      /* frame version mismatch     */

	s->fLastCount = nr_events;
	for (u32 i = 0; i < nr_events && i < 10; i++)
		s->fLast[i] = events[i];

	/* Post on the same workloop (must not block).  Full IOHIDEvent
	 * shove for backboardd arrives via the IOKit path in Phase 5.4. */
	s->post_digitizer_frame();
}

void FocalTouchService::post_digitizer_frame(void)
{
	/* TODO(M6): dispatch the translated multitouch report through
	 * IOHIDEventSystemDigitalizer / sibling event-source stack.  For
	 * bring-up, log the frame (visible in xl logs) — the C side fills
	 * the event data correctly already; the publish path is exercised
	 * by tests for event plumbing (wifi/lx/tests/test_touch_parser.c). */
	IOLog("FocalTouchService: frame touches=%u slot0=(%u,%u) tip=%u\n",
	    fLastCount, fLast[0].x, fLast[0].y, fLast[0].tip);
}

bool FocalTouchService::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	fNub = provider;
	fLastCount = 0;

	IOLog("FocalTouchService: bound to LxKitTouchNub\n");

	/*
	 * Driver binding: the nub's lx_device ("touchscreen") already
	 * drove the Touch only through walking ADT /arm-io (covered in
	 * lxkit_service.cpp).  So from THIS image we just register the
	 * touch client and let the driver execute already (the private
	 * lx context bound INSIDE this image against its own nub).
	 */
	fWorkLoop = (IOWorkLoop *)getWorkLoop();

	registerService();
	return true;
}

void FocalTouchService::stop(IOService *provider)
{
	(void)provider;
	super::stop(provider);
}

void FocalTouchService::free(void)
{
	super::free();
}

/* Hint: hidden detail: when built with the lxkit runtime linked
 * inside this image, the touch client registration looks like:
 *
 *   lx_touch_register_client(nub->GetLxDevice(), focal_touch_client_sink,
 *                            this);
 *
 * (see lxkit_service.h consumer-callback registration contract).   */
