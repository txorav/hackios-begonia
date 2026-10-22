#!/usr/bin/env bash
# qemu/run_ios.sh — run real Mach-O XNU kernel under QEMU -M virt
set -euo pipefail
cd "$(dirname "$0")/.."

[ -d "/media/dev/work/tcbin" ] && export PATH="/media/dev/work/tcbin:${PATH}"

echo "== [1/3] Building QEMU variant of xnuloader =="
make -C xnuloader clean >/dev/null 2>&1 || true
make -C xnuloader LINK_SCRIPT=linker_qemu.ld CFLAGS_EXTRA='-DXLDR_UART_PL011' all
mkdir -p build out
cp xnuloader/xnuloader.bin build/xnuloader_qemu.bin
cp xnuloader/xnuloader.elf build/xnuloader_qemu.elf
make -C xnuloader clean >/dev/null 2>&1 || true

K="prebuilt/mach.development.bcm2837"
[ -f "$K" ] || { echo "error: $K missing" >&2; exit 1; }

RD_ARG=""
if [ -f out/root.apfs ]; then
    RD_ARG="--ramdisk out/root.apfs"
fi

echo "== [2/3] Packaging payload with real XNU kernel =="
python3 mkxnuimg.py \
    --out build/qemu-ios.img \
    --loader build/xnuloader_qemu.bin \
    --kernel "$K" \
    $RD_ARG \
    --cmdline "serial=3 -v debug=0x8e keepsyms=1 rd=md0 cs_enforcement_disable=1 amfi_get_out_of_my_way=0x1" \
    --fb 1080x2340x4320x32 \
    --fb-base 0x7e605000

echo "== [3/3] Booting XNU under QEMU -M virt (press ^C to stop) =="
TIMEOUT="${1:-0}"
if [ "$TIMEOUT" -gt 0 ]; then
    exec timeout "$TIMEOUT" qemu-system-aarch64 \
        -M virt -cpu cortex-a57 -smp 1 -m 2048 \
        -nic none \
        -kernel build/boot-payload.img \
        -display none -serial stdio
else
    exec qemu-system-aarch64 \
        -M virt -cpu cortex-a57 -smp 1 -m 2048 \
        -nic none \
        -kernel build/boot-payload.img \
        -display none -serial stdio
fi
