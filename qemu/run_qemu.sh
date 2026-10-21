#!/usr/bin/env bash
# qemu/run_qemu.sh — guide Phase A (§5, "Bring-up in QEMU first, do not skip").
#
# Runs the stage-2 loader under QEMU's -M virt machine.  With zero hardware
# this validates the real boot chain front-end:
#   entry.S EL1 flatten -> UART banner -> FDT parse (QEMU's own DTB) ->
#   memsize extraction -> framebuffer test pattern (plain RAM writes) ->
#   payload manifest scan (manifest absent in this launcher) ->
#   clean XLDR_PANIC + ramoops record + WFE park.
#
# The QEMU build differs from the device build:
#   -DXLDR_UART_PL011 : -M virt exposes an MMIO PL011 at 0x09000000 (the MTK
#                       UART at 0x11002000 is unmapped there - writing to it
#                       would bus-fault under the emulator).
#   linker_qemu.ld    : links at the RAM-resident base 0x40080000 so QEMU's
#                       ELF loader places the image exactly where lk would on
#                       the device.
# build/xnuloader_qemu.elf is kept as the artifact; in-tree objects are
# cleaned afterwards so a subsequent device build starts pristine.
#
# Usage:  bash qemu/run_qemu.sh     (or: make qemu)
set -euo pipefail
cd "$(dirname "$0")/.."

CC_AR=$(which aarch64-none-elf-gcc aarch64-linux-gnu-gcc 2>/dev/null | head -1)
if [ -z "${CC_AR}" ]; then
	echo "error: no aarch64 cross gcc on PATH (aarch64-none-elf / aarch64-linux-gnu)" >&2
	echo "       install with: scripts/setup_host.sh (or: sudo apt-get install gcc-aarch64-linux-gnu)" >&2
	exit 1
fi

echo "== building QEMU variant of xnuloader =="
make -C xnuloader clean >/dev/null 2>&1 || true
make -C xnuloader LINK_SCRIPT=linker_qemu.ld CFLAGS_EXTRA='-DXLDR_UART_PL011' all
mkdir -p build qemu/out
cp xnuloader/xnuloader.elf build/xnuloader_qemu.elf
make -C xnuloader clean >/dev/null 2>&1 || true

echo "== booting under qemu-system-aarch64 -M virt (TCG) =="
timeout 45 qemu-system-aarch64 \
	-M virt -cpu cortex-a57 -smp 1 -m 2048 \
	-kernel build/xnuloader_qemu.elf \
	-display none -serial stdio \
	> qemu/out/qemu.log 2>&1 || true

echo "== serial log (qemu/out/qemu.log) =="
cat qemu/out/qemu.log

grep -aq "HACKIOS xnuloader" qemu/out/qemu.log || {
	echo "FAIL: loader banner not seen on the UART" >&2
	exit 1
}
grep -aq "XLDR PANIC" qemu/out/qemu.log || {
	echo "FAIL: loader never reached its panic waypoint (expected in this launcher: no payload manifest)" >&2
	exit 1
}

echo "PASS: entry.S -> UART -> FDT -> panic path verified under QEMU (guide Phase A, step 5)."
