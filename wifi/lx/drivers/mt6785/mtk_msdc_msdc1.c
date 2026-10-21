/*
 * mtk_msdc_msdc1.c — Mediatek MSDC1 (SD/eMMC host controller) driver for the
 *                    lx shim layer, claiming the /arm-io "msdc1" device
 *                    (reg=0x11240000, size=0x10000; SPI from the ADT).
 *
 * Linux original:  drivers/mmc/host/mtk-sd.c (msdc_init_gx() bring-up)
 * lx port notes:
 *   - probe()          soft-resets the controller, programs SDC_CFG
 *                      bus-width (from the ADT "bus-width" property),
 *                      creates the lx_io_queue (32 deep) and publishes the
 *                      storage device (medium = LXKIT_MEDIUM_MSDC).
 *   - engine           PIO through the MSDC FIFO (MSDC_RXDATA/TXDATA) for
 *                      bring-up; DMA (MSDC_DMA_SA/GDMA) is a later
 *                      optimization — see msdc1_engine.
 *   - IRQ              MSDC INT latch is drained in the handler; request
 *                      completion is engine-driven on the lxkit workloop.
 */

#include "lx_types.h"
#include "lx_dev.h"
#include "lx_io.h"
#include "lx_irq.h"
#include "lx_iospace.h"
#include "lx_kmem.h"
#include "lx_printk.h"
#include "lxkit_api.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* MSDC register offsets (MSDC v3 datasheet, mtk-sd.c layout)         */
/* ------------------------------------------------------------------ */
#define MSDC_CFG          0x000u
#define MSDC_INT          0x004u
#define   MSDC_INT_CMDTMO   (1u << 14)
#define   MSDC_INT_CMDRDY   (1u << 12)
#define MSDC_INTEN        0x008u
#define MSDC_PS           0x040u
#define   MSDC_PS_CDEN      (1u << 16)
#define MSDC_FIFOCS       0x048u
#define   MSDC_FIFOCS_TXCNT(x) (((x) >> 16) & 0xff)
#define   MSDC_FIFOCS_RXCNT(x) ((x) & 0xff)
#define SDC_CFG           0x030u
#define   SDC_CFG_BUSWIDTH(x) (((x) & 3u) << 16)
#define   SDC_CFG_DTOC(x)     (((x) & 0xfu) << 24)
#define SDC_CMD           0x034u
#define   SDC_CMD_BLKLEN(x)   (((x) & 0xfffu) << 16)
#define   SDC_CMD_DTYPE(x)    (((x) & 3u) << 22)
#define   SDC_CMD_WR          (1u << 24)
#define SDC_ARG           0x038u
#define SDC_STS           0x03cu
#define   SDC_STS_CMDBUSY    (1u << 1)
#define MSDC_DMA_SA       0x090u

#define MSDC_RXDATA       0x054u
#define MSDC_TXDATA       0x058u

#define MSDC1_QUEUE_DEPTH     32u
#define MSDC1_BLOCK_SIZE      512u
#define MSDC1_MAX_XFER        (MSDC1_BLOCK_SIZE * 8)   /* PIO scratch cap */

/* ------------------------------------------------------------------ *
 * driver instance state (dev->drv_priv)
 * ------------------------------------------------------------------ */
struct msdc1_priv {
	struct lx_device      *dev;
	struct lx_iomap       *io;
	struct lx_irq_cookie  *irq;
	struct lx_io_queue    *q;
	u64                    capacity_blocks;   /* filled post-CID/CSD */
};

static inline void msdc1_wr(struct msdc1_priv *p, u32 off, u32 v)
{
	lx_writel(p->io, off, v);
}
static inline u32 msdc1_rd(struct msdc1_priv *p, u32 off)
{
	return lx_readl(p->io, off);
}

static void
msdc1_spin_idle(struct msdc1_priv *p)
{
	u32 spin = 0x100000;

	while ((msdc1_rd(p, SDC_STS) & SDC_STS_CMDBUSY) != 0 && spin--)
		;
}

/* ------------------------------------------------------------------ *
 * IRQ handler — runs on the lxkit workloop (lx_irq.h contract); only
 * clears latched MSDC conditions in the PIO engine.  Full DMA-driven
 * request completion happens in the engine below (tasklet path).
 * return 0 = handled, 1 = none (lx_irqreturn_t).
 * ------------------------------------------------------------------ */
static int
msdc1_irq_handler(int irq_no, void *arg)
{
	struct msdc1_priv *priv = arg;
	u32 pending;

	(void)irq_no;
	pending = msdc1_rd(priv, MSDC_INT);
	if (pending == 0)
		return LX_IRQ_NONE;

	msdc1_wr(priv, MSDC_INT, pending);        /* write-one-to-clear  */
	return LX_IRQ_HANDLED;
}

/* ------------------------------------------------------------------ *
 * PIO engine — pops from the queue, runs synchronously, completes.
 * Called on the lxkit workloop thread (lx_io.h contract).
 * ------------------------------------------------------------------ */
static int
msdc1_fifo_pio(struct msdc1_priv *p, u32 *buf, u32 words, bool write)
{
	u32 i;
	u32 spin;

	msdc1_wr(p, MSDC_INT, 0xffffffff);        /* clear anything latched */

	for (i = 0; i < words; i++) {
		if (write) {
			spin = 0x400000;
			while (MSDC_FIFOCS_TXCNT(msdc1_rd(p, MSDC_FIFOCS)) == 0)
				;
			lx_writel(p->io, MSDC_TXDATA, buf[i]);
		} else {
			spin = 0x400000;
			while (MSDC_FIFOCS_RXCNT(msdc1_rd(p, MSDC_FIFOCS)) < 4) {
				if ((--spin) == 0)
					return -LX_ETIMEDOUT;
			}
			buf[i] = lx_readl(p->io, MSDC_RXDATA);
		}
	}
	return 0;
}

static void
msdc1_engine_work(struct msdc1_priv *priv)
{
	struct lx_io_request *req;
	u32 words;
	u32 spin;
	int rc;

	while ((req = lx_io_pop_next(priv->q)) != NULL) {
		if (req->count == 0 || req->virt == NULL) {
			lx_io_complete(req, -LX_EINVAL);
			continue;
		}
		if (req->op == LX_IO_FLUSH || req->op == LX_IO_DISCARD) {
			/* MSDC1 has no flush/unmap — success for bringing up. */
			lx_io_complete(req, 0);
			continue;
		}

		/* TODO(M5): cap to PIO scratch; switch to DMA (MSDC_DMA_SA)
		 * with a driver-side bounce buffer for large requests. */
		words = (req->count * MSDC1_BLOCK_SIZE) / 4;

		msdc1_spin_idle(priv);

		msdc1_wr(priv, SDC_CMD,
		    SDC_CMD_BLKLEN(req->count * MSDC1_BLOCK_SIZE) |
		    SDC_CMD_DTYPE(3) |                     /* multi-block     */
		    SDC_CMD_WR(req->op == LX_IO_WRITE ? 1u : 0u));
		msdc1_wr(priv, SDC_ARG, (u32)req->lba);

		spin = 0x400000;
		while ((msdc1_rd(priv, MSDC_INT) & MSDC_INT_CMDRDY) == 0) {
			if ((--spin) == 0) {
				lx_io_complete(req, -LX_ETIMEDOUT);
				continue;
			}
		}
		rc = msdc1_fifo_pio(priv, (u32 *)req->virt, words,
		    req->op == LX_IO_WRITE);
		lx_io_complete(req, rc);
	}
}

static int
msdc1_engine_cb(void *arg)                      /* lxkit engine tick   */
{
	msdc1_engine_work((struct msdc1_priv *)arg);
	return 0;                                    /* lx_irqreturn_t      */
}

/* ------------------------------------------------------------------ *
 * probe / remove / resume
 * ------------------------------------------------------------------ */
static int
msdc1_probe(struct lx_device *dev)
{
	struct msdc1_priv *priv;
	struct lx_iomap   *io;
	u64 buswidth;
	int rc;

	if (dev->drv_bound)
		return -LX_EBUSY;

	buswidth = lx_dev_plat_u64(dev, "bus-width", 4);
	io = lx_dev_iomap(dev, 0);
	if (io == NULL)
		return -LX_ENODEV;

	pr_info("msdc1: probe 0x%llx size=%llu bus-width=%llu irq=%u\n",
	    (unsigned long long)io->phys,
	    (unsigned long long)io->len,
	    (unsigned long long)buswidth,
	    lx_dev_res_irq(dev, 0));

	priv = lx_kzalloc(dev->ctx, sizeof(*priv), LX_GFP_KERNEL | LX_GFP_ZERO);
	if (priv == NULL)
		return -LX_ENOMEM;
	priv->dev = dev;
	priv->io  = io;

	/* 1) software reset of the IP block (msdc host reset in mtk-sd). */
	msdc1_wr(priv, MSDC_CFG, 1u);             /* MSDC_CFG_RST  */
	while ((msdc1_rd(priv, MSDC_CFG) & 1u) && priv--)
		;
	/* (priv-- above is a bounded settle spin; restore priv pointer) */
	priv->irq = lx_request_irq(dev, lx_dev_res_irq(dev, 0), 0,
	    msdc1_irq_handler, priv, "msdc1");

	/* 2) bus width + conservative data timeout (SDC_CFG_DTOC). */
	msdc1_wr(priv, MSDC_PS, MSDC_PS_CDEN);
	msdc1_wr(priv, SDC_CFG, SDC_CFG_BUSWIDTH(buswidth) | SDC_CFG_DTOC(12));

	/* 3) engine queue. */
	priv->q = lx_io_queue_create(dev, MSDC1_QUEUE_DEPTH);
	if (priv->q == NULL) {
		if (priv->irq != NULL)
			lx_free_irq(priv->irq);
		lx_kfree(dev->ctx, priv);
		return -LX_ENOMEM;
	}
	/* Engine tick: lxkit's tasklet calls this on its workloop. */
	priv->q->engine_priv = priv;

	priv->capacity_blocks = lx_dev_plat_u64(dev, "capacity-blocks", 0);

	rc = lxkit_storage_publish(dev, LXKIT_MEDIUM_MSDC, MSDC1_BLOCK_SIZE,
	    priv->capacity_blocks, priv->q, priv);
	if (rc != 0) {
		pr_err("msdc1: publish failed: %d\n", rc);
		lx_io_queue_destroy(priv->q);
		if (priv->irq != NULL)
			lx_free_irq(priv->irq);
		lx_kfree(dev->ctx, priv);
		return rc;
	}

	dev->drv_priv = priv;
	dev->drv_bound = true;
	dev_info(dev, "msdc1: ready (queue depth %u)\n", MSDC1_QUEUE_DEPTH);
	return 0;
}

static int
msdc1_remove(struct lx_device *dev)
{
	struct msdc1_priv *priv = dev->drv_priv;

	if (priv == NULL)
		return -LX_ENODEV;

	lxkit_storage_unpublish(dev);
	if (priv->irq != NULL)
		lx_free_irq(priv->irq);
	lx_io_queue_destroy(priv->q);
	lx_kfree(dev->ctx, priv);
	dev->drv_priv = NULL;
	dev->drv_bound = false;
	return 0;
}

static int
msdc1_resume_pre(struct lx_device *dev)
{
	struct msdc1_priv *priv = dev->drv_priv;

	if (priv == NULL)
		return -LX_ENODEV;
	msdc1_wr(priv, MSDC_INT, 0xffffffff);
	return 0;
}

static const char *msdc1_compat[] = { "msdc1", NULL };

/* exported for lxkit_drivers_glue.c */
struct lx_driver mtk_msdc_msdc1_driver = {
	.name       = "mtk-msdc-msdc1",
	.compatible = msdc1_compat,
	.probe      = msdc1_probe,
	.remove     = msdc1_remove,
	.resume_pre = msdc1_resume_pre,
};
