/*
 * focaltech_fts.c — FocalTech FTS (focaltech,fts / goodix,gt9886 class)
 *                    I2C touchscreen driver for the lx layer, executed by
 *                    kexts/FocalTouchIOHID against the LxKitTouchNub
 *                    (i2c0 MMIO @ 0x11007000, fts slave 0x38, SPI 0x78).
 *
 * Linux original : drivers/input/touchscreen/focaltech_touch/fts5x06.c
 * lx port notes  :
 *   - The lx layer pre-maps i2c0 MMIO; the driver realizes MTK I2C bus
 *     (MT6785 i2c2/i2c0 fuse — see docs/hardware_map_mt6785.md §3).
 *   - Panel interrupt SPI -> i2c reg read 0x3C  (Touch events 0x8+ etc.)
 *     decodes the FTS standard up-track packet (see extract_touch()).
 *   - Decoded events are hand-carried to lx_touch_events_push_events()
 *     in the EXACT lxkit_touch_event format lxkit_api.h defines (so
 *     FocalTouchIOHID's parser stays trivial), and the parser's test
 *     lives at wifi/lx/tests/test_touch_parser.c.
 */

#include "lx_types.h"
#include "lx_dev.h"
#include "lx_irq.h"
#include "lx_iospace.h"
#include "lx_kmem.h"
#include "lx_printk.h"
#include "lxkit_api.h"

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* MTK i2c0 MMIO (mt6548/mt6735 style — "i2c controller v1")           */
/* ------------------------------------------------------------------ */
#define I2C_REG_WIDTH         0x1000u

#define I2C_ADDR              0x000u   /* slave address (deprecated) */
#define I2C_START             0x004u
#define   I2C_START_TRAN       (1u << 0)
#define I2C_INTR_STAT         0x008u
#define   I2C_HS_NACKERR       (1u << 2)
#define   I2C_ACKERR           (1u << 1)
#define   I2C_TRANSAC_COMP     (1u << 0)
#define I2C_FIFO_CTRL         0x00cu
#define   I2C_FIFO_CLR         (1u << 0)
#define I2C_SLAVE_ADDR        0x020u   /* newer register (MTK I2C v3) */
#define I2C_TRANSFER_LEN      0x024u
#define I2C_DELAY_LEN         0x028u
#define I2C_HS_SET            0x030u
#define I2C_SOFT_RESET        0x034u
#define I2C_IO_CONFIG         0x040u
#define I2C_PACUBLEN          0x05cu   /* packet length              */
#define I2C_TRANSAC_CFG       0x060u
#define   I2C_EN              (1u << 0)
#define   I2C_CLK_DIV         4u       /* standard 100 kHz divisor per MTK */
#define I2C_TX_DATA           0x200u   /* TX FIFO base               */
#define I2C_RX_DATA           0x300u   /* RX FIFO base               */
#define I2C_DMA_CON           0x318u   /* config: EN=0 sticks to PIO */

/* ------------------------------------------------------------------ *
 * FTS hardware contract (focaltech standard)
 * ------------------------------------------------------------------ */
#define FTS_SLAVE_ADDR        0x38u
#define FTS_REG_STATUS        0x3Cu    /* packet status + finger count */
#define FTS_POINTS_REG        0xD0u    /* start of per-point record  */
#define FTS_POINTS_LEN        6u       /* id, x_hi, x_lo, y_hi, y_lo, cfg */
#define FTS_MAX_POINTS        10u
#define FTS_IRQ_REG           0x01u    /* /chrom status page          */

/* ------------------------------------------------------------------ *
 * Driver instance state (bound inside FocalTouchIOHID's lx context)
 * ------------------------------------------------------------------ */
struct fts_priv {
	struct lx_device     *dev;
	struct lx_iomap      *io;          /* i2c0 MMIO (from LxKitTouchNub) */
	struct lx_iomap      *touch_spi_res; /* SPI seen from the touchscreen */
	struct lx_irq_cookie *irq;
	u32                  spi_no;
	u16                  max_x, max_y;
	/* last decoded frame — pushed straight to lxkit's touch client   */
	struct lxkit_touch_event   frame[FTS_MAX_POINTS];
};

static void fts_wr(struct fts_priv *p, u32 off, u32 v)
{
	lx_writel(p->io, off, v);
}
static u32 fts_rd(struct fts_priv *p, u32 off)
{
	return lx_readl(p->io, off);
}

/* ------------------------------------------------------------------ *
 * MTK I2C "PIO mode" transfer (controller ~v3):
 *   1. CLR FIFO, set transaction length, slave address.
 *   2. Write the register index (addr pointer) + 1 byte data into TX.
 *   3. START_TRAN, poll I2C_TRANSAC_COMP; verify no ACKERR/NACK.
 * FTS register access pattern is always [reg, data].
 * ------------------------------------------------------------------ */
static int
mts_i2c_write_reg(struct fts_priv *p, u8 reg, u8 val)
{
	u32 spin = 0x10000;

	fts_wr(p, I2C_FIFO_CTRL, I2C_FIFO_CLR);
	fts_wr(p, I2C_SLAVE_ADDR, FTS_SLAVE_ADDR);
	fts_wr(p, I2C_TRANSFER_LEN, 0x00020002u);   /* addr + reg + 1 data */
	fts_wr(p, I2C_TX_DATA, ((u32)reg) | ((u32)val << 8));
	fts_wr(p, I2C_TRANSAC_CFG, I2C_EN | I2C_CLK_DIV);
	fts_wr(p, I2C_START, I2C_START_TRAN);

	while ((fts_rd(p, I2C_INTR_STAT) & I2C_TRANSAC_COMP) == 0) {
		if ((--spin) == 0)
			return -LX_ETIMEDOUT;
	}
	if (fts_rd(p, I2C_INTR_STAT) & (I2C_ACKERR | I2C_HS_NACKERR))
		return -LX_EIO;
	return 0;
}

/* raw i2c read of `n` bytes starting at `reg` into buf (downstream     */
static int
fts_i2c_read(struct fts_priv *p, u8 reg, u8 *out, u32 n)
{
	u32 word, i, spin = 0x10000;

	fts_wr(p, I2C_FIFO_CTRL, I2C_FIFO_CLR);
	fts_wr(p, I2C_SLAVE_ADDR, FTS_SLAVE_ADDR);
	fts_wr(p, I2C_TRANSFER_LEN, 0x00010001u | (n << 16));
	fts_wr(p, I2C_TX_DATA, reg);
	fts_wr(p, I2C_TRANSAC_CFG, I2C_EN | I2C_CLK_DIV);
	fts_wr(p, I2C_START, I2C_START_TRAN);

	while ((fts_rd(p, I2C_INTR_STAT) & I2C_TRANSAC_COMP) == 0) {
		if ((--spin) == 0)
			return -LX_ETIMEDOUT;
	}
	if (fts_rd(p, I2C_INTR_STAT) & (I2C_ACKERR | I2C_HS_NACKERR))
		return -LX_EIO;

	for (i = 0; i < n; i += 4) {
		word = fts_rd(p, I2C_RX_DATA + (i / 4) * 4);
		out[i]     = (u8)(word & 0xff);
		if (i + 1 < n) out[i + 1] = (u8)((word >> 8)  & 0xff);
		if (i + 2 < n) out[i + 2] = (u8)((word >> 16) & 0xff);
		if (i + 3 < n) out[i + 3] = (u8)((word >> 24) & 0xff);
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * ISR — deferred to the lxkit workloop by the lxkit IRQ engine; here
 * we poll the FTS status register, decode the contact slots and push
 * them into the lxkit touch client in one heap-free record frame.
 * ------------------------------------------------------------------ */
static int
fts_irq_handler(int irq_no, void *arg)
{
	struct fts_priv *priv = arg;
	u8  status[FTS_MAX_POINTS * FTS_POINTS_LEN + 1];
	u8  reg = FTS_REG_STATUS;
	u32 nr = 0, i;
	int rc;

	(void)irq_no;

	/* [0] status/count byte, [1..] per-point FTS_POINTS_LEN records. */
	rc = fts_i2c_read(priv, reg, status, 1 + FTS_MAX_POINTS * FTS_POINTS_LEN);
	if (rc != 0)
		return LX_IRQ_HANDLED;          /* transient i2c error   */

	nr = status[0] & 0x3fu;                 /* upper bits = gesture  */
	if (nr == 0 || nr > FTS_MAX_POINTS)
		return LX_IRQ_HANDLED;          /* release-only packet   */

	lx_memset(priv->frame, 0, sizeof(priv->frame));
	for (i = 0; i < nr; i++) {
		const u8 *pt = &status[1 + i * FTS_POINTS_LEN];
		struct lxkit_touch_event *ev = &priv->frame[i];

		ev->slot_id  = pt[0];
		ev->id       = ev->slot_id;
		/* FTS raw x/y are 16-bit big-endian pairs in pt[1..4].     */
		ev->x        = (u16)(((u16)pt[1] << 8) | pt[2]);
		ev->y        = (u16)(((u16)pt[3] << 8) | pt[4]);
		ev->pressure = 0xffu;                /* FTS gives no pressure */
		/* Memory barrier: 1=touch, or the LxKit contract's "TIP".  */
		ev->tip      = 1u;
		ev->timestamp_ns = (u32)lx_ktime_get_ns();
	}

	/* Forward the whole frame; the consumer (FocalTouchService) RAW
	 * decodes this same struct (no extra marshalling).  Errors are
	 * fine: `consume` never blocks, we drop if there's no listener */
	lx_touch_events_push_events(priv->dev, priv->frame, nr,
	    (u32)(nr * sizeof(struct lxkit_touch_event)));
	return LX_IRQ_HANDLED;
}

/* ------------------------------------------------------------------ *
 * probe / remove
 * ------------------------------------------------------------------ */
static int
fts_probe(struct lx_device *dev)
{
	struct fts_priv *priv;
	struct lx_iomap *io;
	u8 chip_id[2];
	int rc;

	if (dev->drv_bound)
		return -LX_EBUSY;

	io = lx_dev_iomap(dev, 0);
	if (io == NULL)
		return -LX_ENODEV;

	pr_info("fts: probe i2c0 0x%llx spi=%u addr=0x%x\n",
	    (unsigned long long)io->phys, lx_dev_res_irq(dev, 0),
	    FTS_SLAVE_ADDR);

	priv = lx_kzalloc(dev->ctx, sizeof(*priv), LX_GFP_KERNEL | LX_GFP_ZERO);
	if (priv == NULL)
		return -LX_ENOMEM;
	priv->dev = dev;
	priv->io  = io;

	/* firmware bring-up: soft reset (write 0x54 to FTS_REG 0x80),
	 * then wait for the ready status.  Defaults for bringup.       */
	rc = mts_i2c_write_reg(priv, 0x80, 0x54);    /* FTS_REG_RESET */
	if (rc != 0)
		pr_warn("fts: reset command failed (%d) — continuing\n", rc);

	/* identify controller (chip id 0x8727…) */
	if (fts_i2c_read(priv, 0xA6, chip_id, 2) == 0) {
		pr_info("fts: firmware=0x%02x%02x\n", chip_id[0], chip_id[1]);
	} else {
		pr_warn("fts: chip-id read failed; try after 2 soft resets\n");
	}

	/* panel size default resolution — begonia is 1080×2340.
	 * Consumers scale the raw values (lxkit_api.h touch contract),
	 * so keeping raw 16-bit values with correct orientation is fine */
	priv->max_x = 1080u;
	priv->max_y = 2340u;

	/* panel IRQ (SPI) — flow via lxkit's IOKit-level GIC registration. */
	priv->irq = lx_request_irq(dev, lx_dev_res_irq(dev, 0), LX_IRQF_ONESHOT,
	    fts_irq_handler, priv, "fts");
	if (priv->irq == NULL)
		pr_warn("fts: no hard SPI — bring-up will need lx_softirq_attach\n");

	dev->drv_priv = priv;
	dev->drv_bound = true;
	dev_info(dev, "fts: ready (i2c slave 0x%02x)\n", FTS_SLAVE_ADDR);
	rc = 0;
	return rc;
}

static int
fts_remove(struct lx_device *dev)
{
	struct fts_priv *priv = dev->drv_priv;

	if (priv == NULL)
		return -LX_ENODEV;
	if (priv->irq != NULL)
		lx_free_irq(priv->irq);
	lx_kfree(dev->ctx, priv);
	dev->drv_priv = NULL;
	dev->drv_bound = false;
	return 0;
}

static const char *fts_compat[] = { "touchscreen", "i2c-touchscreen", NULL };

/* Executed by kexts/FocalTouchIOHID against the LxKitTouchNub device.  */
struct lx_driver focal_fts_driver = {
	.name       = "focaltech-ft5x06",
	.compatible = fts_compat,
	.probe      = fts_probe,
	.remove     = fts_remove,
};
