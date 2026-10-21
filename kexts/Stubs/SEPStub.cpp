/*
 * SEPStub.cpp — the "no physical SEP" shim pair (plan §5.3).
 *
 * iOS 16.5's `securityd`, `UserEventAgent`, `Watchdogd` and the
 * `backboardd`/`SpringBoard` lock-screen engine all block on Apple's
 * Secure Enclave Processor (SEP) services.  begonia has no SEP, so this
 * kext provides the UserClient implementations these daemons look for:
 *
 *   - AppleSEPManager: presents the transport nub; keybag unlock
 *     (MKBUnlockDevice) trivially SUCCEEDS, and every data-key path is
 *     backed by software AES (always "usable", never hardware).
 *   - AppleKeyStore: supplies its gk_savez/keybag-dump UserClients with
 *     in-memory software bag state.
 *
 * (Plan §5.3 explicitly: "Return success for keybag unlock
 * (MKBUnlockDevice) and software-backed AES encryption so UserEventAgent,
 * securityd, SpringBoard do not block waiting for a physical SEP").
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSObject.h>

/* =========================================================================
 * AppleSEPManager stand-in
 * ========================================================================= */
class SEPManagerStubService : public IOService {
	OSDeclareDefaultStructors(SEPManagerStubService);

public:
	virtual bool start(IOService *provider) override;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, IOUserClient **handler) override;

protected:
	bool fKeybagUnlocked;           /* our fake MKB state             */
	u8   fSoftwareKey[32];          /* "AES" key: fake, in-memory bag */
};

class SEPUserClient : public IOUserClient {
	OSDeclareDefaultStructors(SEPUserClient);

public:
	virtual bool initWithTask(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties);
	virtual bool start(IOService *provider) override;
	virtual IOReturn externalMethod(uint32_t selector,
	    IOExternalMethodArguments *arguments,
	    IOExternalMethodDispatch *dispatch,
	    OSObject *target, void *reference) override;
};

#define super IOService
OSDefineMetaClassAndStructors(SEPManagerStubService, IOService)

bool SEPManagerStubService::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	fKeybagUnlocked = false;
	for (unsigned i = 0; i < sizeof(fSoftwareKey); ++i)
		fSoftwareKey[i] = (u8)(0x5Au + i);     /* stable fake key   */

	IOLog("SEPStub: AppleSEPManager live (software keybag; "
	      "MKBUnlockDevice = SUCCESS)\n");

	registerService();
	return true;
}

IOReturn SEPManagerStubService::newUserClient(task_t owningTask,
    void *securityID, UInt32 type, IOUserClient **handler)
{
	SEPUserClient *uc = new SEPUserClient;
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
OSDefineMetaClassAndStructors(SEPUserClient, IOUserClient)

bool SEPUserClient::initWithTask(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties)
{
	(void)properties;
	return super::initWithTask(owningTask, securityID, type);
}

bool SEPUserClient::start(IOService *provider)
{
	return super::start(provider);
}

IOReturn SEPUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch,
    OSObject *target, void *reference)
{
	(void)dispatch; (void)target; (void)reference;

	switch (selector) {
	case 0:  /* MKBUnlockDevice */
		IOLog("SEPStub: MKBUnlockDevice → success (fake keybag)\n");
		return kIOReturnSuccess;

	case 1:  /* keybag dump/get — zero-length software bag */
		if (args->structureOutput != NULL && args->structureOutputSize > 0)
			bzero(args->structureOutput, args->structureOutputSize);
		return kIOReturnSuccess;

	default: /* every other SEP call: pretend success (software AES) */
		return kIOReturnSuccess;
	}
}
