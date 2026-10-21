/*
 * Copyright (c) 2026 HackiOS begonia port project.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License is
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/*
 * MediaTek Helio G90T / MT6785V ("Xiaomi Redmi Note 8 Pro", board codename
 * `begonia`) platform definitions.
 *
 * This header plays the same role H13.h/H14.h/VMAPPLE.h play for the Apple
 * SoCs consulted from pexpert/arm64/board_config.h.  It is deliberately
 * NOT derived from apple_arm64_common.h:
 *
 *  - apple_arm64_common.h sets APPLE_ARM64_ARCH_FAMILY, which enables the
 *    Apple-only system register surface (HID0..HID16, APRR, PPL, AMX, CPM,
 *    ACC/CPU_OVRD, Apple fast-IPI registers, KTRR/CTRR).  On Cortex-A55/A76
 *    those registers are UNIMPLEMENTED and any MSR/MRS to them traps (ESR
 *    EC_UNALLOCATED_REGISTER) as a machine check in EL0/EL1.
 *
 *  - Everything in here is restricted to the architectural (ARMV8.2-A)
 *    register space plus the GICv3 system-register interface, which the
 *    MediaTek system firmware trusted-firmware (BL31) exposes normally.
 *
 * Canonical hardware/boot contract: docs/hardware_map_mt6785.md in the
 * hackios-begonia repository.  All constants below must agree with that
 * document; if they drift, update the document FIRST, then this header.
 */

#ifndef _PEXPERT_ARM64_MT6785_H
#define _PEXPERT_ARM64_MT6785_H

/*
 * SoC identity.  MT6785 is used by per-file #elif chains (e.g. the cache
 * line table in osfmk/arm64/proc_reg.h) and by the MT6785-specific glue in
 * osfmk/arm64/mt6785/, osfmk/arm64/cpu.c and pexpert/arm/.
 */
#define MT6785                                  1
#define MT6785SOC                               1
/* Cortex core identities kept as own defines; proc_reg.h keys on them. */
#define CORTEXMEDIATEKA55                       1
#define CORTEXMEDIATEKA76                       1

/*
 * ---------------------------------------------------------------------------
 * CPU feature deltas vs Apple H1x (all ARMv8.2-A architectural)
 * ---------------------------------------------------------------------------
 */

/* ARMv8.1+ hardware PAN (PSTATE.PAN) is present on A55/A76. */
#define __ARM_PAN_AVAILABLE__                   1

/* FEAT_SB (Speculation Barrier) is ARMv8.4/8.5 -- not present on A76/A55. */
/* __ARM_SB_AVAILABLE__ intentionally NOT defined. */

/*
 * MMU / page size configuration.
 *
 * XNU is burned with 16 KiB page sizes for both TTBR0 and TTBR1 (the loader
 * contract) -- ID_AA64MMFR0_EL1.TGran16 = 0x1 on MT6785.  ARM_SMALL_PAGES
 * must never be defined for this build (16K page is fixed at boot).
 */
#define __ARM_16K_PG__                          1

/*
 * The ARM64_LARGE_MEMORY (VM_MIN_KERNEL_ADDRESS = -2TB) configuration is a
 * MacOSX-only switch; the embedded (iPhoneOS-style) build we target keeps the
 * small-memory window (VM_MIN_KERNEL_ADDRESS = 0xffffffe000000000), which is
 * what the manual hardware map loader contract assumes.  Do not define
 * ARM_LARGE_MEMORY for MT6785 builds.
 */

#if !ARM_LARGE_MEMORY
/*
 * Embedded XNU builds place each user pmap root-pmap L1 table inside a
 * single page ("subpage L1"), which suits the 16 KiB granule: an L1 table
 * only needs 8 slots (2^4 = 16 bits of VA width per TTE... the level-1
 * window for a 47-bit user VA with 16K pages is 8 slots * 16 KiB granule
 * coverage blocks).  Keep the classic embedded behavior.
 */
#define __ARM64_PMAP_SUBPAGE_L1__               1
#define __ARM64_PMAP_KERN_SUBPAGE_L1__          1
#endif /* !ARM_LARGE_MEMORY */

/*
 * Architectural timer.  MT6785 has the standard ARMv8 system counter
 * (a single 13 MHz "system counter", CNTFRQ_EL0 == 13_000_000), NOT the
 * 24 MHz Apple always-on counter.  We use the EL1 *virtual* timer
 * (CNTV_TVAL_EL0 == "decrementer"), exactly like the pre-AppleSoC K32/K24
 * builds did.  Do NOT define HAS_APPLE_GENERIC_TIMER.
 */
#define ARM_ARCH_TIMER                          1

/*
 * Standard software characteristics.  Matches MT6785 silicon:
 *  __ARM_VFP__      4 -- ARMv8.2 SVE-unsupported, but VFPv4-func. VA regs.
 *  swap             1 -- CAS/LDXR exist.
 *  crypto           1 -- A55/A76 implement the ARMv8 crypto extensions.
 *  coherent cache   1 -- MTK big.LITTLE participates in the DSM
 *                         (hardware coherent dynamic monitoring Orbit), and
 *                         the L2/L3 SLA snoops SeC requests.  In-tree caches
 *                         should still fall back on DMB/DSB semantics.
 *  IC_NOALIAS_ICACHE 1 -- i-cache invalidation by VA/IC IVAU style IC IVAU.
 */
#define __ARM_ARCH__                            8
#define __ARM_VMSA__                            8
#define __ARM_VFP__                             4
#define __ARM_COHERENT_CACHE__                  1
#define __ARM_COHERENT_IO__                     1
#define __ARM_IC_NOALIAS_ICACHE__               1
#define __ARM_ENABLE_SWAP__                     1
#define __ARM_V8_CRYPTO_EXTENSIONS__            1

/*
 * Feature toggles that apple_arm64_common.h would set but which we must NOT
 * (or cannot) use on MT6785:
 *
 *  APPLE_ARM64_ARCH_FAMILY      -- absent (see header comment).
 *  HAS_IPI                      -- Apple fast-IPI registers don't exist.
 *  HAS_NEX_PG / HAS_BP_RET      -- Apple pCore power-gating/bp-retention.
 *  HAS_CONTINUOUS_HWCLOCK       -- provided by the PMGR always-on domain.
 *  HAS_CTRR / HAS_KTRR          -- none of the Apple code integrity units.
 *  HAS_ACNTVCT / __ARM_ARCH_8_6__ -- non-speculative counter reads absent.
 *  CPU_HAS_APPLE_PAC            -- no PAC (see top of file).
 *  __ARM_AMP__                  -- we run as one coherent domain; avoid the
 *                                  per-cluster IPI-flavored snoop paths.
 *  HAS_APPLE_GENERIC_TIMER      -- MTK counter is architectural 13 MHz.
 */

/*
 * xnu's "monitor" is the Apple EL3 firmware contract (PSCI-style SMC
 * power gating with an AOP/PMGR daemon behind it).  MT6785's EL3 is the
 * MTK BL31; xnu must not register a monitor callback/entry point with
 * it, so we set NO_MONITOR exactly like the H13..H16 and VMAPPLE headers
 * do.  The only EL3 traffic MT6785 xnu produces is the PSCI smc #0 calls
 * implemented in osfmk/arm64/mt6785/ (CPU_ON / CPU_OFF / SYSTEM_OFF /
 * SYSTEM_RESET), which need no monitoring contract.
 */
#define NO_MONITOR                              1

/*
 * The Apple CPU_OVRD / ACC_OVRD / CYC_OVRD system registers (which gate IRQ
 * masking and "ok to power down" deep-sleep signaling on Apple clusters) do
 * not exist on Cortex-A55/A76.  start.s common_start and the WFI paths test
 * `#if HAS_CLUSTER && !NO_CPU_OVRD`; we DO have two hardware clusters, so
 * we must set NO_CPU_OVRD to keep those MSR/MRS pairs out of the boot
 * vector.  With NO_CPU_OVRD the Apple-specific urrown handles IRQ masking
 * by DAIF (the architectural mask) instead, which GICv3 priorities handle
 * correctly (see kexts/MT6785GICv3/README.md, "why no CPU_OVRD").
 */
#define NO_CPU_OVRD                             1

#define HAS_CLUSTER                             1 /* Two CPU clusters (see board_config.h) */
/* HAS_RETENTION_STATE intentionally NOT defined (PSCI CPU_ON does not
 * preserve architectural state; WFI is standard on Cortex). */
#define HAS_EL2                                 1 /* EL2 exists, although xnuloader enters at EL1 */
#define HAS_UCNORMAL_MEM                        1 /* generic uncached-device normality is safe */

/*
 * Interrupt controller: an ARM GICv3 WITHOUT the trustzone re-support ASM
 * (VMAPPLE style).  The GIC register block lives at SoC-specific physical
 * addresses (see MT6785_GIC feature below); the sed header defines offsets.
 */
#define HAS_GIC_V3                              1

#define GIC_SPURIOUS_IRQ                        1023 /* vIRQ no for GIC spurious interrupt */

/*
 * GICv3 register geometry (from the hardware map doc; MUST match it):
 *   GICD @ 0x0c000000, region size 256 KiB (0x40000)
 *   GICR start @ 0x0c040000, per-PE stride 0x20000, 8 PEs -> 0xc0000 size
 * These come in via the ADT "/arm-io/gic" reg property normally; keep
 * fallback constants so the bare-metal bring-up (no ADT) also works.
 */
#define MT6785_GICD_BASE                        0x0c000000ull
#define MT6785_GICD_SIZE                        0x40000ull
#define MT6785_GICR_BASE                        0x0c040000ull
/*
 * 8 PEs x GICR_PE_SIZE.  NOTE: the hardware map doc's ADT template carries
 * `size=0xC0000` (6 redistributors) which does NOT match the 8 documented
 * CPUs; the kernel-side constant below is authoritative for 8 * 0x20000.
 * The ADT template must be fixed when bring-up lands (open item, see the
 * port report).
 */
#define MT6785_GICR_SIZE                        0x100000ull

#define GICR_PE_SIZE                            0x20000 /* Size of each redistributor region */

/* GICv3 SGI/PPI block register offsets (relative to GICD/GICR PE base). */
#define GICD_CTLR                               0x0
#define GICD_CTLR_ENABLEGRP0                    (1u << 0)
#define GICD_CTLR_ENABLEGRP1                    (1u << 1)
#define GICD_TYPER                              0x4
#define GICD_IIDR                               0xfe8
#define GICD_SGIR                               0xf00

#define GICR_TYPER                              0x08
#define GICR_WAKER                              0x14
#define GICR_IGROUPR0                           0x10080
#define GICR_ISENABLER0                         0x10100
#define GICR_ICENABLER0                         0x10180

#define GICR_TYPER_AFFINITY_VALUE_SHIFT         32
#define GICR_TYPER_LAST                         0x10

#define GICR_WAKER_PROCESSORSLEEP               (1u << 1)
#define GICR_WAKER_CHILDRENASLEEP               (1u << 2)

/* CPU interface (system) registers -- routed through ICC_* EL1 registers. */
#define ICC_CTLR_EOIMODE                        (1u << 1)
#define ICC_SRE_SRE                             (1u << 0)
#define ICC_SRE_DFB                             (1u << 1)
#define ICC_SRE_DIB                             (1u << 2)
/* End of GICv3 register definitions */

/*
 * Timer PPI assignments on the standard GICv3 (XNU uses the EL1 VIRTUAL
 * timer as its decrementer, exactly as configured by ml_set_decrementer()
 * writing CNTV_TVAL_EL0).  The interrupt that the virtual timer raises after
 * CNTHCTL amounts is PPI INTID 29 on MT6785 hardware.
 */
#define MT6785_TTIMER_INT                       27 /* per-CPU EL1 CNTP (kept for reference) */
#define MT6785_VTIMER_INT                       29 /* EL1 CNTV -- IT157, handled via GICv3 (group 1) */
/* SPRIOUS IRQ 1023 */
#define MT6785_IPI_SGI_BASE                     0  /* SGIs INTID 0..7 on the GICv3 */

/*
 * ---------------------------------------------------------------------------
 * PSCI function IDs (invoked with `smc #0` into the MediaTek BL31)
 * ---------------------------------------------------------------------------
 */
#define MT6785_PSCI_VERSION_FUNC                0x84000000ull
#define MT6785_PSCI_VERSION_MAJOR(x)            (((x) >> 16) & 0x7FFF)
#define MT6785_PSCI_VERSION_MINOR(x)            ((x) & 0xFFFF)
#define MT6785_PSCI_CPU_ON_FUNC                 0xC4000003ull
#define MT6785_PSCI_CPU_OFF_FUNC                0x84000002ull
#define MT6785_PSCI_SYSTEM_RESET_FUNC           0x84000009ull
#define MT6785_PSCI_SYSTEM_OFF_FUNC             0x84000008ull
#define MT6785_PSCI_SMC_SUCCESS                 0
#define MT6785_PSCI_SMC_NOT_SUPPORTED           (-1)
#define MT6785_PSCI_SMC_INVALID_PARAMS          (-2)
#define MT6785_PSCI_SMC_DENIED                  (-3)
#define MT6785_PSCI_SMC_ALREADY_ON              (-4)
#define MT6785_PSCI_SMC_ON_PENDING              (-5)
#define MT6785_PSCI_SMC_DISABLED                (-8)

/*
 * ---------------------------------------------------------------------------
 * MT6785 dram geometry (hardware map doc section "DRAM map").  These live
 * here rather than board_config.h because the loader-produced ADT is
 * authoritative, so both the arm_vm_init MT6785 glue and the loader DT
 * templates need exactly the same constants or the hole placement drifts.
 * ---------------------------------------------------------------------------
 */
#define MT6785_DRAM_BASE                        0x40000000ull
#define MT6785_DRAM_SIZE_DEFAULT                0xE0000000ull /* 3.5GiB -- 4GiB modules; the ADT wins if present */

/* ATF/SCP/SSPM/TEE carveout: XNU must NEVER map / hand these frames out. */
#define MT6785_CARVEOUT_BASE                    0x47c00000ull
#define MT6785_CARVEOUT_SIZE                    0x400000ull  /* 4 MiB to 0x48000000 */

/* ramoops / panic ring (written by xnuloader, survives warm watchdog
 * resets, 64 KiB).  Lives inside the carveout's lower neighbor region. */
#define MT6785_RAMOOPS_BASE                     0x47df0000ull
#define MT6785_RAMOOPS_SIZE                     0x10000ull   /* 64 KiB */

/* lk (MediaTek bootloader) framebuffer: RGBX8888, 1080x2340, 4320 stride. */
#define MT6785_FB_BASE                          0x7f000000ull /* near end of DRAM; check ADT /chosen/framebuffer */
#define MT6785_FB_SIZE                          0x9a5c00ull  /* 2340 * 4320 */
#define MT6785_FB_WIDTH                         1080
#define MT6785_FB_HEIGHT                        2340
#define MT6785_FB_ROW_BYTES                     4320

/*
 * ---------------------------------------------------------------------------
 * UART: UART0 @ 0x11002000, a 16550a-style block with a *4-byte* stride.
 * We disable the in-tree apple "DockChannel"/AppleUART paths and use the
 * 16550 driver; the low-level early console comes up through the loader.
 * ---------------------------------------------------------------------------
 */
#define MT6785_UART0_BASE                       0x11002000ull
#define MT6785_UART0_SIZE                       0x1000ull
#define MT6785_UART0_SPI                        65 /* uart interrupt (Fallback; verify on device) */
#define MT6785_UART_CLOCK                       26000000 /* 26 MHz UART clock, standard on MTK */

/*
 * Init/deinit silhouettes similar to the PRELET check the Apple headers do.
 */
#if !CONFIG_SPTM
/*
 * At EL1 we run below the (unused) EL2; VHE is never enabled on MT6785 (the
 * loader enters the kernel at EL1 with VHE bits neutralized, and nothing in
 * this config sets HCR_EL2).  Nothing to do here for vhe_disable.h; kept as
 * a comment for cross-referencing.
 */
#endif /* !CONFIG_SPTM */

#ifndef ASSEMBLER
/*
 * Watchdog: MTK has a standard 6517-style wdt @ 0x10007000 (see the kext).
 * XNU keeps the console/panic paths generic; the hardware safety net lives
 * in kexts/WatchDogStub.
 */
#define MT6785_WDT_BASE                         0x10007000ull
#define MT6785_WDT_SIZE                         0x1000ull
#endif /* !ASSEMBLER */

#endif /* !_PEXPERT_ARM64_MT6785_H */
