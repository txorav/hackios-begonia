/*
 * lxkit_api.h — THE shared consumer contract between the lx shim layer and
 *               the three consumer kexts.
 *
 * ---------------------------------------------------------------------------
 * WHO INCLUDES THIS HEADER
 * ---------------------------------------------------------------------------
 *   Producer side (part of the wifi/lx tree):
 *     lxkit/lxkit_service.cpp, lxkit/lxkit_req.cpp, lxkit/lxkit_irq.cpp,
 *     drivers/mt6785 sources (they publish storage/touch devices).
 *
 *   Consumer side (part of the kexts tree):
 *     kexts/LxStorage           — storage contract (§STORAGE CONTRACT)
 *     kexts/FocalTouchIOHID     — touch contract (§TOUCH CONTRACT)
 *     kexts/MT6785Framebuffer   — NOT a consumer of lxkit (stands alone;
 *                                  display ships its own ADT reader) but
 *                                  kept on this include list for the one
 *                                  shared constant: LXKIT_NAME_LEN.
 *
 * The consumer kexts MUST NOT include lx/lx_dev.h, lx/lx_io.h, lx/lx_dma.h,
 * lx/lx_irq.h or lx/lx_kmem.h directly — lxkit_api.h is the ABI surface;
 * everything else is implementation detail subject to change.
 *
 * ---------------------------------------------------------------------------
 * ARCHITECTURE CONTEXT (which kext instantiates what)
 * ---------------------------------------------------------------------------
 *
 *   ┌──────────────────────────────────────────────────────────┐
 *   │  kexts/LxStorage (HOST kext, single live LxKitService)   │
 *   │  ┌─────────────────────────────────────────────────────┐ │
 *   │  │  LXKitService (IOService)                            │ │
 *   │  │   - walks ADT /arm-io children (msdc1, ufs0, i2c0,   │ │
 *   │  │     touchscreen) from the IODT plane                  │ │
 *   │  │   - builds struct lx_device (+res, plat, iomaps)      │ │
 *   │  │   - instantiates the drivers compiled INTO this kext  │ │
 *   │  │     (mtk_msdc_msdc1, mtk_ufshcd)                      │ │
 *   │  │   - publishes IOKit nubs:                             │ │
 *   │  │        LxKitBlockNub   (msdc1, ufs0)                  │ │
 *   │  │        LxKitTouchNub   (i2c0 + touchscreen IRQ)      │ │
 *   │  └──────────────────────────┬───────────────────────────┘ │
 *   │                             │ same-image C API calls       │
 *   │  LXBlockService / LXBlockDevice / LXMedia (GPT nubs)      │
 *   └──────────────────────────────────────────────────────────┘
 *               │                                  ▲
 *               │ IOKit match: LxKitTouchNub       │ IOKit match:
 *               ▼                                  │ IOProviderClass =
 *   ┌──────────────────────────────────────────────┐  LxKitStorageNub
 *   │  kexts/FocalTouchIOHID                        │  (user client for
 *   │   FocalTouchService : IOService               │   GetBootMediumInfo)
 *   │   - binds its OWN lx_context to the nub       │
 *   │   - executes lx/i2c0 + focaltech_fts.c        │
 *   │   - renders digitizer events to backboardd    │
 *   └───────────────────────────────────────────────┘
 *
 *   kexts/MT6785Framebuffer — sees the ADT disp-fb node directly; no lx.
 *
 * ---------------------------------------------------------------------------
 * WHY TWO LxKitService-free CONSUMERS?
 * ---------------------------------------------------------------------------
 * Cross-kext C calls are impossible (separate Mach-O, no shared symbol
 * resolution at load for kexts).  The kext↔kext boundary is therefore
 * pure IOKit (service matching nub ↔ consumer, properties as data), while
 * the lx↔driver boundary and the provider↔consumer path WITHIN one kext
 * image can use the plain C contract below.  Everything the spec calls the
 * "lxkit bridge" lives at the IOService level for cross-kext traffic.
 */

#ifndef LXKIT_API_H
#define LXKIT_API_H

/* ==========================================================================
 * Shared constants (versioned contract)
 * ========================================================================= */
#define LXKIT_ABI_VERSION        1
#define LXKIT_MAGIC            0x4c58494bu   /* "LXIK"                                */
#define LXKIT_CACHELINE          64
#define LXKIT_NAME_LEN           32

/* Boot medium identification (see GetBootMediumInfo consumer selector). */
#define LXKIT_MEDIUM_UFS         0x01u
#define LXKIT_MEDIUM_MSDC        0x02u
#define LXKIT_MEDIUM_RAMDISK     0x03u
#define LXKIT_MEDIUM_UNKNOWN     0x00u

/* ==========================================================================
 * CONTEXT
 *
 * One context == one instance of the lx runtime inside ONE kext image.
 *   - The host kext (LxStorage) creates the master context and hosts the
 *     master's IOService (LxKitService).
 *   - FocalTouchIOHID creates a *private* context bound INSIDE its image to
 *     its own provider nub (the LxKitTouchNub).
 *   - Contexts never share device lists, mutexes, or work queues.
 * ========================================================================== */

struct lx_workqueue;             /* lx_work.h                */
typedef struct lx_lock lx_lock;  /* lx_mutex.h struct lx_lock* */

/*
 * The context object.  Kernel-side layout is managed by lxkit; consumers
 * may peek at `abi_version`, `name`, and the service pointer, and must use
 * lxkit_copysink etc. for everything else.
 */
struct lx_context {
	u32                abi_version;    /* LXKIT_ABI_VERSION check            */
	u32                magic;          /* LXKIT_MAGIC check                  */
	const char        *name;           /* "lxkit-host" / "lxkit-touch"       */

	/* Privates (opaque to consumers; see lxkit_*.cpp for layouts). */
	void              *priv;           /* lxkit_service / glue engine        */
	struct lx_workqueue *wq;           /* one workqueue per context          */
	lx_lock           *ctx_lock;       /* protects device/driver registries  */
};

/* Create from driver glue — the context object the drivers receive. */
int  lxkit_core_init(struct lx_context *ctx, const char *name);
void lxkit_core_fini(struct lx_context *ctx);

/* Asserts ABI/version (fast, no allocation); returns 0 ok. */
int  lxkit_check_context(const struct lx_context *ctx);

/* ==========================================================================
 * STORAGE CONTRACT
 * ========================================================================== */

/* Maximum GPT children we publish per device.  Keep small — IOKit registry
 * enumeration is O(n) and begonia has one ufs + one µSD.                */
#define LXKIT_MAX_PARTITIONS    16

/* Some capacity helpers (see LXGPT scanner for the on-disk layout). */
struct lxkit_storage_desc {
	u32   abi_revision;      /* LXKIT_ABI_VERSION                        */
	u32   medium;             /* LXKIT_MEDIUM_*                           */

	char  name[LXKIT_NAME_LEN];   /* "msdc1" or "ufs0"                  */

	u64   block_size;         /* 512 typical; UFS logical size            */
	u64   num_blocks;         /* total logical blocks                     */
	u32   rw_posdd;           /* write protected? 0 = write-allowed       */

	u32   features;           /* bitmask LXKIT_ST_F_** defined below      */

	void *queue_fwd;          /* the lx_io_queue* (consumer-OPAQUE)       */

	/* Bring-up hooks: drivers fill at lxkit_storage_publish() time.     */
	void *driver_priv;        /* lx driver instance state                 */
};

/* Storage device feature bits */
#define LXKIT_ST_F_DMA          (1u << 0)   /* DMA engine live               */
#define LXKIT_ST_F_BOOT_PRIM    (1u << 1)   /* prefer this for rd=/root "dev=md0 → real dev resolve" */

#define LXKIT_ST_F_REMOVABLE    (1u << 2)   /* µSD: ejectable; report to kext */

/* ========================================================================== */
/* Client registration (consumed by kexts/LxStorage):                          */
/*                                                                             */
/* The *host* kext (LxStorage) also INSTANTIATES LxKitService, so it uses the   */
/* lxkit C API directly (same Mach-O).  This structure is the formal golue     */
/* between the two halves; it documents the async completion path that         */
/* lxkit drives on the lxkit workloop — i.e. exactly the IOCommandGate/         */
/* workloop "async completion via IOInterruptEventSource workloops" pattern    */
/* from the project spec (Phase 4 §3 Stage B and Phase 5 §4).                   */
/* ========================================================================== */
struct lxkit_storage_client;

struct lxkit_storage_client_ops {
	/* Called when the driver publishes a storage descriptor (device ready).
	 * `desc` is owned by lxkit; the consumer must copy out. */
	void (*on_storage_attached)(struct lxkit_storage_client *cl,
	    const struct lxkit_storage_desc *desc, void *lx_dev);

	/* Called on lxkit workloop when a request completes.
	 * `cookie` is the lx_io_request* token handed to lx_io_submit().
	 * `lx_err` 0 = ok, else negated lx_errno value. */
	void (*on_request_done)(struct lxkit_storage_client *cl,
	    void *cookie, int lx_err);

	/* Optional: card eject / device went away. */
	void (*on_storage_detached)(struct lxkit_storage_client *cl,
	    void *lx_dev);

	/* Optional: the consumer can finish reading GEOMETRY lazily. */
	void (*on_geometry_ready)(struct lxkit_storage_client *cl,
	    void *lx_dev);
};

/* A subscription point; consumers embed this and fill ->ops before call. */
struct lxkit_storage_client {
	const char                     *who;             /* debug tag          */
	struct lxkit_storage_client_ops ops;
	void                           *consumer_ctx;    /* kext-side state    */

	/* lxkit-side; consumers must leave untouched. */
	void                           *lxkit_priv;
};

/* Register a client callback; returns 0 ok. Safe single-threaded. */
int  lxkit_storage_subscribe(struct lxkit_storage_client *cl,
    struct lx_context *ctx);

/* The consumer path for every I/O: submit a request to the registered
 * storage driver of `lx_dev`.  Returns 0 ok; the completion arrives via
 * ops->on_request_done() on the lxkit workloop thread. */
int  lxkit_storage_submit(void *lx_dev, unsigned op, u64 lba, u32 count,
    void *virt_buf, void *cookie, void (*done_cb)(void *cb, void *ck, int err),
    void *cb_ctx);

/* ==========================================================================
 * TOUCH CONTRACT
 * ========================================================================== */

/* Multitouch report — the exact event payload format FocalTouchIOHID's
 * FocalTouchService consumes.  Generated by drivers/mt6785/focaltech_fts.c
 * (see the parser test in wifi/lx/tests/test_touch_parser.c).
 */
struct lxkit_touch_event {
	u32 slot_id;      /* 0..9, stable per tracked contact               */
	u16 tip;          /* 0 not-down / 1 down — matches HID TIP_SWITCH   */
	u16 id;           /* tracked id == slot_id (kept for offsetting)    */
	u16 x;            /* raw panel coords 0..1080 (to be scaled)        */
	u16 y;            /* raw panel coords 0..2340 (see service: conversion
	                   * to point space is the CONSUMER's job!)         */
	u16 pressure;     /* 1..255-style, panel-provided                   */
	u32 timestamp_ns; /* lx_ktime_get_ns() at packet decode             */
};

/* FocalTouchIOHID registers a callback through this structure from ITS OWN
 * image when it binds against the LxKitTouchNub (see kexts/FocalTouchIOHID/
 * FocalTouchLxGlue.cpp). */
typedef void (*lx_touch_client_fn)(void *consumer_ctx,
    const struct lxkit_touch_event *events, u32 nr_events, u32 datalen);

/* Registration: consumes one consumer callback; returns 0 ok. */
int  lx_touch_register_client(void *lx_dev, lx_touch_client_fn cb, void *ctx);

/* The lx driver's push API (spec-required name).  `buf` points at
 * struct lxkit_touch_event[nevents].  Invocation context: the lxkit
 * workloop thread (from the touchscreen IRQ handler deferred path — see
 * lx_irq.h); the consumer callback runs on the SAME thread.  This is the
 * main handoff to FocalTouchIOHID: it must not block!                        */
int  lx_touch_events_push_events(void *lx_dev,
    const void *events, u32 nr_events, u32 datalen);

/* ==========================================================================
 * MISC CONSUMER FACING UTILITIES
 * ========================================================================== */

/* Boot-medium info: used by the GetBootMediumInfo consumer selector to       */
/* describe the storage path to backboardd / launchd.                          */
struct lxkit_boot_medium_info {
	u32   medium;              /* LXKIT_MEDIUM_*                       */
	char  name[LXKIT_NAME_LEN];/* "msdc1"/"ufs0"/"md0"                 */
	u64   num_blocks;
	u64   block_size;
	u64   root_lba;            /* 0 = whole device; partition filter   */
};

/* Fill out boot medium info; ctx must be the host context (LxStorage's).      */
int  lxkit_get_boot_medium_info(struct lx_context *ctx,
    struct lxkit_boot_medium_info *out);

#endif /* LXKIT_API_H */
