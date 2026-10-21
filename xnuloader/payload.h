/*
 * payload.h — kernel-payload manifest shared between mkxnuimg.py (packager)
 * and xnuloader (stage-2 loader).  This is the blob that is gzip-compressed
 * and placed as the Android boot.img "kernel" section; lk loads it at
 * 0x40080000 and the loader walks upward in memory to find the manifest
 * (mkxnuimg appends it after the loader binary, page-aligned).
 */
#ifndef _XLDR_PAYLOAD_H_
#define _XLDR_PAYLOAD_H_

#include <stdint.h>

#define PAYLOAD_MAGIC       "XLDRPAY1"
#define PAYLOAD_VERSION     1
#define PAYLOAD_MAX_SECT    4

/* Manifest is little-endian (host build LE, arm64 device LE). */
struct payload_section {
	char     name[8];    /* NUL-padded: "adt", "kernel", "ramdisk" */
	uint32_t flags;
	uint32_t reserved;
	uint64_t offset;     /* relative to manifest base */
	uint64_t size;
};

struct payload_manifest {
	uint8_t  magic[8];              /* "XLDRPAY1" */
	uint32_t version;
	uint32_t section_count;         /* valid sections in sect[] */
	uint64_t total_size;            /* whole payload (manifest + data) */
	uint64_t phys_base;             /* 0x40000000 DRAM base */
	uint64_t virt_base;             /* kernel virtual base (gVirtBase) */
	uint64_t fb_base;               /* framebuffer physical base (0 = default) */
	uint64_t fb_size;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t fb_stride;
	uint32_t fb_depth;              /* bpp */
	struct payload_section sect[PAYLOAD_MAX_SECT];

	/* bootargs: offset relative to manifest base (0 when absent) */
	uint32_t cmdline_offset;
	uint32_t cmdline_len;              /* ≤ XLDR BOOT (608) */
};

#define SECT_ADT      0
#define SECT_KERNEL   1
#define SECT_RAMDISK  2

#endif /* _XLDR_PAYLOAD_H_ */
