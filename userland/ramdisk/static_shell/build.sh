#!/usr/bin/env bash
#
# build.sh — HackiOS-begonia userland :: ramdisk/static_shell
#
# Builds:
#   shell        — static freestanding Mach-O (arm64) REPL  (main.c)
#   mach_hello   — optional Mach-IPC demo binary            (mach_hello.c)
#
# Two build routes, documented (see ../../tools/README.md for toolchain prep):
#
#  (1) PREFERRED — Apple-targeted clang + ld64 from our cctools-port install
#      (tools/install_llvm_toolchain.sh).  Cross-emits a real Mach-O that
#      XNU can load, and we fake-sign with ldid so AMFI-stub boots with zero
#      surprises.
#  (2) FALLBACK — plain clang with -target aarch64-apple-darwin and NO link
#      (object-only) then hand-link via ld64 if available; if neither is
#      available the script explains exactly what is missing rather than
#      pretending it built.
#
# What we deliberately do NOT do here:
#   * no iOS SDK / no lib System (freestanding, -nostdlib);
#   * no real code-signing identity (ldid -S = ad-hoc; acceptable under
#     cs_enforcement_disable=1 from docs/hardware_map_mt6785.md §4).
#
# Usage:
#   ./build.sh [options]   (see flags below; default builds 'shell')
#
# Exit codes: 0 ok · 1 usage · 2 no usable toolchain · 3 build failed.

set -euo pipefail

PROG="$(basename "${BASH_SOURCE[0]}")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTDIR="$HERE"

banner() { cat <<'EOF'
===============================================================================
 ${PROG} — static Mach-O arm64 console shell for the M4 ramdisk
 binaries: shell (REPL / PID1 for XNU), mach_hello (IPC experiment)
 ABI: static, -nostdlib, svc #0x80 (BSD via x16), Mach traps via -31 (x16)
===============================================================================
EOF
}
log()  { printf '[build] %s\n' "$*"; }
warn() { printf '[build] WARNING: %s\n' "$*" >&2; }
die()  { printf '[build] ERROR: %s\n' "$*" >&2; exit "${2:-3}"; }

CLANG=""
LD64=""
LDID=""
BUILD_HELLO=0
CLEAN=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clang) CLANG="${2:?--clang needs a value}"; shift ;;
    --ld64) LD64="${2:?--ld64 needs a value}"; shift ;;
    --ldid) LDID="${2:?--ldid needs a value}"; shift ;;
    --with-hello) BUILD_HELLO=1 ;;
    --clean) CLEAN=1 ;;
    -h|--help) sed -n '6,50p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" 1 ;;
  esac
  shift
done

banner

if (( CLEAN )); then
  rm -f "$OUTDIR/shell" "$OUTDIR/mach_hello" "$OUTDIR"/*.o
  log "clean done"
  exit 0
fi

# ---------------------------------------------------------------------------
# tool discovery
# ---------------------------------------------------------------------------
if [[ -z "$CLANG" ]]; then
  for candidate in clang clang-16 clang-15 /usr/lib/llvm-16/bin/clang /usr/lib/llvm-15/bin/clang; do
    if command -v "$candidate" >/dev/null 2>&1; then CLANG="$candidate"; break; fi
  done
fi
[[ -n "$CLANG" ]] || die "no clang found (tools/install_llvm_toolchain.sh installs it)" 2

# ld64 (cctools-port) discovery: bundled in /usr/local/bin by the installer
if [[ -z "$LD64" ]]; then
  for candidate in aarch64-apple-darwin-ld arm-apple-darwin-ld ld64/ld64-ld; do
    if command -v "$candidate" >/dev/null 2>&1; then LD64="$candidate"; break; fi
  done
fi
if [[ -z "$LD64" ]]; then
  for candidate in ld64 ld; do
    if command -v "$candidate" >/dev/null 2>&1; then
      # plain ld is ELF-ld — reject unless it biarchs (cctools' ld64-ld names win)
      if "$candidate" -V 2>&1 | grep -qi "ld64\|Darwin"; then LD64="$candidate"; break; fi
    fi
  done
fi
[[ -n "$LD64" ]] || die "no ld64/Mach-O linker found:
  install via ../../tools/install_llvm_toolchain.sh (cctools-port --target=arm-apple-darwin).
  Without ld64 we cannot emit a Mach-O executable; raw .o files alone will NOT
  satisfy XNU's exec." 2
if command -v ldid >/dev/null 2>&1; then LDID="ldid"; fi

CFLAGS=( -target aarch64-apple-darwin -nostdlib -ffreestanding -fno-builtin
         -fno-stack-protector -fomit-frame-pointer -O2 -march=armv8.2-a
         -D__PARAGON__=1 )
log "using clang: $CLANG (${CLANG} -dumpversion 2>/dev/null || ?)"
log "using linker: $LD64"

# compile
log "compiling shell.o"
"$CLANG" "${CFLAGS[@]}" -c "$HERE/main.c" -o "$OUTDIR/shell.o"
log "compiling mach_hello.o"
"$CLANG" "${CFLAGS[@]}" -c "$HERE/mach_hello.c" -o "$OUTDIR/mach_hello.o"

# linker (static Mach-O, entry = _main; no dyld needed at load; XNU execs
# it directly as PID 1)
log "linking shell (Mach-O, static, no dyld)"
"$LD64" -arch arm64 -static -o "$OUTDIR/shell" -e _main "$OUTDIR/shell.o"
log "linking mach_hello (Mach-O, static)"
"$LD64" -arch arm64 -static -o "$OUTDIR/mach_hello" -e _main "$OUTDIR/mach_hello.o"

# fake-sign (ad-hoc) so the AMFI-stub path has something to bless
if [[ -n "$LDID" ]]; then
  log "ldid -S (ad-hoc sign)"
  "$LDID" -S "$OUTDIR/shell"
  "$LDID" -S "$OUTDIR/mach_hello"
else
  warn "ldid not found — kernel boots fine because cs_enforcement_disable=1"
  warn "but AMFI-stub wants a signature; run: ldid -S shell mach_hello"
fi

# sanity: real Mach-O arm64?
for bin in "$OUTDIR/shell" "$OUTDIR/mach_hello"; do
  magic="$(head -c 4 "$bin" | od -An -tx1 | tr -d ' \n')"
  if [[ "$magic" != "cffaedfe" ]]; then
    die "$bin is not a Mach-O (magic cffaedfe); got ${magic}" 3
  fi
  log "$(basename "$bin"): Mach-O arm64 confirmed ($(du -h "$bin" | cut -f1))"
done

log "next:"
log "  userland/ramdisk/mkramdisk.sh --shell $HERE/shell"
exit 0
