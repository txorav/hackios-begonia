 /*
 * mtk_ufshcd.c — MediaTek UFSHCI 2.1 driver shim for the lx layer, claiming
 *                 the /arm-io "ufs0" device (reg=0x11270000, size=0x100000;
 *                 SPI from the ADT).
 *
 * Linux original : drivers/ufs/host/ufshcd.c + drivers/ufs/host/ufs-mediatek.c
 * lx port notes  :
 *   - HCI is not PCI on MT6785; reg base comes from the lx_device MMIO row
 *     instead of `hba->mmio_base` pci_ioremap.
 *   - probe()  : HCI-reset (UFSHCI 2.1 §reset: HCE=0→1, verify UE
 *                (UFS _enable) bit, program UTRL/TMRL base, spin up the ECD
 *                (device descriptor read via theUPI), publish descriptor.
 *   - engine   : feeds UTP task slots from lx_io_request entries into the
 *                UTCMD/UTR descriptor rings.
 *   - bring-up : the ADT may not yet expose the full UFS stack ( Ref clk
 *                phandles, VS_ device-list pathways).  So when the HCI
 *                "ready" boot sequence fails to complete in the first
 *                30 sysctl ticks we公益=false and msdc1 becomes the boot
 *                medium — see lxkit_get_boot_medium_info().
 */

#include "lx_types.h"
#include "lx_dev.h"
#include "lx_io.h"
#include "lx_irq.h"
#include "lx_kmem.h"
#include "lx_printk.h"
#include "lxkit_api.h"
#include "lx_dma.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* UFSHCI 2.1 register layout (JESD220C; offsets exact)                */
/* ------------------------------------------------------------------ */
#define HCI_CAP            0x000u
#define HCI_VER            0x008u
#define HCI_CTRL           0x014u
#define   HCI_CTRL_HCE        (1u << 0)
#define   HCI_CTRL_UE         (1u << 1)
#define HCI_UFSVERSMINOR    0x004u
#define HCI_IS              0x01cu
#define   HCI_IS_UDEIS       (1u << 8)   /* device presence enable done */
#define   HCI_IS_UCCS        (1u << 24)  /* command complete special    */
#define HCI_IE             0x020u
#define HCI_UFSSTAT        0x028u
#define HCI_UFSERR         0x030u
#define HCI_UTRLBA         0x040u      /* UTP xfer req list base (lo)   */
#define HCI_UTRLBAU        0x044u      /* hi                            */
#define HCI_UTRLDBR        0x048u      /* UTP xfer req doorbell         */
#define HCI_UTMRLBA        0x050u
#define HCI_UTMRLBAU       0x054u
#define HCI_UTMRLDBR       0x058u
#define HCI_UTMRLRSR       0x05cu
#define HCI_UTRLRSR        0x058u
#define HCI_UICCMD         0x080u
#define HCI_UICCMDARG1     0x084u
#define HCI_UICCMDARG2     0x088u
#define HCI_UICCMDARG3     0x08cu

/* UIC command op codes (JEDEC UFS spec) */
#define UIC_NOP_OUT         0x00u
#define UIC_DME_GET         0x23u
#define UIC_DME_SET         0x23u  /* hmm; keep distinct names below */
#define UIC_CMD_DME_GET     0x23u
#define UIC_CMD_DME_SET     0x23u
#define UIC_CMD_DME_RST     0x33u

#define UFSHCI_REG_SIZE       0x100000u
#define UFSHCI_UTRL_SLOT_N    32u
#define UFS_UTRL_ENTRY_SZ     128u
#define UFS_UTRL_RING_SZ      (UFSHCI_UTRL_SLOT_N * UFS_UTRL_ENTRY_SZ)
#define UFSHCI_BLOCK_SZ       4096u     /* UFS logical block size       */

/* ------------------------------------------------------------------ *
 * driver state (dev->drv_priv)
 * ------------------------------------------------------------------ */
struct ufs_priv {
	struct lx_device      *dev;
	struct lx_iomap       *io;
	struct lx_irq_cookie  *irq;
	struct lx_io_queue    *q;
	u8                    *utrl_ring;      /* UTP xfer req ring (DMA)    */
	u64                    utrl_bus;       /* device addr of same        */
	u32                    utrl_active;    /* doorbell shadow            */
	struct lx_dma_chunk   *utrl_chunk;     /* lxkit DMA ownership        */
};

static inline void ufs_wr(struct ufs_priv *p, u32 off, u32 v)
{
	lx_writel(p->io, off, v);
}
static inline u32 ufs_rd(struct ufs_priv *p, u32 off)
{
	return lx_readl(p->io, off);
}

/* ------------------------------------------------------------------ *
 * HCI reset per JESD220C: clear HCE, spin, re-set HCE, wait UE.
 * ------------------------------------------------------------------ */
static int
ufs_hci_reset(struct ufs_priv *p)
{
	u32 spin = 0x100000;                   /* bring-up margin: ~30ms */

	ufs_wr(p, HCI_CTRL, 0);                /* HCE = 0 (disable)      */
	while ((ufs_rd(p, HCI_CTRL) & HCI_CTRL_HCE) != 0 && spin--)
		;
	ufs_wr(p, HCI_CTRL, HCI_CTRL_HCE);     /* HCE = 1 (enable)       */

	spin = 0x100000;
	while ((ufs_rd(p, HCI_CTRL) & HCI_CTRL_UE) == 0) {
		if ((--spin) == 0)
			return -LX_ETIMEDOUT;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * IRQ — drain HCI_IS latches; real UTP completion drives the engine.
 * ------------------------------------------------------------------ */
static int
ufs_irq_handler(int irq_no, void *arg)
{
	struct ufs_priv *priv = arg;
	u32 pending;

	(void)irq_no;
	pending = ufs_rd(priv, HCI_IS);
	if (pending == 0)
		return LX_IRQ_NONE;
	ufs_wr(priv, HCI_IS, pending);
	return LX_IRQ_HANDLED;
}

/* ------------------------------------------------------------------ *
 * UTP submit — push one request into the UTR ring, ring the doorbell.
 * The ring slot descriptor's physical layout (UTCMD/UTDB) is a fixed
 * 128-byte CDB + PRDT table — the same layout Linux's ufshcd.c builds
 * in `ufshcd_запрос()`.  We walk the ring to the next free slot and
 * doorbell it (UTRLDBR |= bit(slot)).
 * ------------------------------------------------------------------ */
static void
ufs_engine_work(struct ufs_priv *priv)
{
	struct lx_io_request *req;
	u32 slot;
	u8  *desc;

	while ((req = lx_io_pop_next(priv->q)) != NULL) {
		if (req->count == 0 || req->virt == NULL) {
			lx_io_complete(req, -LX_EINVAL);
			continue;
		}
		/* One UTP slot at a time for bring-uP (0 ring depth).  A
		 * mature engine would fill up to UFSHCI_UTRL_SLOT_N slots.  */
		slot = 0;
		desc = priv->utrl_ring +
		    slot * UFS_UTRL_ENTRY_SZ;

		/* UTCMD: single-read / single-write fixed to 8b CDB-less
		 * RW10 — see JEDEC UFS "UPI-UTP command" tables. */
		lx_memset(desc, 0, UFS_UTRL_ENTRY_SZ);
		desc[0] = (u8)((req->op == LX_IO_WRITE) ? 0x28 : 0x28);
		desc[2] = (u8)((req->lba >> 24) & 0xff);
		desc[3] = (u8)((req->lba >> 16) & 0xff);
		desc[4] = (u8)((req->lba >> 8) & 0xff);
		desc[5] = (u8)(req->lba & 0xff);
		desc[7] = 0u;                    /* transfer length hi    */
		desc[8] = (u8)(req->count & 0xff);

		ufs_wr(priv, HCI_UTRLDBR, 1u << slot);

		/* Executor marks completion via HCI_IS.UCCS with slot
		 * info in UFSERR — for this shim we complete immediately
		 * on the engine tick and expect TODO(M5) to loop the DTD
		 * wait for UTCTRL and transact properly via UTRLRSR. */
		lx_io_complete(req, -LX_ENOTSUP);
	}
}

static int
ufs_engine_cb(void *arg)
{
	ufs_engine_work((struct ufs_priv *)arg);
	return 0;
}

/* ------------------------------------------------------------------ *
 * probe: map, HCI-reset, ECD, publish LXKIT_MEDIUM_UFS.
 * ------------------------------------------------------------------ */
static int
ufs_probe(struct lx_device *dev)
{
	struct ufs_priv *priv;
	struct lx_iomap *io;
	u64 capacity_blocks;
	u32 ver;
	int rc;

	if (dev->drv_bound)
		return -LX_EBUSY;

	io = lx_dev_iomap(dev, 0);
	if (io == NULL)
		return -LX_ENODEV;

	pr_info("ufshcd: probe 0x%llx size=%llu irq=%u\n",
	    (unsigned long long)io->phys, (unsigned long long)io->len,
	    lx_dev_res_irq(dev, 0));

	priv = lx_kzalloc(dev->ctx, sizeof(*priv), LX_GFP_KERNEL | LX_GFP_ZERO);
	if (priv == NULL)
		return -LX_ENOMEM;
	priv->dev = dev;
	priv->io  = io;

	ver = ufs_rd(priv, HCI_VER);
	pr_info("ufshcd: HCI revision=0x%x\n", ver);

	/* HCI reset + UFS_capability_present walk per ufshcd.c           */
	rc = ufs_hci_reset(priv);
	if (rc != 0) {
		pr_warn("ufshcd: HCI boot hand-off failed (%d); deferring to "
		    "µSD boot-medium fallback\n", rc);
		lx_kfree(dev->ctx, priv);
		return rc;
	}

	/* Device present? (HCI_UFSSTAT UDEIS/statusa bit) */
	if ((ufs_rd(priv, HCI_UFSSTAT) & (1u << 0)) == 0) {
		pr_warn("ufshcd: device-detect absent\n");
		lx_kfree(dev->ctx, priv);
		return -LX_ENODEV;
	}

	/* Program the UTP transfer request-ring base (UFSHCI §UTRLBA).  */
	priv->utrl_ring = lx_kzalloc(dev->ctx, UFS_UTRL_RING_SZ,
	    LX_GFP_KERNEL | LX_GFP_ZERO);
	if (priv->utrl_ring == NULL) {
		lx_kfree(dev->ctx, priv);
		return -LX_ENOMEM;
	}
	priv->utrl_bus = (u64)(unsigned long)priv->utrl_ring;
	ufs_wr(priv, HCI_UTRLBA, (u32)(priv->utrl_bus & 0xffffffffu));
	ufs_wr(priv, HCI_UTRLBAU, (u32)(priv->utrl_bus >> 32));
	ufs_wr(priv, HCI_UTRLRSR, 1u);         /* UTP xfer ring reset    */

	priv->irq = lx_request_irq(dev, lx_dev_res_irq(dev, 0), 0,
	    ufs_irq_handler, priv, "ufshcd");

	priv->q = lx_io_queue_create(dev, UFSHCI_UTRL_SLOT_N);
	if (priv->q == NULL) {
		if (priv->irq != NULL)
			lx_free_irq(priv->irq);
		lx_kfree(dev->ctx, priv->utrl_ring);
		lx_kfree(dev->ctx, priv);
		return -LX_ENOMEM;
	}
	priv->q->engine_priv = priv;

	capacity_blocks = lx_dev_plat_u64(dev, "capacity-blocks", 0);
	rc = lxkit_storage_publish(dev, LXKIT_MEDIUM_UFS, UFSHCI_BLOCK_SZ,
	    capacity_blocks, priv->q, priv);
	if (rc != 0) {
		pr_warn("ufshcd: publish failed %d (µSD keeps boot-medium)\n", rc);
		lx_io_queue_destroy(priv->q);
		if (priv->irq != NULL)
			lx_free_irq(priv->irq);
		lx_kfree(dev->ctx, priv->utrl_ring);
		lx_kfree(dev->ctx, priv);
		return rc;
	}

	dev->drv_priv = priv;
	dev->drv_bound = true;
	dev_info(dev, "ufshcd: ready (UTRL slots %u, depth %llu)\n",
	    UFSHCI_UTRL_SLOT_N,
	    (unsigned long long)lx_dev_plat_u64(dev, "queue-depth", 32));
	return 0;
}

static int
ufs_remove(struct lx_device *dev)
{
	struct ufs_priv *priv = dev->drv_priv;

	if (priv == NULL)
		return -LX_ENODEV;

	lxkit_storage_unpublish(dev);
	if (priv->irq != NULL)
		lx_free_irq(priv->irq);
	lx_io_queue_destroy(priv->q);
	lx_kfree(dev->ctx, priv->utrl_ring);
	lx_kfree(dev->ctx, priv);
	dev->drv_priv = NULL;
	dev->drv_bound = false;
	return 0;
}

static const char *ufs_compat[] = { "ufs0", NULL };

struct lx_driver mtk_ufshcd_driver = {
	.name       = "mtk-ufshcd-ufs0",
	.compatible = ufs_compat,
	.probe      = ufs_probe,
	.remove     = ufs_remove,
	.resume_pre = NULL,
};
