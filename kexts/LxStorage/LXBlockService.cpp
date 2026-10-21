/*
 * LXBlockService.cpp — the storage host kext (plan §4.3 Stage B).
 *
 * This kext image contains the whole lxkit runtime (wifi/lx/lxkit/*)
 * plus the ported storage drivers (wifi/lx/drivers/mt6785/).  Its job:
 *
 *   1. instantiate LxKitService (IOService) which walks the ADT and runs
 *      the lx core + drivers → storage devices get published into lxkit
 *      (see wifi/lx/lxkit/lxkit_api.cpp: lxkit_storage_publish);
 *   2. register an lxkit_storage_client here; on each
 *      on_storage_attached() construct the IOKit shadow stack:
 *          IOMedia (whole disk + GPT children) → IOBlockStorageDevice-
 *          equivalent nubs that AppleAPFSContainer can attach to
 *          (plan §4.3 "publish an IOMedia nub with the GPT partition
 *          scheme so AppleAPFSContainer attaches");
 *   3. relay block I/O submissions into lxkit_storage_submit().
 *
 * GPT parsing is lernt from the lxkit/lxgpt on-disk layout (see
 * wifi/lx/include/lx/lx_gpt.h) so only the IOKit plumbing lives here.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOMedia.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>

#include "lxkit_service.h"      /* LxKitService, LxKitDeviceNub… */
#include "lx/lxkit_api.h"

/* How many partitions a single published device may spawn as IOMedia
 * nubs (GPT children); see LXKIT_MAX_PARTITIONS in lxkit_api.h. */
extern const unsigned kLXBSPartitionCap = LXKIT_MAX_PARTITIONS;

/* =========================================================================
 * LXMedia — the IOMedia nub the port publishes over one GPT child (or
 * the whole disk when root_lba == 0).  Real IOKitBSDInit visibility
 * (___apfs container attach) expects:
 *     class IOMedia, "Content" OSString, block-based,
 *     readable, writeable, bootable-per-match-filters.
 * ========================================================================= */
class LXMedia : public IOMedia {
	OSDeclareDefaultStructors(LXMedia);

public:
	virtual bool init(u64 base_byte, u64 size_bytes,
	    u64 block_size, const char *content,
	    LXBlockService *provider);

protected:
	LXBlockService *fService;
};

#undef super
#define super IOMedia
OSDefineMetaClassAndStructors(LXMedia, IOMedia)

bool LXMedia::init(u64 base_byte, u64 size_bytes,
    u64 block_size, const char *content, LXBlockService *provider)
{
	bool ok = super::init();

	if (!ok || provider == NULL)
		return false;

	fService = provider;

	/* IOMedia contract (xnu/iokit/IOKit/IOMedia.h):
	 *    init(base, size, block_size, characteristics, isWhole,
	 *         isReady, name, kind)
	 */
	OSString *contentStr = OSString::withCString(content ? content : "Unknown");
	setProperty("Content", contentStr);
	contentStr->release();

	return true;
}

#undef super
#define super IOService

/* =========================================================================
 * LXBlockService — the storage-shadow part of the host kext image.
 * ========================================================================= */
OSDefineMetaClassAndStructors(LXBlockService, IOService)

/* the lxkit storage client we hand to lxkit_storage_subscribe(); the
 * consumer-side struct is static so lifetime is trivial.        */
static struct lxkit_storage_client g_client;

static bool
publish_disk(IOMedia **root_media_out,
    const struct lxkit_storage_desc *desc, void *lx_dev)
{
	IOMedia *media;
	u64 block_size = desc->block_size;
	u64 total = desc->num_blocks;
	u64 bytes = block_size * total;

	media = new LXMedia;
	if (media == NULL) {
		IOLog("LXBlockService: alloc failed for %s\n", desc->name);
		return false;
	}
	if (!media->init(0, bytes, block_size, "CD partition scheme",
	    /*whole=*/true, /*bootable=*/false, desc->name, "Disk")) {
		media->release();
		return false;
	}

	/* Read blocks for write-protected media (µSD RO switch etc.):
	 * see desc->rw_posdd in lxkit_api.h. */
	media->setWriteProtected(desc->rw_posdd != 0);
	media->setProperty("lx-medium", OSString::withCString(desc->name));

	IOLog("LXBlockService: %s %llu blocks @%llu B\n",
	    desc->name, (unsigned long long)total,
	    (unsigned long long)block_size);

	*root_media_out = media;
	return true;
}

/* =========================================================================
 * lxkit client callbacks — invoked on the lxkit workloop (see
 * lxkit_api.h §Client registration).  Non-blocking only!
 * ========================================================================= */
static void
on_storage_attached(struct lxkit_storage_client *cl,
    const struct lxkit_storage_desc *desc, void *lx_dev)
{
	LXBlockService *svc = (LXBlockService *)cl->consumer_ctx;
	IOMedia *root = NULL;

	if (!publish_disk(&root, desc, lx_dev)) {
		IOLog("LXBlockService: attach of '%s' dropped\n", desc->name);
		return;
	}

	svc->attachToParent(root, gIOServicePlane);
	svc->registerService();
	/* TODO(M5): GPT child walker (LXGPT) publishes child IOMedia
	 * entries per partition record here; bring-up ships whole-disk. */
}

static void
on_request_done(struct lxkit_storage_client *cl, void *cookie, int lx_err)
{
	LXBlockService *svc = (LXBlockService *)cl->consumer_ctx;
	IOLog("LXBlockService[%p]: cookie %p done, err=%d\n", svc, cookie,
	    lx_err);
	/* The real path: drive the pending IOStorageCommand completion —
	 * IOReturn code = -EIO mapping (lx_errno.h). */
}

static void
on_storage_detached(struct lxkit_storage_client *cl, void *lx_dev)
{
	(void)cl; (void)lx_dev;
	IOLog("LXBlockService: device detached\n");
}

static void
on_geometry_ready(struct lxkit_storage_client *cl, void *lx_dev)
{
	(void)cl; (void)lx_dev;
	IOLog("LXBlockService: geometry ready (GPT capacity known)\n");
}

/* =========================================================================
 * service lifecycle
 * ========================================================================= */
bool LXBlockService::init(OSDictionary *props)
{
	return super::init(props);
}

bool LXBlockService::start(IOService *provider)
{
	if (!super::start(provider))
		return false;

	/* Wire the storage-client seen by lxkit_api.h to point back
	 * at us.  (LxKitService — same image — drives the drivers.) */
	g_client.who          = "LXBlockService";
	g_client.consumer_ctx = this;
	g_client.ops.on_storage_attached = on_storage_attached;
	g_client.ops.on_request_done     = on_request_done;
	g_client.ops.on_storage_detached = on_storage_detached;
	g_client.ops.on_geometry_ready   = on_geometry_ready;

	int rc = lxkit_storage_subscribe(&g_client,
	    ((LxKitService *)provider)->service_lx_context());
	if (rc != 0) {
		IOLog("LXBlockService: lxkit subscribe failed: %d\n", rc);
		return false;
	}

	registerService();
	IOLog("LXBlockService: subscribed to lxkit\n");
	return true;
}
