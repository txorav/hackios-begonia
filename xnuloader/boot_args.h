/*
 * boot_args.h — mirror of xnu/pexpert/pexpert/arm64/boot.h's boot_args.
 * Layout MUST match exactly what the ADT/kernel expects (xnu-8796.121.2).
 * BOOT_LINE_LENGTH matches iBoot's IBOOT_MAX_ENV_VAR_DATA_SIZE (iOS 16
 * era): 608 bytes (see plan §3 Phase 2 item 3).
 */
#ifndef _XLDR_BOOT_ARGS_H_
#define _XLDR_BOOT_ARGS_H_

#include <stdint.h>

#define KBOOT_ARGS_REVISION   2   /* kBootArgsRevision2 */
#define KBOOT_ARGS_VERSION    2   /* kBootArgsVersion2  */

#define XLDR_BOOT_LINE_LENGTH 608

struct boot_video {
	uint64_t v_baseAddr;
	uint64_t v_display;
	uint64_t v_rowBytes;
	uint64_t v_width;
	uint64_t v_height;
	uint64_t v_depth;
};

struct boot_args {
	uint16_t Revision;
	uint16_t Version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t *deviceTreeP;         /* physical address, 8-aligned */
	uint32_t deviceTreeLength;
	char CommandLine[XLDR_BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#endif /* _XLDR_BOOT_ARGS_H_ */
