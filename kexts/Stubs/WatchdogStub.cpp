/*
 * WatchdogStub.cpp — the "no hardware watchdog trips during bring-up"
 * stub (plan §5.3, Watchdog bullet).
 *
 * `watchdogd` in iOS 16.5 requires a functioning hardware watchdog
 * provider and PANICS the box if an essential daemon fails to check in
 * after ~60 s.  During a bring-up boot half of userland is stubbed, so
 * the "legacy schedule" always trips.  This kext satisfies the
 * AppleARMWatchdogTimer personality with a NO-OP provider.
 */

#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPMpowerSource.h>
#include <libkern/c++/OSObject.h>

class WatchdogStubService : public IOService {
	OSDeclareDefaultStructors(WatchdogStubService);

public:
	virtual bool start(IOService *provider) override;
virtual IOReturn setWatchdogTimeout(u32 timeout_in_seconds)  override;
};

#define super IOService
OSDefineMetaClassAndStructors(WatchdogStubService, IOService)

bool WatchdogStubService::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	IOLog("WatchdogStub: AppleARMWatchdogTimer replaced by no-op "
	      "(bring-up mode; hardware reset disabled)\n");

	registerService();
	return true;
}

IOReturn WatchdogStubService::setWatchdogTimeout(u32 seconds)
{
	(void)seconds;
	IOLog("WatchdogStub: setWatchdogTimeout(%u) swallowed\n", seconds);
	return kIOReturnSuccess;
}
