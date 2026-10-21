/*
 * Copyright (c) 2026 HackiOS begonia port project.  Portions Copyright
 * 2003-2020 Apple Inc. All rights reserved (structure mirrors the private
 * osfmk/arm64/tunables/tunables.s from the Apple internal tree).
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
 * ============================================================================
 * ARM64 boot-time tunables -- assembly side.
 *
 * start.s invokes:
 *
 *	APPLY_TUNABLES x12, x13, x14
 *
 * after reading MIDR_EL1 into x12.  In the Apple internal tree this macro
 * walks a table of (MIDR-match, S3_x register, field, value) records --
 * the Apple "PIO/power/perf tuning" knobs that iBoot also knows about and
 * that differ per SoC (HID fields on t8101/t8112/H13.., PMGR freeze
 * windows, ...).
 *
 * This tree does not ship a per-SoC tunables table: the whole directory
 * was stripped from the upstream import.  For HackiOS builds we do not
 * need it:
 *
 *  - The Apple HID/PIO registers that the internal table configures are
 *    UNIMPLEMENTED on MT6785's Cortex-A55/A76 cores; writing them from
 *    boot would trap in EL1 with ESR EC=0x18..0x1F (undefined first class).
 *  - Cortex cores are tuned at power-up by their own TrmA wire
 *    station (ACTLA/ACTLR defaults are sane), and MediaTek's preloader (lk)
 *    applies the SoC's (binary) intuition of the correct LDO/PLL settings.
 *
 * So the macro is a whole-tree no-op.  It must still EXIST for gas so that
 * every apple arm64 configuration (including T8101/VMAPPLE dev builds of
 * this source drop that didn't need it before) keeps assembling.
 *
 * If a future MT6785 bring-up needs real per-SoC register tuning, define
 * MT6785_TUNABLES in pexpert/pexpert/arm64/mt6785.h and implement the
 * records in mt6785_tunables.s (see xnu-patches/0005 for the design note).
 * ============================================================================
 */

/*
 * Assembler sanity helpers; mirrors the internal signature exactly.
 *   $0 - MIDR_EL1 value
 *   $1 - Scratch GPR
 *   $2 - Scratch GPR
 */
.macro APPLY_TUNABLES midr_el1, scratch0, scratch1
.endmacro

/*
 * Boot-time switch to enable pCore tuning for the Apple SoCs that care;
 * set to 0 unconditionally here (the Apple files must be modified for
 * MT6785 not to reference these tunable symbols).
 */
#define NO_TUNABLES				1
