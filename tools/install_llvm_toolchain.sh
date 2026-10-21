#!/usr/bin/env bash
#
# install_llvm_toolchain.sh — HackiOS-begonia tools/
#
# Installs, idempotently and verifiably:
#   1. LLVM {clang, lld} from apt (llvm-16 preferred, falls back to llvm-15)
#      that can target BOTH aarch64-none-elf (xnuloader) and
#      *-apple-darwin (Apple Mach-O object emission for iOS-side binaries).
#   2. cctools-port (https://github.com/tpoechtrager/cctools-port), built with
#      --target=arm-apple-darwin, which gives us ld64 + dsymutil + otool +
#      lipo able to LINK arm64 Mach-O on Linux. (ld64 linker from cctools-port
#      is what the static-shell and kext builds actually invoke.)
#   3. ldid from https://github.com/sbingner/ldid — ad-hoc Mach-O fake signer
#      ("ldid -S").
#
# Network use: git clones + (optionally) apt. Opt out of the clones with
# --no-net (then the script only validates what already exists, validating
# an operator's pre-provisioned toolchain, e.g. an air-gapped host).
#
# Idempotency: a marker file in --prefix records success; re-running a
# completed install fast-exits unless --force.
#
# Options:
#   --prefix DIR    install root (default /opt/hackios-toolchain)
#   --jobs N        parallel build jobs (default: nproc)
#   --no-tapi       skip libtapi build (only if ld64's tapi use is unavailable)
#   --no-net        never touch the network; only use what is already there
#   --force         re-run even if the marker exists
#   --help
#
# Exit codes: 0 ok · 1 usage · 2 environment failure · 3 build failure.

set -euo pipefail

PROG="$(basename "${BASH_SOURCE[0]}")"
PREFIX="/opt/hackios-toolchain"
WORKROOT="/tmp/hackios-toolchain-build"
JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
DO_TAPI=1
DO_NET=1
FORCE=0
MARKER=""

log()  { printf '[toolchain] %s\n' "$*"; }
warn() { printf '[toolchain] WARNING: %s\n' "$*" >&2; }
die()  { printf '[toolchain] ERROR: %s\n' "$1" >&2; exit "${2:-2}"; }
usage(){ sed -n '14,60p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

banner() {
  printf '===============================================================================\n'
  printf ' %s — cross toolchain installer for HackiOS-begonia\n' "$PROG"
  printf ' clang (aarch64-none-elf + apple-darwin) · cctools-port ld64 · ldid\n'
  printf ' NOTE: network allowed this run (git clone + apt) — see --no-net.\n'
  printf '===============================================================================\n'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?value needed}"; shift ;;
    --prefix=*) PREFIX="${1#*=}" ;;
    --jobs) JOBS="${2:?value needed}"; shift ;;
    --jobs=*) JOBS="${1#*=}" ;;
    --no-tapi) DO_TAPI=0 ;;
    --no-net) DO_NET=0 ;;
    --force) FORCE=1 ;;
    -h|--help|help) usage ;;
    *) die "unknown option: $1 (see --help)" 1 ;;
  esac
  shift
done

banner
MARKER="${PREFIX}/.hackios-toolchain-ok"

#friendly preflight: prefix must be creatable/writable before we start work
if ! mkdir -p -- "$PREFIX/bin" 2>/dev/null || [[ ! -w "$PREFIX" ]]; then
  die "cannot create or write to --prefix ${PREFIX}
  (are you non-root on a system prefix? try:  $0 --prefix \$HOME/.hackios-toolchain )" 2
fi

if [[ -f "$MARKER" && "$FORCE" != 1 ]]; then
  log "already installed (marker ${MARKER}); fast-exit unless --force"
  "${PREFIX}/bin/clang" --version 2>/dev/null | head -1 || true
  exit 0
fi

# ---------------------------------------------------------------------------
# sanity: OS + sudo
# ---------------------------------------------------------------------------
OSNAME="$(uname -s)"
case "$OSNAME" in
  Linux) ;;
  Darwin) die "macOS path is unscripted here — use Xcode clang + brew ldid
(see tools/README.md); this script is Linux (apt) oriented." 2 ;;
  *) die "unsupported OS: ${OSNAME}" 2 ;;
esac
if (( DO_NET )); then
  if [[ "$(id -u)" != 0 ]]; then
    command -v sudo >/dev/null 2>&1 || die "need root or sudo for apt" 2
    SUDO=sudo
  else
    SUDO=""
  fi
else
  SUDO=""
fi

# ---------------------------------------------------------------------------
# 1. LLVM from apt (prefer 16; fall back 15; fail loudly, never silently)
# ---------------------------------------------------------------------------
LLVM_BIN=""
install_apt_llvm() {
  log "apt: installing llvm-16/clang-16/lld-16 (fallback llvm-15)"
  local pkgs16=( "llvm-16" "clang-16" "lld-16" )
  local pkgs15=( "llvm-15" "clang-15" "lld-15" )
  if (( DO_NET )); then
    $SUDO apt-get update -y || warn "apt-get update failed; trying existing cache"
    if $SUDO apt-get install -y "${pkgs16[@]}"; then
      LLVM_BIN="/usr/lib/llvm-16/bin"
    elif $SUDO apt-get install -y "${pkgs15[@]}"; then
      LLVM_BIN="/usr/lib/llvm-15/bin"
    else
      die "apt could not install llvm-16 nor llvm-15 (check universe repos)" 2
    fi
  else
    # offline: probe what exists (llvm prefix dirs first, then PATH clangs)
    for cand in /usr/lib/llvm-16 /usr/lib/llvm-15 /usr/lib/llvm-14 \
                /usr/lib/llvm-17 /usr/lib/llvm-18; do
      if [[ -x "${cand}/bin/clang" ]]; then LLVM_BIN="${cand}/bin"; break; fi
    done
    if [[ -z "$LLVM_BIN" ]]; then
      for cand in clang clang-18 clang-17 clang-16 clang-15 clang-14; do
        if command -v "$cand" >/dev/null 2>&1; then
          LLVM_BIN="$(dirname "$(command -v "$cand")")"; break
        fi
      done
    fi
    [[ -n "$LLVM_BIN" ]] || die "--no-net mode: no llvm-14..18 clang found in PATH or /usr/lib/llvm-*; run without --no-net once, then it is cached" 2
  fi
}
install_apt_llvm
log "LLVM at: ${LLVM_BIN} $("$LLVM_BIN/clang" --version | head -1)"

# expose in our prefix for deterministic PATH use by other scripts
for tool in clang clang++ clang-16 clang-15 lld ld.lld llvm-ar llvm-nm llvm-objdump llvm-dwarfdump llvm-config; do
  if [[ -x "$LLVM_BIN/$tool" ]]; then
    ln -sf "$LLVM_BIN/$tool" "$PREFIX/bin/$tool"
  fi
done
ln -sf "$LLVM_BIN/clang" "$PREFIX/bin/clang"

# ---------------------------------------------------------------------------
# 2. cctools-port (ld64 etc.) — the Mach-O native linker
# ---------------------------------------------------------------------------
CCTOOLS_URL="https://github.com/tpoechtrager/cctools-port"
CCTOOLS_DIR="$WORKROOT/cctools-port"
build_cctools() {
  if [[ -d "$CCTOOLS_DIR/cctools" ]]; then
    log "reusing existing clone: $CCTOOLS_DIR"
  else
    if (( ! DO_NET )); then
      die "--no-net: cctools-port source not present at $CCTOOLS_DIR" 2
    fi
    log "git clone --depth 1 ${CCTOOLS_URL}"
    rm -rf -- "$CCTOOLS_DIR"
    git clone --depth 1 "$CCTOOLS_URL" "$CCTOOLS_DIR" \
      || die "git clone of cctools-port failed (check network/proxy)" 2
  fi
  log "configuring cctools with --target=arm-apple-darwin (per plan/tools README)"
  (
    set -e
    cd "$CCTOOLS_DIR/cctools"
    ./configure --prefix="$PREFIX" --target=arm-apple-darwin
    make -j"$JOBS"
    $SUDO make install
  ) || die "cctools-port build/install failed — read the upstream README in
 $CCTOOLS_DIR (the error surface is preserved above). Common causes:
  missing libtapi (built separately below, retry without --no-tapi),
  missing libxml2-dev / libssl-dev on the host." 3

  # per TARGET: these land in $PREFIX/bin by cctools' make install
  for tool in arm-apple-darwin-ld arm-apple-darwin-as arm-apple-darwin-ar \
              arm-apple-darwin-ranlib arm-apple-darwin-lipo arm-apple-darwin-otool; do
    [[ -x "$PREFIX/bin/$tool" ]] || warn "cctools did not install $tool (some builds omit some tools)"
  done
}

# ---------------------------------------------------------------------------
# 2b. libtapi (needed by modern ld64 for darwin dyld/shared cache formats)
# ---------------------------------------------------------------------------
TAPI_URL="https://github.com/tpoechtrager/libtapi"
build_tapi() {
  if (( ! DO_TAPI )); then
    warn "--no-tapi: skipping libtapi; ld64's dyld-info support may warn."
    return 0
  fi
  local TAPI_DIR="$WORKROOT/libtapi"
  if [[ -x "$PREFIX/bin/tapi" || -x "$TAPI_DIR/build/lib/libtapi.so" ]] || ls "$PREFIX"/lib/libtapi* 2>/dev/null | head -1 | grep -q tapi; then
    log "libtapi already present, skipping clone"
    return 0
  fi
  if (( ! DO_NET )); then
    warn "--no-net: libtapi not found and cannot be fetched (ld64 may refuse\n" \
         "  headers only linking is still possible; continue at your own risk)."
    return 0
  fi
  log "git clone --depth 1 ${TAPI_URL}"
  rm -rf -- "$TAPI_DIR"
  git clone --depth 1 "$TAPI_URL" "$TAPI_DIR" \
    || die "git clone of libtapi failed" 2
  (
    set -e
    cd "$TAPI_DIR"
    ./build.sh || die "libtapi build.sh failed (upstream recipe: see
      $TAPI_URL README)" 3
    # their canonical install target: make -C build install tapi
    $SUDO make -C build install tapi \
      || die "libtapi install failed (make -C build install tapi)" 3
  ) || { warn "libtapi failure treated as optional; ld64 may still link with
    reduced dyld-info checks"; return 0; }
}

build_tapi
build_cctools

# ---------------------------------------------------------------------------
# 3. ldid
# ---------------------------------------------------------------------------
LDID_URL="https://github.com/sbingner/ldid"
build_ldid() {
  local LDID_DIR="$WORKROOT/ldid"
  if [[ -x "$PREFIX/bin/ldid" && "$FORCE" != 1 ]]; then
    log "ldid already present at $PREFIX/bin/ldid"
    return 0
  fi
  if [[ -d "$LDID_DIR" ]]; then
    log "reusing existing ldid clone"
  elif (( DO_NET )); then
    log "git clone --depth 1 ${LDID_URL}"
    rm -rf -- "$LDID_DIR"
    git clone --depth 1 "$LDID_URL" "$LDID_DIR" || die "git clone ldid failed" 2
  else
    warn "--no-net: skipping ldid build (no clone present)."
    return 0
  fi
  (
    set -e
    cd "$LDID_DIR"
    make -j"$JOBS" || die "ldid build failed" 3
    $SUDO install -m 0755 "$LDID_DIR"/ldid "$PREFIX/bin/ldid" || true
    # some checkouts produce ./ldid in root, others under ldid/ldid — probe:
    if [[ ! -x "$PREFIX/bin/ldid" ]]; then
      $SUDO install -m 0755 "$LDID_DIR"/ldid/ldid "$PREFIX/bin/ldid" \
        || die "ldid install probe failed (see repo layout)" 3
    fi
  )
}

build_ldid

# ---------------------------------------------------------------------------
# marker + summary
# ---------------------------------------------------------------------------
if (( DO_NET )); then
  touch "$MARKER"
fi
log "installed under: $PREFIX"
log "verify with: PATH=$PREFIX/bin:\$PATH $PREFIX/bin/clang --version; ldid -V 2>/dev/null || true"
log "next: ./check_env.sh"
exit 0
