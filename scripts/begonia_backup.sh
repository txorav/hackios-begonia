#!/usr/bin/env bash
# begonia_backup.sh — Phase 1 critical-partition backup via mtkclient (BROM)
# + optional bootlocker-unlock assist notes and TWRP install step.
#
# Usage:
#   scripts/begonia_backup.sh out/backup              # BROM full backup
#   scripts/begonia_backup.sh --twrp out/twrp.img     # flash recovery (needs unlocked BL)
#   scripts/begonia_backup.sh --list                  # just list critical partitions
set -euo pipefail

MTK="tools/mtkclient/mtk"
BACKUP_DIR="out/backup/mt6785"
CRITICAL="nvram,nvdata,persist,seccfg,proinfo,preloader,pgpt,sgpt,lk,lk1stboot,nvcfg"

# partition names can differ per firmware; verify with `mtk pgpt`
FLAG_TWRP=0
FLAG_LIST=0
while getopts "hlt" o; do
  case "${o}" in
    t) FLAG_TWRP=1 ;;
    l) FLAG_LIST=1 ;;
    h) echo "usage: $0 [-t twrp.img] [--list] [backup_dir]"; exit 0 ;;
    *) exit 1 ;;
  esac
done
shift $((OPTIND-1))
[ "${1:-}" = "--list" ] && FLAG_LIST=1
[ $# -gt 0 ] && BACKUP_DIR="$1"

if [ "$FLAG_LIST" = "1" ]; then
  echo "critical partitions backed up under: $BACKUP_DIR"
  echo "  nvram nvdata persist seccfg proinfo preloader pgpt sgpt lk lk1stboot nvcfg boot vbmeta recovery"
  exit 0
fi

if [ ! -x "$(command -v python3)" ]; then echo "need python3" >&2; exit 1; fi
if [ ! -f "${MTK}" ]; then
  echo "mtkclient not found. clone it once (needs network, do it on your host):"
  echo "  git clone https://github.com/bkerler/mtkclient tools/mtkclient"
  echo "  pip3 install -r tools/mtkclient/requirements.txt"
  exit 3
fi

mkdir -p "${BACKUP_DIR}"
echo "== Backup ledger =="
{
  date -u +"# UTC %Y-%m-%d %H:%M:%S  device: begonia (mt6785)"
} | tee "${BACKUP_DIR}/LEDGER.txt"

echo "== Step 1: critical partitions =="
${MTK} r "${CRITICAL},${BACKUP_DIR}" 2>&1 | tee "${BACKUP_DIR}/critical.log"

echo "== Step 2: boot + vbmeta + recovery (snapshot, not required but cheap) =="
${MTK} r "boot,vbmeta,recovery,dtbo,${BACKUP_DIR}" 2>&1 | tee "${BACKUP_DIR}/boot.log" || true

echo "== Step 3: full-dump (optional, ~40+ GiB) =="
read -r -p "Full flash dump too? [y/N] " ans
if [ "${ans}" = "y" ]; then
  ${MTK} rf "${BACKUP_DIR}/full.img" 2>&1 | tee "${BACKUP_DIR}/full.log"
fi

echo "== Step 4: verification =="
sha256sum "${BACKUP_DIR}"/* "${@}" 2>/dev/null | tee -a "${BACKUP_DIR}/LEDGER.txt" || true

if [ "$FLAG_TWRP" = "1" ]; then
  TWRP="${1:-}"
  [ -z "${TWRP}" ] && { echo "TWRP image path missing (use --twrp <img> with the image as argument)"; exit 1; }
  cat <<'EOF'
Flash TWRP only with the bootloader unlocked. Steps on your host:
    fastboot flashing unlock     # on device confirm
    fastboot flash recovery <twrp.img>
    fastboot reboot recovery
Keep the stock recovery in backup dir if you need OTA downgrades.
EOF
fi
echo "Done. Keep ${BACKUP_DIR} on two separate machines."
