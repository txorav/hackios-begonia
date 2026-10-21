#!/usr/bin/env bash
#
# fetch_ipsw.sh — HackiOS-begonia (userland/ scope)
#
# Download the iPhone10,3 iOS 16.5 (Build 20F66) full-restore IPSW from Apple's
# release CDN, with -C resume support, a SHA1/SHA256 record, and a documented
# fallback (api.ipsw.me) if Apple's CDN URL ever dies.
#
#   Primary (from porting plan, iOS 16.5 / 20F66 restore bundle):
#     https://updates.cdn-apple.com/2022SpringFCS/fullrestores/012-13311/
#            0ff319df-969e-4ae1-95aa-76ef67dd92f3/
#            iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw
#   Fallback (community mirror index):  https://api.ipsw.me/v4/device/iPhone10,3?type=ipsw
#
# The bundle covers both iPhone X identifiers (iPhone10,3 = our target,
# iPhone10,6 = the other iPhone X SKU); a single IPSW serves both.
# Our work only ever consumes the iPhone10,3 / board-D22AP identity.
#
# Usage:
#   ./fetch_ipsw.sh [--dry-run] [--work DIR] [--out FILE] [--force] [-q]
#   ./fetch_ipsw.sh --url "https://..." --sha1 40hexdigits   # custom override
#
# Exit codes: 0 ok, 1 usage error, 2 curl/download failure, 3 checksum mismatch,
#             4 fallback lookup failed (Apple URL 404 and ipsw.me unusable).
#
# NOTE: no network access is performed when --dry-run is given.

set -euo pipefail

PROG="$(basename "${BASH_SOURCE[0]}")"
DEFAULT_WORK="${HOME}/hackios-begonia-work"

# ---- canonical data (keep in sync with userland/README.md) ------------------
IPSW_DEVICE_IDENT="iPhone10,3"
IPSW_PAYLOAD_NAME="iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw"
IPSW_URL_PRIMARY="https://updates.cdn-apple.com/2022SpringFCS/fullrestores/012-13311/0ff319df-969e-4ae1-95aa-76ef67dd92f3/iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw"
IPSW_FALLBACK_API="https://api.ipsw.me/v4/device/iPhone10,3?type=ipsw"
EXPECTED_IOS_VERSION="16.5"
EXPECTED_BUILD="20F66"

banner() {
  cat <<EOF
===============================================================================
 ${PROG} — HackiOS-begonia userland :: fetch_ipsw.sh
 target device : Xiaomi Redmi Note 8 Pro (begonia) / MT6785 (Helio G90T)
 target ident  : ${IPSW_DEVICE_IDENT} (board D22AP, A11 Bionic, arm64, no PAC)
 iOS target    : iOS ${EXPECTED_IOS_VERSION} (Build ${EXPECTED_BUILD})
===============================================================================
EOF
}

log()  { printf '[fetch] %s\n' "$*"; }
warn() { printf '[fetch] WARNING: %s\n' "$*" >&2; }
die()  { printf '[fetch] ERROR: %s\n' "$*" >&2; exit "${2:-1}"; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "required command '$1' not found in PATH (install it, e.g. via your package manager)" 1
}

usage() {
  sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 0
}

# ---------------------------------------------------------------------------
# parse args (getopts; long opts via manual shift loop)
# ---------------------------------------------------------------------------
WORK_DIR="$DEFAULT_WORK"
OUT_FILE=""
DRY_RUN=0
FORCE=0
QUIET=0
CUSTOM_URL=""
CUSTOM_SHA1=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run) DRY_RUN=1 ;;
    --force)   FORCE=1 ;;
    -q|--quiet) QUIET=1 ;;
    --work) WORK_DIR="${2:?--work needs a value}"; shift ;;
    --work=*) WORK_DIR="${1#*=}" ;;
    --out) OUT_FILE="${2:?--out needs a value}"; shift ;;
    --out=*) OUT_FILE="${1#*=}" ;;
    --url) CUSTOM_URL="${2:?--url needs a value}"; shift ;;
    --sha1) CUSTOM_SHA1="${2:?--sha1 needs a value}"; shift ;;
    -h|--help|help) usage ;;
    *) die "unknown option: $1 (see --help)" 1 ;;
  esac
  shift
done

banner

need_cmd curl
need_cmd python3

IPSW_DIR="${WORK_DIR}/ipsw"
# shellcheck disable=SC2174
mkdir -p -- "${IPSW_DIR}"

if [[ -n "$OUT_FILE" ]]; then
  OUT_FILE="$(readlink -f "$OUT_FILE")"
else
  OUT_FILE="${IPSW_DIR}/${IPSW_PAYLOAD_NAME}"
fi
SHA1_SIDECAR="${OUT_FILE}.sha1"
SHA256_SIDECAR="${OUT_FILE}.sha256"

if [[ -n "$CUSTOM_URL" ]]; then
  PRIMARY_URL="$CUSTOM_URL"
else
  PRIMARY_URL="$IPSW_URL_PRIMARY"
fi

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

# compute sha1/sha256 (also mirrors to sidecar files). streams, never loads to RAM.
record_hashes() {
  local f="$1"
  log "computing SHA1 + SHA256 (streamed) ..."
  sha1sum   "$f" > "$SHA1_SIDECAR"
  sha256sum "$f" > "$SHA256_SIDECAR"
  log "sha1(16 chars)   = $(cut -c1-16 < "$SHA1_SIDECAR") ..."
  log "full sha1/sha256 written to: ${SHA1_SIDECAR} ${SHA256_SIDECAR}"
}

# query api.ipsw.me for the device's firmware list; extract url+sha1 for 16.5/20F66.
lookup_fallback() {
  log "fallback: querying ${IPSW_FALLBACK_API}"
  local tmp
  tmp="$(mktemp)" || die "mktemp failed" 2
  # -L: ipsw.me redirects; --connect-timeout keeps a dead API from hanging bring-up
  if ! curl -L --connect-timeout 20 --max-time 300 -f -sS \
        -o "$tmp" "$IPSW_FALLBACK_API"; then
    rm -f "$tmp"
    return 1
  fi
  # python3 (stdlib json only) extracts the entry matching our build; only the
  # exact 20F66 build is valid for our plan (16.5.x security patches point to
  # different builds and are NOT the userland this project pins).
  python3 - "$tmp" <<'PYEOF' || { rm -f "$tmp"; return 1; }
import json, sys
with open(sys.argv[1]) as fh:
    data = json.load(fh)
firmwares = data.get("firmwares") or []
entry = next((f for f in firmwares if f.get("buildid") == "20F66"), None)
if not entry:
    sys.exit(1)
url = entry.get("url") or ""
sha1 = (entry.get("sha1sum") or entry.get("sha1") or "").lower()
if not url:
    sys.exit(1)
print(url)
if sha1:
    print(sha1)
PYEOF
  rm -f "$tmp"
}

# hash existing file; echo "sha1 sha256"
current_hashes() {
  local f="$1"
  local s1 s2
  s1=$(sha1sum "$f" | cut -d' ' -f1)
  s2=$(sha256sum "$f" | cut -d' ' -f1)
  echo "$s1 $s2"
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
DATED_CLASSIC_URL="$PRIMARY_URL"

if (( DRY_RUN )); then
  log "DRY-RUN: URLs only (no download)."
  log "primary : ${DATED_CLASSIC_URL}"
  log "note    : if this URL 404s, the script falls back automatically to:"
  log "fallback: ${IPSW_FALLBACK_API}  → resolves to Apple's CDN mirror copy of"
  log "          ${IPSW_PAYLOAD_NAME} (+ sha1 for verification if published)."
  log "work dir: ${WORK_DIR}"
  exit 0
fi

RESUME_FLAGS=(-C -)
CURL_FLAGS=(-f -sS -L --connect-timeout 30 --retry 3 --retry-delay 5)
(( QUIET )) || CURL_FLAGS+=("--progress-bar")

# Re-download decision: exist + nonempty, unless --force
try_download() {
  local url="$1"
  log "downloading: ${url}"
  if (( FORCE )) || [[ ! -s "$OUT_FILE" ]]; then
    :
  else
    log "existing file present; resuming/completing with curl -C - (use --force to restart)"
  fi
  curl "${CURL_FLAGS[@]}" "${RESUME_FLAGS[@]}" -o "$OUT_FILE" "$url"
}

sha1_expected=""
if [[ -n "$CUSTOM_SHA1" ]]; then
  sha1_expected="$(echo "$CUSTOM_SHA1" | tr 'A-F' 'a-f' | tr -d ' ')"
elif [[ -s "$SHA1_SIDECAR" ]]; then
  sha1_expected="$(cut -d' ' -f1 < "$SHA1_SIDECAR")"
fi

# Attempt 1: Apple CDN primary.
set +e
try_download "$DATED_CLASSIC_URL"
rc=$?
set -e

# Attempt 2 (on 404/expiry/generic curl failure): api.ipsw.me lookup.
if (( rc != 0 )); then
  warn "Apple CDN download failed (curl rc=${rc})."
  warn "The IPSW URL above may have been retracted/renamed by Apple (release CDN"
  warn "paths are periodically reorganized). You need a re-download either from"
  warn "the primary URL once Apple restores it, or from the fallback index."
  log "Resolving via fallback index ..."
  if ! FALLBACK_OUT="$(lookup_fallback)"; then
    die "Apple URL failed AND fallback lookup failed.
Manual steps:
  1) open ${IPSW_FALLBACK_API} in a browser and take the 20F66 'url' field, or
  2) use ipsw.me / theiphonewiki 2026 direct-link archive, then run:
       ${PROG} --url '<resolved-url>' --sha1 <sha1-if-known>
  3) Re-run './unpack_ipsw.py --work ${WORK_DIR}' afterwards." 4
  fi
  FALLBACK_URL="$(printf '%s\n' "$FALLBACK_OUT" | sed -n '1p')"
  FALLBACK_SHA1="$(printf '%s\n' "$FALLBACK_OUT" | sed -n '2p')"
  if [[ -n "$FALLBACK_SHA1" && -z "$sha1_expected" ]]; then
    sha1_expected="$FALLBACK_SHA1"
  fi
  log "fallback resolved: ${FALLBACK_URL}"
  try_download "$FALLBACK_URL"
fi

[[ -s "$OUT_FILE" ]] || die "download produced no file at ${OUT_FILE}" 2

# Quick sanity: iOS IPSWs are zip files; zip magic = "PK\x03\x04"
MAGIC="$(head -c 4 "$OUT_FILE" | od -An -tx1 | tr -d ' \n')"
if [[ "$MAGIC" != "504b0304" ]]; then
  die "downloaded file does not start with zip magic (PK\x03\x04); got ${MAGIC}.
This usually means the CDN served an error page or a redirect page.
Delete ${OUT_FILE} and retry, or use --dry-run to check the URL." 2
fi

if [[ ! -s "$SHA256_SIDECAR" || ! -s "$SHA1_SIDECAR" ]] || (( FORCE )); then
  record_hashes "$OUT_FILE"
fi

ACTUAL_SHA1="$(cut -d' ' -f1 < "$SHA1_SIDECAR")"
if [[ -n "$sha1_expected" && "$sha1_expected" != "none" ]]; then
  if [[ "$ACTUAL_SHA1" != "$sha1_expected" ]]; then
    die "SHA1 MISMATCH:
  expected : ${sha1_expected}
  actual   : ${ACTUAL_SHA1}
The file is either truncated (relay the download again; curl -C - resumes) or
came from a mirror that modified it. Trust nothing; re-download from Apple CDN." 3
  fi
  log "sha1 verified: ${ACTUAL_SHA1}"
else
  warn "no expected SHA1 available (no sidecar, --sha1 not given, ipsw.me gave none)."
  warn "SHA1 recorded locally for future re-verification: ${ACTUAL_SHA1}"
fi

log "done: ${OUT_FILE} ($(du -h "$OUT_FILE" | cut -f1))"
log "next : ./unpack_ipsw.py --work ${WORK_DIR}"
exit 0
