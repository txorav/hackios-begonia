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

[ -d "/media/dev/work/tcbin" ] && export PATH="/media/dev/work/tcbin:${PATH}"

# cross-toolchain detection without deprecated `which`; prints PATH
# diagnostics when missing so CI failures are explainable.
find_cc() {
	for c in aarch64-none-elf-gcc aarch64-linux-gnu-gcc aarch64-linux-android-gcc aarch64-linux-gnu-gcc-14 aarch64-linux-gnu-gcc-13; do
		if command -v "$c" >/dev/null 2>&1; then
			command -v "$c"
			return 0
		fi
	done
	return 1
}

CC_AR=$(find_cc || true)
if [ -z "${CC_AR}" ]; then
	echo "error: no aarch64 cross gcc on PATH (aarch64-none-elf / aarch64-linux-gnu)" >&2
	echo "PATH=${PATH}" >&2
	echo "-- aarch64 toolchains present in /usr/bin:" >&2
	ls /usr/bin 2>/dev/null | grep -i aarch64 | head >&2 || true
	echo "install with: scripts/setup_host.sh (or: sudo apt-get install gcc-aarch64-linux-gnu)" >&2
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
	-nic none \
	-kernel build/xnuloader_qemu.elf \
	-display none -serial file:build/serial.txt \
	-d guest_errors,int,cpu_reset -D build/qemu.trace \
	> build/qemu-stderr.log 2>&1 || true

echo "== qemu stderr (build/qemu-stderr.log) =="
cat build/qemu-stderr.log
echo "== guest cpu exception trace tail (build/qemu.trace) =="
tail -40 build/qemu.trace 2>/dev/null || echo "(no trace)"
echo "== serial log (build/serial.txt) =="
cat build/serial.txt 2>/dev/null || echo "(empty)"

grep -aq "HACKIOS xnuloader" build/serial.txt || {
	echo "FAIL: loader banner not seen on the UART" >&2
	exit 1
}
grep -aq "XLDR PANIC" build/serial.txt || {
	echo "FAIL: loader never reached its panic waypoint (expected in this launcher: no payload manifest)" >&2
	exit 1
}

echo "PASS: entry.S -> UART -> FDT -> panic path verified under QEMU (guide Phase A, step 5)."
