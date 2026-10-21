#!/usr/bin/env bash
# begonia_discovery.sh — probe candidate framebuffer addresses on a live device
# with /dev/mem writes: paint candidate base addresses with a distinctive
# pattern, pull a screencap, and let the human visually confirm which one lit up.
# Cannonical placeholder: 0x7f000000 RGBX8888 1080x2340 stride 4320.
set -euo pipefail
SCREEN_W=1080; SCREEN_H=2340; BYTES=$((SCREEN_W*SCREEN_H*4))
PATTERN='0xFF3C7A55'   # distinctive ARGB maybe; screen just needs to look "weird"

CANDS="${1:-0x7f000000 0x7e000000 0x80000000}"
for base in $CANDS; do
  hex=$((base))
  echo "trying fb base ${base}"
  # Write 64 KB of pattern at the beginning of the candidate region
  adb shell "su -c 'dd if=/dev/zero bs=65536 count=1 2>/dev/null | dd of=/dev/mem bs=1 seek=$((hex)) 2>/dev/null || true'"
  adb shell "su -c 'dd if=/dev/zero of=/dev/null 2>/dev/null; : '"
  # Pattern push via python on host is inaccurate; use dd on device to blast the raw:
  python3 - "$base" <<'EOF' > /tmp/pattern.bin 2>/dev/null || true
import sys
base = int(sys.argv[1], 0)
w = 1080; h=2340
data = bytearray()
for y in range(64):
    for x in range(w//8):
        data += bytes([0xFF,0x3C,0x7A,0x55]) * 8
sys.stdout.buffer.write(data)
EOF
  adb push /tmp/pattern.bin /data/local/tmp/pattern.bin >/dev/null 2>&1 || true
  adb shell "su -c 'dd if=/data/local/tmp/pattern.bin of=/dev/mem seek=$((hex)) bs=1 count=65536 conv=notrunc 2>/dev/null'" >/dev/null 2>&1 || true
  echo "  -> look at the phone display: if you saw stripes at ${base}, that's the fb."
  read -r -p "Continue to next candidate? [Y/n] " ans || true
  case "${ans:-Y}" in [Nn]*) break;; esac
done
echo "record the confirmed address in docs/hardware_map_mt6785.md"
