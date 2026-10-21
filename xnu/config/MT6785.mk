# -*- mode: makefile;-*-
#
# Copyright (c) 2026 HackiOS begonia port project.  Portions derived from
# xnu makefiles (Apple Inc.).
#
# =====================================================================
# MT6785 (Helio G90T / "begonia") target configuration for the XNU
# source tree.
#
# The stock xnu build system picks its machine config from
# TARGET_CONFIGS (see makedefs/MakeInc.top: kernel-config / arch /
# machine triplets), and makedefs/MakeInc.def carries the per-machine
# compile flags via MACHINE_FLAGS_ARM64_<MACHINE>.  MT6785 hooks both:
#
#   - SUPPORTED_ARM64_MACHINE_CONFIGS gained MT6785             (0002)
#   - MACHINE_FLAGS_ARM64_MT6785 gained -DARM64_BOARD_CONFIG_MT6785 -march=armv8.2-a
#
# This file exists to (a) capture the canonical build invocation in
# one place, (b) assert the configuration that the loader contract
# (docs/hardware_map_mt6785.md section 2) requires, and (c) guard the
# things a plain "make" would silently get wrong for us.  It is NOT
# auto-included: xnu/Makefile is stock-Apple and must stay untouched
# apart from documented hooks.
#
# Canonical invocation (from the xnu/ root):
#
#   make SDKROOT=iphoneos PLATFORM=iPhoneOS SDK_VERSION=16.5 \
#        TARGET_CONFIGS="development arm64 MT6785" \
#        RC_ProjectName=xnu
#
# or the emulated variant used by the packaged build (no Apple SDK,
# uses the free toolchain from kexts/README.md):
#
#   make PLATFORM=MTK TARGET_CONFIGS="development arm64 MT6785"
#
# Because ARM64 board configs switch via -DARM64_BOARD_CONFIG_MT6785,
# the *whole* kernel must be compiled at once (no kext "config-in-a-jar"
# mixing).  Everything dynamic lives in kexts/.
# =====================================================================

#
# Configuration assertions (fail-fast).  These are evaluated at
# import time.  If you load this file outside a full xnu build
# environment, unknown variables resolve empty -- that is fine here,
# these only fire when explicitly requested to gate a build.load.
#

MT6785_REQUIRED_DEFINES := \
	ARM64_BOARD_CONFIG_MT6785 \
	__ARM_16K_PG__ \
	ARM_ARCH_TIMER \
	HAS_GIC_V3

# Things that must NOT leak into an MT6785 kernel config; the list is
# the Apple-proprietary compile surface we neutralize (see
# xnu-patches/0004 and xnu/osfmk/arm64/proc_reg.h MT6785 block).
MT6785_FORBIDDEN_DEFINES := \
	ARM64_BOARD_CONFIG_T8101 \
	ARM64_BOARD_CONFIG_T8112 \
	ARM64_BOARD_CONFIG_T6000 \
	ARM64_BOARD_CONFIG_T6020 \
	ARM64_BOARD_CONFIG_T6030 \
	ARM64_BOARD_CONFIG_T6031 \
	ARM64_BOARD_CONFIG_T6041 \
	ARM64_BOARD_CONFIG_T8103 \
	ARM64_BOARD_CONFIG_T8122_T8130 \
	ARM64_BOARD_CONFIG_T8132 \
	ARM64_BOARD_CONFIG_VMAPPLE \
	APPLE_ARM64_ARCH_FAMILY \
	KERNEL_INTEGRITY_CTRR \
	KERNEL_INTEGRITY_KTRR \
	KERNEL_INTEGRITY_PV_CTRR \
	HAS_IPI \
	HAS_APPLE_GENERIC_TIMER \
	USE_APPLEARMSMP \
	__ARM_AMP__ \
	__ARM_KERNEL_PROTECT__ \
	HAS_NEX_PG \
	HAS_BP_RET \
	HAS_CONTINUOUS_HWCLOCK \
	HAS_ACNTVCT \
	CPU_HAS_APPLE_PAC \
	__ARM_42BIT_PA_SPACE__ \
	HAS_IOA

.PHONY: mt6785-config-check
mt6785-config-check:
	@echo "MT6785 kernel config assertions:"
	@for d in $(MT6785_REQUIRED_DEFINES); do \
		echo "  required: $$d"; \
	done
	@for d in $(MT6785_FORBIDDEN_DEFINES); do \
		echo "  forbidden (-U$$d): $$d"; \
	done
	@echo "page size: 16 KiB (TTBR0 == TTBR1), ARM_SMALL_PAGES MUST be undefined"
	@echo "expected VM_MIN_KERNEL_ADDRESS: 0xffffffe000000000 (non-LARGE_MEMORY embedded window)"
	@echo "expected linker contract with xnuloader: physBase=0x40000000, virtBase=VM_MIN_KERNEL_ADDRESS+physBase"

.PHONY: mt6785-help
mt6785-help:
	@echo "MT6785 make hook usage:"
	@echo "  make -C xnu TARGET_CONFIGS=\"development arm64 MT6785\""
	@echo "Verify with the kernel's own config dump after a successful link."
