#!/usr/bin/env bash
# setup_host.sh — Phase-1 host dependencies (Linux, tested Ubuntu 22.04+).
# Installs the tools the bring-up needs. No device drivers; mtkclient is
# cloned separately by begonia_backup.sh.
set -euo pipefail

echo "== HackiOS begonia host setup =="

APT_PKGS=(python3 python3-pip curl git build-essential device-tree-compiler
          fastboot adb libncurses5-dev zlib1g-dev libxml2-dev libfuse-dev
          pkg-config uuid-dev libssl-dev)
if command -v apt-get >/dev/null; then
  sudo apt-get update
  sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y "${APT_PKGS[@]}" || true
fi

python3 -m pip install --user -r <(cat <<'EOF'
# pyserial for xl log, requests not used (offline-first)
pyserial>=3.5
EOF
) || true

echo
echo "== mkbootimg (python implementation baked into mkxnuimg.py — none needed) =="
echo "== cross toolchain: run tools/install_llvm_toolchain.sh next (see tools/) =="
echo "== mtkclient: git clone https://github.com/bkerler/mtkclient tools/mtkclient =="
echo "Done."
