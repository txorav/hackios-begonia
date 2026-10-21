#!/usr/bin/env bash
# begonia_dump_fdt.sh — pull FDT + /proc/iomem + kernel cmdline/host map from a
# booted Android/mainline/postmarketOS shell. Run on the human's host with adb.
set -euo pipefail
OUT="out/discovery"
mkdir -p "${OUT}"

adb wait-for-device
echo "== /sys/firmware/fdt =="
adb shell "su -c 'cp /sys/firmware/fdt /data/local/tmp/fdt && chmod 644 /data/local/tmp/fdt'" || \
adb shell "cp /sys/firmware/fdt /data/local/tmp/fdt && chmod 644 /data/local/tmp/fdt" || true
adb pull /data/local/tmp/fdt "${OUT}/fdt.img" || true

if [ -s "${OUT}/fdt.img" ]; then
  if command -v dtc >/dev/null; then
    dtc -I dtb -O dts -o "${OUT}/mt6785-xiaomi-begonia.dts" "${OUT}/fdt.img"
  else
    echo "dtc missing: install (apt install device-tree-compiler) and re-run to decompile"
  fi
fi

echo "== /proc/iomem (reserved carveouts) =="
adb shell "su -c 'cat /proc/iomem'" > "${OUT}/proc_iomem.txt" 2>/dev/null || true

echo "== /proc/cmdline =="
adb shell "cat /proc/cmdline" > "${OUT}/proc_cmdline.txt" 2>/dev/null || true

echo "== lk logs (known location on MTK 12A firmware) =="
adb shell "su -c 'ls -l /proc/pstore' " > "${OUT}/pstore.txt" 2>/dev/null || true
adb shell "su -c 'cat /dev/pro_info' " > "${OUT}/pro_info.txt" 2>/dev/null || true

echo "== framebuffer info probes =="
adb shell "su -c 'cat /sys/class/graphics/fb0/virtual_size 2>/dev/null; cat /sys/class/graphics/fb0/stride 2>/dev/null'" > "${OUT}/fb.txt" 2>/dev/null || true

cat <<'EOF'
Next steps:
  1. verify the DRAM carveouts from proc_iomem.txt and update
     docs/hardware_map_mt6785.md (Section 2).
  2. confirm the framebuffer base (0x7f000000 placeholder) using
     scripts/begonia_discovery.sh and freeze it into the same file.
  3. grep the decompiled DTS for: uart0@0x11002000, msdc1@0x11240000,
     ufs@0x11270000, gic@0x0c000000, i2c@0x11007000, and the touchscreen's
     interrupt-parent + IRQ number; update /arm-io nodes in xnuloader/adt.c.
EOF
