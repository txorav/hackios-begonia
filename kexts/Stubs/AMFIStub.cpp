/*
 * AMFIStub.cpp — minimal stand-in for AppleMobileFileIntegrity (plan §5.2).
 *
 * iOS 16.5 userland (`launchd`, `backboardd`, all SpringBoard family)
 * only runs when the kernel code-signing gate says "yes".  On iPhones
 * that's AMFI + Apple SEP; on begonia we REPLACE the whole stack:
 *
 *   - AMFI never runs; the kernel's cs_okchecking flags come from this
 *     stub's behavior instead:
 *       · treat every loaded Mach-O as `CS_PLATFORM_BINARY`
 *       · check cs_debug=1, cs_enforcement_disable=1 (set in xnu's
 *         bsd/kern/ubc_subr.c / vm_map.c — see userland/README.md)
 *   - entitlements: parse CoreEntitlements in the shell format we
 *     ship (userland/IDEAS_OR_TODO.md), but never verify a root CA.
 *
 * What this class actually exports: a shell IOService so the IOKit
 * matching plane is satisfied (iOS's `securityd` checks for the AMFI
 * personality), plus a UserClient for launchd's amfi calls — returning
 * kIOReturnSuccess for every non-mandatory operation.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSObject.h>

class AMFIUserClient;

class AMFIStubService : public IOService {
	OSDeclareDefaultStructors(AMFIStubService);

public:
	virtual bool start(IOService *provider) override;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, IOUserClient **handler) override;
};

class AMFIUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AMFIUserClient);

public:
	virtual bool initWithFrame(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties);
	virtual bool start(IOService *provider) override;
	virtual IOReturn clientMemoryForType(UInt32 type,
	    IOMemoryDescriptor **memory) override;
};

#define super IOService
OSDefineMetaClassAndStructors(AMFIStubService, IOService)

bool AMFIStubService::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	IOLog("AMFIStub: code-signing enforcement DISABLED (plan §5.2; "
	      "all binary reads pass as CS_PLATFORM_BINARY)\n");

	registerService();
	return true;
}

IOReturn AMFIStubService::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, IOUserClient **handler)
{
	AMFIUserClient *uc;

	uc = new AMFIUserClient;
	if (uc == NULL)
		return kIOReturnNoMemory;

	if (!uc->initWithTask(owningTask, securityID, type)) {
		uc->release();
		return kIOReturnBadArgument;
	}
	if (!uc->attach(this)) {
		uc->release();
		return kIOReturnError;
	}

	*handler = uc;
	return kIOReturnSuccess;
}

#undef super
#define super IOUserClient
OSDefineMetaClassAndStructors(AMFIUserClient, IOUserClient)

bool AMFIUserClient::initWithTask(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties)
{
	(void)properties;
	return super::initWithTask(owningTask, securityID, type);
}

bool AMFIUserClient::start(IOService *provider)
{
	return super::start(provider);
}

IOReturn AMFIUserClient::clientMemoryForType(UInt32 type,
    IOMemoryDescriptor **memory)
{
	(void)type;
	(void)memory;
	return kIOReturnUnsupported;
}
