#!/usr/bin/env bash
#
# mkramdisk.sh — HackiOS-begonia userland :: M1..M4 secondary-boot ramdisk.
#
# Builds a small filesystem image (HFS+ by default) meant to be embedded in
# boot.img by mkxnuimg.py (root-repo scope) and referenced by the ADT as
# /chosen/memory-map `RAMDisk` (→ XNU mdevadd → /dev/md0 → rd=md0).
# See ./README.md for the whole contract and ./static_shell/ for the payload.
#
# Requirements (checked, not assumed):
#   * mkfs.hfsplus  (Debian/Ubuntu: apt install hfsprogs)  — or hdiutil on macOS
#   * optional: mkfs.vfat (dosfstools) if you --format vfat
#
# Options:
#   --out FILE       output image path (default: ./ramdisk_m4.img)
#   --size-mb N      image size in MiB (default: 64)
#   --format FMT     hfsplus (default) | vfat
#   --shell BIN      path to a prebuilt static shell; if absent, we print the
#                    ../static_shell/build.sh instructions instead of failing
#   --no-shell       skip shell embedding entirely (bare image)
#   --dry-run        print the plan, touch nothing
#
# Exit codes: 0 ok · 1 usage · 2 missing tool · 3 populate failed · 4 other.

set -euo pipefail

PROG="$(basename "${BASH_SOURCE[0]}")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

banner() {
  cat <<'EOF'
===============================================================================
 ${PROG} — HackiOS-begonia M4 test ramdisk builder
 target     : Xiaomi Redmi Note 8 Pro (begonia) · MT6785 (Helio G90T)
 boot       : XNU via xnuloader (boot.img payload, rd=md0), PID1 = /sbin/launchd
 image ref  : ADT /chosen/memory-map key RAMDisk (reg = <phys,size>)
 fs default : HFS+ (non-journaled; mounts rw under Linux loop + hfsplus module)
===============================================================================
EOF
}

log()  { printf '[ramdisk] %s\n' "$*"; }
warn() { printf '[ramdisk] WARNING: %s\n' "$*" >&2; }
die()  { printf '[ramdisk] ERROR: %s\n' "$*" >&2; exit "${2:-4}"; }
usage() { sed -n '10,46p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

# ---------------------------------------------------------------------------
# args
# ---------------------------------------------------------------------------
OUT="ramdisk_m4.img"
SIZE_MB=64
FORMAT="hfsplus"
SHELL_BIN=""
NO_SHELL=0
DRY_RUN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out) OUT="${2:?--out needs a value}"; shift ;;
    --out=*) OUT="${1#*=}" ;;
    --size-mb) SIZE_MB="${2:?--size-mb needs a value}"; shift ;;
    --size-mb=*) SIZE_MB="${1#*=}" ;;
    --format) FORMAT="${2:?--format needs a value}"; shift ;;
    --format=*) FORMAT="${1#*=}" ;;
    --shell) SHELL_BIN="${2:?--shell needs a value}"; shift ;;
    --no-shell) NO_SHELL=1 ;;
    --dry-run) DRY_RUN=1 ;;
    -h|--help|help) usage ;;
    *) die "unknown option: $1 (see --help)" 1 ;;
  esac
  shift
done

banner
[[ "$FORMAT" == "hfsplus" || "$FORMAT" == "vfat" ]] || die "bad --format '$FORMAT'" 1
case "$SIZE_MB" in (*[!0-9]*|"") die "--size-mb must be a positive integer" 1 ;; esac

# ---------------------------------------------------------------------------
# tool discovery (macOS path and Linux path; no silent assumptions)
# ---------------------------------------------------------------------------
MKFS=""
COPY_MODE="manual"
if [[ "$FORMAT" == "hfsplus" ]]; then
  if command -v mkfs.hfsplus >/dev/null 2>&1; then
    MKFS="mkfs.hfsplus"
  elif command -v hdiutil >/dev/null 2>&1; then
    MKFS="hdiutil"   # macOS
  else
    die "neither mkfs.hfsplus (Ubuntu: apt install hfsprogs) nor hdiutil (macOS) found." 2
  fi
else
  command -v mkfs.vfat >/dev/null 2>&1 || die "mkfs.vfat (dosfstools) missing for --format vfat" 2
fi
if command -v hdiutil >/dev/null 2>&1; then
  COPY_MODE="hdiutil"
elif [[ "$(id -u)" == "0" ]]; then
  COPY_MODE="root-mount"
fi

log "out=$OUT size=${SIZE_MB}MiB format=$FORMAT populate=$COPY_MODE"

if (( DRY_RUN )); then
  log "(dry-run) plan:"
  log "  1) create image file (${SIZE_MB} MiB)"
  log "  2) $MKFS $FORMAT it (non-journaled)"
  log "  3) populate /sbin/launchd <- static shell (+ /bin/bash symlink), /etc/passwd"
  log "  4) print xnuloader/mkxnuimg wiring reminders"
  exit 0
fi

# ---------------------------------------------------------------------------
# 1) create + 2) format
# ---------------------------------------------------------------------------
if [[ -e "$OUT" ]]; then
  warn "removing previous image: $OUT"
  rm -f -- "$OUT"
fi

if [[ "$FORMAT" == "hfsplus" ]]; then
  if [[ "$MKFS" == "mkfs.hfsplus" ]]; then
    # hfsprogs: SPECIAL-DEVICE is the image file itself; size comes from the file.
    # Non-journaled default keeps Linux loop-mount rw possible.
    log "creating ${SIZE_MB}MiB sparse file + mkfs.hfsplus"
    truncate -s "$((SIZE_MB * 1024 * 1024))" -- "$OUT"
    mkfs.hfsplus -v HACKIOSRD -b 4096 "$OUT" >/dev/null # prints its own noise; keep rc
  else
    log "creating HFS+ image via hdiutil (macOS)"
    hdiutil create -size "${SIZE_MB}m" -fs HFS+ -volname HACKIOSRD -format UDRW "$OUT" >/dev/null
  fi
else
  log "creating FAT32 image via mkfs.vfat"
  mkfs.vfat -F 32 -n HACKIOSRD -C "$OUT" "$((SIZE_MB * 1024))"
fi

# ---------------------------------------------------------------------------
# 3) populate
# ---------------------------------------------------------------------------
MNT="/tmp/hackios-ramdisk-mnt"
SHELL_EMBEDDED=0

if (( NO_SHELL )); then
  log "--no-shell: leaving the image empty"
else
  if [[ -z "$SHELL_BIN" ]]; then
    warn "no --shell provided."
    cat <<SHELPEOF
  Build the static shell first, then re-run with --shell:

      ${HERE}/static_shell/build.sh /path/to/cross-clang
      $0 --out $OUT --shell ${HERE}/static_shell/shell

  (see ${HERE}/static_shell/README.md for toolchain options: Apple Darwin
   cctools toolchain, plain aarch64-none-elf clang, or llvm --target=aarch64.)
SHELPEOF
  elif [[ ! -f "$SHELL_BIN" ]]; then
    die "--shell file not found: $SHELL_BIN" 1
  else
    # verify arm64 Mach-O before embedding (MH_MAGIC_64 0xfeedfacf, cputype 0x0100000C)
    MAGIC="$(head -c 4 "$SHELL_BIN" | od -An -tx1 | tr -d ' \n')"
    [[ "$MAGIC" == "cffaedfe" ]] || die "shell is not a Mach-O (LE magic cffaedfe); got ${MAGIC}" 3
    CPUTYPE="$(od -An -tx4 -j 4 -N 4 "$SHELL_BIN" | tr -d ' \n')"
    [[ "$CPUTYPE" == "0c000001" ]] || die "shell cputype is not ARM64 (0x0100000c LE bytes 0c000001); got ${CPUTYPE}" 3
    log "shell verified: Mach-O 64,_arm64 ($(du -h "$SHELL_BIN" | cut -f1))"
    SHELL_EMBEDDED=1
  fi
fi

POPULATE_NOTES=()
case "$COPY_MODE" in
  hdiutil)
    if (( SHELL_EMBEDDED )); then
      hdiutil attach -mountpoint "$MNT" -nobrowse "$OUT" >/dev/null
      cp "$SHELL_BIN" "$MNT/sbin_launchd_placeholder"
      mkdir -p "$MNT/bin" "$MNT/etc" "$MNT/dev"
      mv "$MNT/sbin_launchd_placeholder" "$MNT/sbin/launchd" 2>/dev/null || true
      ln -sf /sbin/launchd "$MNT/bin/bash"
      printf 'root::0:0:Operator:/:/bin/bash\n' > "$MNT/etc/passwd"
      hdiutil detach "$MNT" >/dev/null
      log "populated via hdiutil (rise macOS)"
    else
      POPULATE_NOTES+=("shell not given — image left empty")
    fi
    ;;
  root-mount)
    if (( SHELL_EMBEDDED )); then
      mkdir -p "$MNT"
      FS_FLAG="hfsplus"; [[ "$FORMAT" == "vfat" ]] && FS_FLAG="vfat"
      mount -t "$FS_FLAG" -o loop "$OUT" "$MNT"
      mkdir -p "$MNT/sbin" "$MNT/bin" "$MNT/etc" "$MNT/dev"
      cp "$SHELL_BIN" "$MNT/sbin/launchd"
      chmod 755 "$MNT/sbin/launchd"
      ln -sf /sbin/launchd "$MNT/bin/bash"
      printf 'root::0:0:Operator:/:/sbin/launchd\n' > "$MNT/etc/passwd"
      umount "$MNT"
      log "populated via loop-mount"
    else
      POPULATE_NOTES+=("shell not given — image left empty")
    fi
    ;;
  manual)
    POPULATE_NOTES+=(
      "no privileges for loop-mount and no hdiutil: populate by hand (as root):"
      "  mount -t ${FORMAT} -o loop $OUT $MNT"
      "  <copy shell to \$MNT/sbin/launchd; symlink \$MNT/bin/bash -> /sbin/launchd>"
      "  write \$MNT/etc/passwd; umount $MNT"
    )
    ;;
esac
for n in "${POPULATE_NOTES[@]+"${POPULATE_NOTES[@]}"}"; do log "$n"; done

# ---------------------------------------------------------------------------
# 4) wiring reminders (cross-repo contract; nothing else to do here)
# ---------------------------------------------------------------------------
cat <<'WIRE'
  bootloader-side wiring (other owners; reminders only):
    * xnuloader: place this image in DRAM, record (/chosen/memory-map).RAMDisk reg=<phys,size>
    * mkxnuimg.py: pass the image bytes unmodified inside the boot.img payload
    * boot args: rd=md0 rootdev=md0 (CANONICAL list in docs/hardware_map_mt6785.md §4)
    * PID1: /sbin/launchd — statically linked shell masquerading as launchd
      (XNU panic "Process 1 exec of /sbin/launchd failed" if it is absent!)
WIRE
log "done: $OUT ($(du -h "$OUT" | cut -f1))"
exit 0
