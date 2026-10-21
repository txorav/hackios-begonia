#!/usr/bin/env bash
#
# adb/snippets/create_patches.sh — device-side patch-diff helper (run over adb shell).
#
# WHAT (from tools/adb/snippets.md): before touching anything for the
# userland bring-up phase, snapshot the files we plan to patch (per
# userland/apply_rootfs_patches.sh) into /sdcard/hackios/patches/:
#   * FIRST run: copies each stock original to <name>.baseline
#   * LATER runs: emits <name>.patch, a unified diff of the live file
#     against that baseline, so other agents can reproduce the *same*
#     edits on a fresh 16.5 system volume
#
# This script is push-safe: it only READS the target files. The writes it
# performs are its own snapshot/diff outputs under /sdcard/hackios/.
#
# TARGETS (stock iOS 16.5, iPhone10,3/D22AP — cross-checked against
# userland/patches/fstab and the porting plan):
#   /private/var/fstab                                        (mount table)
#   /private/var/save/restore/com.apple.MobileGestalt.plist   (MG cold cache)
#
# Requirements on-device: adb shell with *root* (magisk'd or jailbroken
# test device) and prefer busybox / toybox with `diff`/`mkdir`; run:
#     adb push create_patches.sh /sdcard/hackios/
#     adb shell /sdcard/hackios/create_patches.sh -h
#
# Exit codes: 0 ok · 1 usage · 3 a snapshot failed (partial snapshots still ok).

set -euo pipefail

PROG="$(basename "${BASH_SOURCE[0]}")"
OUT="/sdcard/hackios/patches"

snapshot_targets=(
  "/private/var/fstab"
  "/private/var/save/restore/com.apple.MobileGestalt.plist"
)

log()  { printf '[create_patches] %s\n' "$*"; }
warn() { printf '[create_patches] WARNING: %s\n' "$*" >&2; }
die()  { printf '[create_patches] ERROR: %s\n' "$1"; exit "${2:-1}"; }
usage(){ sed -n '3,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

while getopts ":ho:" opt; do
  case "$opt" in
    h) usage ;;
    o) OUT="$OPTARG" ;;
    :) die "option -$OPTARG needs an argument" 1 ;;
    \?) die "unknown option: -$OPTARG (see -h)" 1 ;;
  esac
done
shift $(( OPTIND - 1 ))
(( $# == 0 )) || die "no positional args expected (see -h)" 1

[[ "$(id -u)" == 0 ]] || warn "not running as root — privpath reads may fail; do: adb root || su"

# push-safety: only ever output under our /sdcard/hackios/ directory
case "$OUT" in
  /sdcard/hackios/*) ;;
  *) die "refusing non-/sdcard output dir: ${OUT} (keeps us push-safe)" 1 ;;
esac

command -v diff >/dev/null 2>&1 || die "on-device diff missing — install busybox first (see snippets.md)" 3

PROBE=""
for candidate in /private/var/fstab; do
  if [[ -e "$candidate" ]]; then
    PROBE="$candidate"
    break
  fi
done
if [[ -z "$PROBE" ]]; then
  die "/private/var missing — are we booted into the IOS filesystem at all? (adb shell 'ls /')" 3
fi

mkdir -p "$OUT"
log "snapshotting ${#snapshot_targets[@]} patch targets to $OUT (baseline + drift diffs)"

cp_snap() {  # cp_snap SRC DST — cp -a with an on-device cp -R fallback
  if ! cp -a "$1" "$2" 2>/dev/null; then
    cp -R "$1" "$2"
  fi
}

RC=0
for t in "${snapshot_targets[@]}"; do
  name="$(basename "$t")"
  log "  → $t"
  if [[ ! -e "$t" ]]; then
    warn "    missing on this build — skipping (relocated? see userland/patches/fstab)"
    RC=3
    continue
  fi
  if [[ ! -e "$OUT/${name}.baseline" ]]; then
    if ! cp_snap "$t" "$OUT/${name}.baseline"; then
      warn "    baseline copy failed (permissions?)"
      RC=3
      continue
    fi
    log "    baseline created: $OUT/${name}.baseline (unmodified stock)"
  elif diff -u "$OUT/${name}.baseline" "$t" > "$OUT/${name}.patch"; then
    log "    unchanged since baseline"
    rm -f "$OUT/${name}.patch"
  else
    log "    drift vs baseline → $OUT/${name}.patch"
  fi
done

# Manifest of what we touched — follow-up agents read this first
{
  echo "# create_patches.sh runtime manifest ($(date -u +%FT%TZ))"
  echo "device: $(getprop ro.product.model 2>/dev/null || echo unknown)"
  echo "ios-version-key-line: $(grep -m1 ProductVersion /System/Library/CoreServices/SystemVersion.plist 2>/dev/null || echo unknown)"
  echo "targets:"
  for t in "${snapshot_targets[@]}"; do
    echo "  - $t"
  done
  echo "outputs: <name>.baseline (first-run stock copy) · <name>.patch (drift diff if any)"
} > "$OUT/manifest.txt" 2>/dev/null || warn "manifest write failed"

log "done — pull with:  adb pull '$OUT'"
log "never push modified files back manually; userland/apply_rootfs_patches.sh owns that"
exit "$RC"
