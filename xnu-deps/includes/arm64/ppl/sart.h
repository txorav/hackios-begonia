/*
 * hackios-begonia: xnu-deps arm64/ppl/sart.h
 * ------------------------------------------
 * Non-SPTM (Apple PPL-era "classic" pmap) builds reference the SART
 * ("Secure Address Translation" service, Apple's MMU-side DMA-trust)
 * bootstrap exactly once:
 *
 *     osfmk/arm/pmap/pmap.c:88   #include <arm64/ppl/sart.h>
 *     osfmk/arm/pmap/pmap.c:2350 sart_bootstrap();
 *
 * (`optional nos_arm_pmap` builds in osfmk/conf/files.arm64:9.)
 * On macOS the header comes from Apple's SDK; on Linux the HackiOS port
 * supplies it here.
 *
 * The MT6785 (begonia) port runs the standard ARM GICv3 + SMMU world and
 * has no Apple SART hardware, so:
 *   - sart_bootstrap() is a valid no-op API-count-wise; the kernel-port
 *     team may fill in resource accounting inside the kernel port itself
 *     (they own osfmk/arm/pmap patches) or instantiate PCPU-less stubs.
 *   - No SART controller registers must ever be touched on MT6785.
 */

#ifndef _ARM64_PPL_SART_H
#define _ARM64_PPL_SART_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Apple PPL signature (mirrors what pmap_ios.c/hi_iokit would import from
 * the SDK): "Allowed SART regions" bookkeeping used by DMA translation
 * for kernel-owned SART-aware hardware (SMU, SARA...).  On MT6785 all
 * buffer pool sizing is managed by the IOMMU present in the SoC instead.
 */
void sart_bootstrap(void);

/* SART-registrable memory-range lying types used by other in-tree PPL
 * headers; kept minimal and unused on this target (0-sized features). */
typedef struct sart_range_s {
	uint64_t start;      /* physical address */
	uint64_t size;       /* bytes */
} sart_range_t;

#ifdef __cplusplus
}
#endif

#endif /* _ARM64_PPL_SART_H */
