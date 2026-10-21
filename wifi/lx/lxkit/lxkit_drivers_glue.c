/*
 * lxkit_drivers_glue.c — the bridge between LxKitService (IOService) and the
 * drivers compiled into the host kext image (see lxkit_service.h (c)).
 *
 *   lxkit_register_storage_drivers(ctx)      -- called from LxKitService::start()
 *   lxkit_unregister_storage_drivers(ctx)    -- called from LxKitService::stop()
 *
 * The drivers themselves live in wifi/lx/drivers/mt6785/ and register as
 * plain struct lx_driver objects (lx_dev.h).  Keeping the list here lets the
 * symlink-free per-kext build (kexts/build.mk) pick the exact driver set.
 */

#include "lx_types.h"
#include "lx_dev.h"
#include "lx_errno.h"
#include "lx_printk.h"

/* Implemented in drivers/mt6785/*.c (exported for this glue). */
extern struct lx_driver mtk_msdc_msdc1_driver;   /* mtk_msdc_msdc1.c */
extern struct lx_driver mtk_ufshcd_driver;       /* mtk_ufshcd.c     */

int
lxkit_register_storage_drivers(struct lx_context *ctx)
{
	int rc;

	rc = lx_driver_register(ctx, &mtk_msdc_msdc1_driver);
	if (rc != 0) {
		pr_err("lxkit: msdc1 driver registration failed: %d\n", rc);
		return rc;
	}

	rc = lx_driver_register(ctx, &mtk_ufshcd_driver);
	if (rc != 0) {
		pr_err("lxkit: ufshcd driver registration failed: %d\n", rc);
		/* roll msdc1 back out so stop()/start() stays symmetric */
		lx_driver_unregister(&mtk_msdc_msdc1_driver);
		return rc;
	}

	pr_info("lxkit: storage drivers registered (msdc1, ufshcd)\n");
	return 0;
}

void
lxkit_unregister_storage_drivers(struct lx_context *ctx)
{
	lx_driver_unregister(&mtk_ufshcd_driver);
	lx_driver_unregister(&mtk_msdc_msdc1_driver);
	pr_info("lxkit: storage drivers unregistered\n");
}
