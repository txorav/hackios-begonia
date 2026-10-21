#!/usr/bin/env bash
#
# install_hosttoolchain.sh — HackiOS-begonia tools/
#
# Installs the HOST-side packages needed to build:
#   * xnuloader            (make, a freestanding aarch64 compiler — clang via
#                           apt or a Linaro/none-elf toolchain if present),
#   * kexts & XNU helpers  (make, bison, flex, dtc, libssl, libelf, python3),
#   * packaging utilities  (mkbootimg, fastboot, adb — where apt has them),
#   * image tooling used by userland scripts (hfsprogs, dosfstools).
#
# This script does NOT build any cross toolchain (that is
# install_llvm_toolchain.sh) and never runs a `git clone` — fully apt-only.
# Idempotent: apt-get install is naturally idempotent; re-run freely.
#
# Options:
#   --list           print the package list and exit (no system changes)
#   --skip-optional  skip emulated-device tools (fastboot/adb/mkbootimg) if
#                    your distro lacks them, without failing
#   --help
#
# Exit codes: 0 ok · 1 usage · 2 apt failure.

set -euo pipefail

PROG="$(basename "${BASH_SOURCE[0]}")"
LIST_ONLY=0
SKIP_OPTIONAL=0

CORE_PKGS=(build-essential make git python3 python3-dev
           bc bison flex libssl-dev libelf-dev zlib1g-dev
           device-tree-compiler dosfstools hfsprogs curl wget
           attr libc6-dev groff uuid-dev libxml2-dev)
OPTIONAL_PKGS=(mkbootimg fastboot adb ccache ninja-build pkg-config)

banner() {
  printf '===============================================================================\n'
  printf ' %s — HackiOS-begonia host-side (apt-only, no source builds)\n' "$PROG"
  printf ' kernel-port + userland host packages · dtc/hfsprogs/dosfstools included\n'
  printf '===============================================================================\n'
}
log()  { printf '[hosttoolchain] %s\n' "$*"; }
warn() { printf '[hosttoolchain] WARNING: %s\n' "$*" >&2; }
die()  { printf '[hosttoolchain] ERROR: %s\n' "$1" >&2; exit "${2:-2}"; }
usage(){ sed -n '12,42p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --list) LIST_ONLY=1 ;;
    --skip-optional) SKIP_OPTIONAL=1 ;;
    -h|--help|help) usage ;;
    *) die "unknown option: $1 (see --help)" 1 ;;
  esac
  shift
done

banner

if (( LIST_ONLY )); then
  echo "core packages     : ${CORE_PKGS[*]}"
  echo "optional packages : ${OPTIONAL_PKGS[*]} (skipped if not packaged)"
  exit 0
fi

[[ "$(uname -s)" == "Linux" ]] || die "Ubuntu-host oriented; on macOS use tools/README.md guidance (brew)" 2

if [[ "$(id -u)" != 0 ]]; then
  command -v sudo >/dev/null 2>&1 || die "need root or sudo for apt" 2
  SUDO=sudo
else
  SUDO=""
fi

log "apt-get update"
$SUDO apt-get update -y || warn "apt-get update failed; continuing against the existing cache"

log "installing core packages (${#CORE_PKGS[@]} pkgs)"
if ! $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y "${CORE_PKGS[@]}"; then
  die "core apt install failed — inspect the apt output above.
Hint: enable 'universe' repo for hfsprogs:
  sudo add-apt-repository universe && sudo apt-get update" 2
fi
log "core packages installed"

if (( SKIP_OPTIONAL )); then
  log "--skip-optional: not attempting ${OPTIONAL_PKGS[*]}"
else
  log "installing optional device tools (${OPTIONAL_PKGS[*]})"
  if ! $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y "${OPTIONAL_PKGS[@]}"; then
    warn "optional packages partially unavailable on this distro.
  Missing fastboot/adb/mkbootimg are fine during Phase-1 (pure host work);
  install Android platform-tools manually when device-time comes:
  https://developer.android.com/tools/releases/platform-tools"
  else
    log "optional packages installed"
  fi
fi

log "sanity checks"
for cmd in gcc make python3 dtc mkfs.hfsplus; do
  if command -v "$cmd" >/dev/null 2>&1; then
    log "  ok: $cmd"
  else
    warn "  still missing: $cmd"
  fi
done
log "now run: ./install_llvm_toolchain.sh  (the cross piece), then ./check_env.sh"
exit 0
