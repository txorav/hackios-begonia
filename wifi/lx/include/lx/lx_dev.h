/*
 * lx_dev.h — the Linux-ish driver model for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS MODELS
 * ---------------------------------------------------------------------------
 * Linux model:  platform_device + driver-model probe; devices describe
 *               resources (MMIO ranges, IRQ numbers, name, compatible).
 * lx model:     the same, but the resources are ALREADY KNOWN at
 *               construction time — they were read from the Apple
 *               DeviceTree /arm-io children by lxkit (see
 *               lxkit/lxkit_service.cpp; property tables live in
 *               docs/hardware_map_mt6785.md §3).
 *
 *   ADT child `/arm-io/msdc1`          struct lx_device
 *   ----------------------------------------------------------
 *   name = "msdc1"                     .name = "msdc1"
 *   reg = <0x11240000 0x10000>         res[i] = { LX_RES_MMIO, addr, size }
 *   interrupt=MSDC1_SPI                res[j] = { LX_RES_IRQ, spi, 0 }
 *   bus-width=<8>                      platform[i] = { "bus-width", 8 }
 *
 * The `platform data` concept (struct platform_device::platform_data) is
 * replaced by **platform properties**: a small, fixed-by-bringup table of
 * u64 key/value pairs built by lxkit from ADT scalars, so a driver can read
 * "bus-width=8", "clock-frequency=..." etc. without a FDT parser.
 *
 * MMIO maps for every MMIO resource are created by lxkit eagerly (so drivers
 * do not map/unmap at runtime; one mapping per device is enough for every
 * MT6785 IP we host — single-range controllers).  The iomaps array parallels
 * the resource list: iomaps[i] resolves dev->res[i en merchi MMIO].
 */

#ifndef LX_DEV_H
#define LX_DEV_H

#include "lx_types.h"
#include "lx_list.h"
#include "lx_iospace.h"   /* struct lx_iomap */
#include <stddef.h>       /* size_t (freestanding, idempotent) */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

struct lx_context;           /* lxkit_api.h */
struct lx_driver;            /* this file */

/* bring-up cap: each hosted IP is single-MMIO; 4 is generous. */
#define LX_DEV_MAX_IOMAPS 4

/* ---------------------------------------------------------------------
 * Resource types.  Only MMIO and IRQ exist on this bus: the ADT has no
 * memory-class resource concept for these devices.
 * ------------------------------------------------------------------ */
enum lx_res_type {
	LX_RES_MMIO = 0,     /* device register range (physical)          */
	LX_RES_IRQ,          /* SPI number                               */
	LX_RES_NUM,
};

/*
 * One resource row.  MMIO rows carry (phys, size); IRQ rows carry a
 * single u32 SPI.
 */
struct lx_resource {
	struct lx_resource *next;
	enum lx_res_type    type;
	u64                 a;      /* MMIO: physical address; IRQ: SPI number */
	u64                 b;      /* MMIO: length;          IRQ: 0           */
	char                name[24];
};

/* A device on the (virtual) lx bus: created by lxkit, handed to drivers. */
struct lx_device {
	char                name[32];      /* "msdc1" / "ufs0" / "i2c0" / "touchscreen" */
	struct lx_resource *res;           /* singly-linked resource list             */
	unsigned            nres;

	/* Parallel array of pre-mapped MMIOs (index == MMIO resource ord). */
	struct lx_iomap    *iomaps[LX_DEV_MAX_IOMAPS];
	unsigned            niomaps;

	struct lx_context  *ctx;           /* owning lxkit context                   */

	/* Platform properties (ADT scalars → key/value rows, e.g. bus-width). */
	struct lx_ppair    *platform;
	unsigned            nplatform;

	/* Driver binding: NULL until a probe() match. */
	struct lx_driver   *drv;
	bool                drv_bound;
	void               *drv_priv;      /* driver instance state                 */

	/* Runtime record on the lxkit "lx bus". */
	struct lx_list_head bus_entry;

	/* IOService-facing token when the device came from a registry nub
	 * (LxKitDeviceNub*); NULL when lxkit created it standalone. */
	void               *iokit_nub;
};

/* One platform key/value row ("bus-width" -> 8). */
struct lx_ppair {
	char key[24];
	u64  value;
};

/* ---------------------------------------------------------------------
 * driver: probe/remove heartbeat.
 * ------------------------------------------------------------------ */
struct lx_driver {
	const char  *name;                 /* "mtk-msdc-msdc1" etc.              */
	const char **compatible;           /* NULL-terminated list of device     */
	                                    /* names this driver claims           */
	int         (*probe)(struct lx_device *dev);
	int         (*remove)(struct lx_device *dev);
	/* optional resume hook (retuning after sleep / card reborrow). */
	int         (*resume_pre)(struct lx_device *dev);
};

/* Register a driver with the context's lx bus; probing runs lazily when a
 * matching device is (or becomes) registered. */
int  lx_driver_register(struct lx_context *ctx, struct lx_driver *drv);
void lx_driver_unregister(struct lx_driver *drv);

/* Device registry (called by lxkit when it synthesizes the ADT children). */
int  lx_device_register(struct lx_context *ctx, struct lx_device *dev);
void lx_device_unregister(struct lx_device *dev);

/* ---------------------------------------------------------------------
 * strcmp (freestanding).
 * ------------------------------------------------------------------ */
static inline int lx_strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* ---------------------------------------------------------------------
 * Resource accessors (typesafe views of struct lx_resource).
 * ------------------------------------------------------------------ */
static inline struct lx_resource *
lx_dev_get_res(struct lx_device *dev, enum lx_res_type type, unsigned idx)
{
	struct lx_resource *r;
	unsigned i = 0;

	for (r = dev->res; r != NULL; r = r->next) {
		if (r->type != type)
			continue;
		if (i == idx)
			return r;
		i++;
	}
	return NULL;
}

static inline u64 lx_dev_res_addr(struct lx_device *dev, unsigned idx)
{
	struct lx_resource *r = lx_dev_get_res(dev, LX_RES_MMIO, idx);
	return r ? r->a : 0;
}

static inline u64 lx_dev_res_size(struct lx_device *dev, unsigned idx)
{
	struct lx_resource *r = lx_dev_get_res(dev, LX_RES_MMIO, idx);
	return r ? r->b : 0;
}

static inline u32 lx_dev_res_irq(struct lx_device *dev, unsigned idx)
{
	struct lx_resource *r = lx_dev_get_res(dev, LX_RES_IRQ, idx);
	return r ? (u32)r->a : 0;
}

/* Pre-mapped iomap for the idx'th MMIO resource (see class comment). */
static inline struct lx_iomap *lx_dev_iomap(struct lx_device *dev,
    unsigned idx)
{
	return (idx < dev->niomaps) ? dev->iomaps[idx] : NULL;
}

static inline u64 lx_dev_plat_u64(struct lx_device *dev,
    const char *key, u64 dflt)
{
	unsigned i;
	for (i = 0; i < dev->nplatform; i++)
		if (lx_strcmp(dev->platform[i].key, key) == 0)
			return dev->platform[i].value;
	return dflt;
}

/* Standard debug print of an lx_device (bring-up help). */
void lx_dev_dump(struct lx_device *dev);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_DEV_H */
