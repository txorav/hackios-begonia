#!/usr/bin/env bash
#
# apply_rootfs_patches.sh — HackiOS-begonia userland :: Phase 5 step 3.
#
# Runs AFTER the iPhone10,3 iOS 16.5 (Build 20F66) rootfs from the IPSW has
# been (a) [macOS] mounted RW via hdiutil, (b) [Linux] extracted to a plain
# file tree, or (c) staged/on-device in recovery — and applies our patches:
#
#   (a) dyld_shared_cache_arm64 presence/merge. A11 is a NON-cryptex
#       architecture: the merged cache normally ships INSIDE the rootfs DMG
#       already. We verify (never assume); only if missing/placeholder do we
#       merge via ../ipsw/merge_rd.py from the restore-ramdisk candidates.
#   (b) /private/etc/fstab replaced with patches/fstab (makes / rw, apfs).
#   (c) /var/db seeding bookkeeping. Stock rootfs DMG has NO /var (data
#       volume is separate) — the on-device hack is documented in
#       ipsw/LayoutNotes.md; we only seed when a /var/db tree exists.
#   (d) MobileGestalt MGCopyAnswers patch, via ../patch_mobilegestalt.py.
#   (e) documentation-only: which missing Apple services are answered by stub
#       kexts in kexts/ (kernel-port scope) and which ADT properties they
#       read (docs/hardware_map_mt6785.md §3).
#   (f) records patch_manifest.json — sha256 before/after of every file
#       touched, all notes above — so nothing about patching is silent.
#
# Idempotent by design: re-running is safe; original files are backed up once
# as <name>.hackios-orig and never re-clobbered.
#
# Options:
#   --root DIR     rootfs directory (extracted tree or mount point).
#                  [default: <work>/patched/rootfs_patched — patched COPY mode]
#   --work DIR     work dir                [default: ~/hackios-begonia-work]
#   --skip-fstab   skip step (b)
#   --skip-mg      skip step (d)
#   --skip-dyld    skip step (a)
#   --rollback     restore every .hackios-orig backup recorded in the manifest
#   --help         this text
#
# Exit codes: 0 ok · 1 usage · 2 environment · 3 rootfs sanity · 4 patch failed.

set -euo pipefail

PROG="$(basename "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_WORK="${HOME}/hackios-begonia-work"

banner() {
  cat <<EOF
===============================================================================
 ${PROG} — HackiOS-begonia userland :: apply_rootfs_patches.sh
 device      : Xiaomi Redmi Note 8 Pro (begonia) · MT6785 (Helio G90T) · ARMv8.2-A
 identity    : iPhone10,3 · board D22AP · chip-id 0x8020 (= 32800, A11) · arm64
 rootfs      : iOS 16.5 (Build 20F66) iPhone10,3 payload
===============================================================================
EOF
}

log()  { printf '[patch] %s\n' "$*"; }
warn() { printf '[patch] WARNING: %s\n' "$*" >&2; }
die()  { printf '[patch] ERROR: %s\n' "$*" >&2; exit "${2:-4}"; }
usage() { sed -n '8,50p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

# ---------------------------------------------------------------------------
# args
# ---------------------------------------------------------------------------
WORK_DIR="$DEFAULT_WORK"
ROOT_DIR=""
DO_FSTAB=1
DO_MG=1
DO_DYLD=1
ROLLBACK=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) ROOT_DIR="${2:?--root needs a value}"; shift ;;
    --root=*) ROOT_DIR="${1#*=}" ;;
    --work) WORK_DIR="${2:?--work needs a value}"; shift ;;
    --work=*) WORK_DIR="${1#*=}" ;;
    --skip-fstab) DO_FSTAB=0 ;;
    --skip-mg) DO_MG=0 ;;
    --skip-dyld) DO_DYLD=0 ;;
    --rollback) ROLLBACK=1 ;;
    -h|--help|help) usage ;;
    *) die "unknown option: $1 (see --help)" 1 ;;
  esac
  shift
done

banner

command -v python3 >/dev/null 2>&1 || die "python3 not found" 2
command -v sha256sum >/dev/null 2>&1 || die "sha256sum not found (coreutils)" 2
command -v stat >/dev/null 2>&1 || die "stat not found (coreutils)" 2

[[ -n "$ROOT_DIR" ]] || ROOT_DIR="${WORK_DIR}/patched/rootfs_patched"
# normalize: works for both symlinked mounts and plain trees
ROOT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$ROOT_DIR")"
MANIFEST="${WORK_DIR}/patched/patch_manifest.json"
mkdir -p -- "${WORK_DIR}/patched"

log "root dir : ${ROOT_DIR}"
log "manifest : ${MANIFEST}"

if [[ ! -d "$ROOT_DIR" ]]; then
  die "rootfs directory does not exist: ${ROOT_DIR}
${ROOT_DIR} is:
  * the COPY mode default — first extract the DMG there (unpack_ipsw.py output,
    then 'cp -a <unpacked>/rootfs.dmg-tree patched/rootfs_patched'), or
  * pass --root <your mounted/extracted tree>.
See userland/README.md §5." 1
fi

# ---------------------------------------------------------------------------
# rollback mode
# ---------------------------------------------------------------------------
if (( ROLLBACK )); then
  log "rollback: restoring .hackios-orig backups recorded in the manifest"
  [[ -f "$MANIFEST" ]] || die "no patch manifest at ${MANIFEST}" 3
  python3 - "$MANIFEST" <<'PYRO'
import json, os, shutil, sys
m = json.load(open(sys.argv[1]))
restored = 0
for step in m.get("steps", []):
    for op in step.get("ops", []):
        orig, target = op.get("orig_backup"), op.get("target")
        if orig and os.path.isfile(orig) and target:
            shutil.copy2(orig, target)
            restored += 1
        elif op.get("created_file") and target and os.path.isfile(target):
            os.unlink(target)
            restored += 1
print(f"[rollback] restored/removed entries: {restored}")
PYRO
  log "rollback complete"
  exit 0
fi

# ---------------------------------------------------------------------------
# sanity: is this really the iPhone10,3 iOS 16.5 rootfs?
# ---------------------------------------------------------------------------
SYSVERS="${ROOT_DIR}/System/Library/CoreServices/SystemVersion.plist"
if [[ ! -f "$SYSVERS" ]]; then
  die "not an iOS rootfs (${SYSVERS} missing).
Mount or extract the DMG produced by unpack_ipsw.py first (README.md §5)." 3
fi
python3 - "$SYSVERS" <<'PYSEL' || warn "could not parse SystemVersion.plist"
import plistlib, sys
with open(sys.argv[1], "rb") as fh:
    sv = plistlib.load(fh)
print(f"rootfs identity check: DeviceName={sv.get('DeviceName','?')} "
      f"iOS={sv.get('ProductUserVisibleVersion','?')} "
      f"build={sv.get('ProductBuildVersion','?')}")
PYSEL

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

append_manifest() {  # append_manifest <json-ops-string>
  local ops="$1"
  python3 - "$MANIFEST" "$ops" "$PROG" <<'PYAPP'
import json, sys, time
mf_path, ops_json, prog = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    with open(mf_path) as fh:
        m = json.load(fh)
    if not isinstance(m, dict) or "steps" not in m:
        m = {"schema": "hackios-begonia/patch_manifest/v1", "steps": []}
except (FileNotFoundError, json.JSONDecodeError):
    m = {"schema": "hackios-begonia/patch_manifest/v1", "steps": []}
m.setdefault("generated_by", prog)
m["steps"].append({"ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "ops": ops_json})
with open(mf_path, "w") as fh:
    json.dump(m, fh, indent=2)
PYAPP
}

# build a safe JSON string from shell vars (quoting handled inside python)
mkjson() {  # mkjson key=value ...  -> prints compact JSON to stdout
  python3 - "$@" <<'PYJS'
import json, sys
out = {}
for arg in sys.argv[1:]:
    kv = arg.split("=", 1)
    if len(kv) != 2:
        raise SystemExit(f"bad pair {arg!r} (form: key=value)")
    key, val = kv
    if val == "null" or val == "":
        out[key] = None
    elif val in ("true", "false"):
        out[key] = val == "true"
    elif val.isdigit():
        out[key] = int(val)
    else:
        out[key] = val
print(json.dumps(out, separators=(",", ":")), end="")
PYJS
}

# ---------------------------------------------------------------------------
# step (a): dyld_shared_cache_arm64 — verify / merge if missing
# ---------------------------------------------------------------------------
DYLD_REL="System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"

if (( DO_DYLD )); then
  echo
  log "step (a): dyld_shared_cache_arm64 (A11 non-cryptex layout)"
  DYLD_PATH="${ROOT_DIR}/${DYLD_REL}"
  if [[ -f "$DYLD_PATH" ]]; then
    SIZE="$(stat -c '%s' "$DYLD_PATH")"
    if (( SIZE > 100000000 )); then
      log "  present & real: ${DYLD_REL} (${SIZE} bytes) — no merge needed (expected on A11)"
      append_manifest "$(mkjson step=a_dyld_opcode result=already_present size=$SIZE)"
    else
      warn "dyld cache is only ${SIZE} bytes — placeholder/stripped stub?"
      log "  attempting merge from unpacked candidates via ipsw/merge_rd.py"
      python3 "${SCRIPT_DIR}/ipsw/merge_rd.py" --rootfs "$ROOT_DIR" \
        --candidates "${WORK_DIR}/unpacked" \
        || die "merge_rd.py failed; boot with a missing dyld cache will hang dyld" 4
      append_manifest "$(mkjson step=a_dyld_opcode result=merged_via_merge_rd size=$SIZE)"
    fi
  else
    log "  dyld cache ABSENT at ${DYLD_REL} — attempting merge via ipsw/merge_rd.py"
    python3 "${SCRIPT_DIR}/ipsw/merge_rd.py" --rootfs "$ROOT_DIR" \
      --candidates "${WORK_DIR}/unpacked" \
      || die "merge_rd.py failed; dyld cache still missing — do NOT boot yet" 4
    append_manifest "$(mkjson step=a_dyld_opcode result=merged_via_merge_rd)"
  fi
fi

# ---------------------------------------------------------------------------
# step (b): fstab -> rw (APFS), single-shot backup
# ---------------------------------------------------------------------------
FSTAB_FILE="${ROOT_DIR}/private/etc/fstab"
OUR_FSTAB="${SCRIPT_DIR}/patches/fstab"

if (( DO_FSTAB )); then
  echo
  log "step (b): /private/etc/fstab -> patched rw/apfs variant"
  [[ -f "$OUR_FSTAB" ]] || die "repository file missing: ${OUR_FSTAB}" 2
  if [[ ! -d "$(dirname "$FSTAB_FILE")" ]]; then
    die "no private/etc directory in ${ROOT_DIR} (extraction layout unexpected)" 3
  fi
  SHA_BEFORE=""
  BACKUP="${FSTAB_FILE}.hackios-orig"
  if [[ -f "$FSTAB_FILE" ]]; then
    SHA_BEFORE="$(sha256_of "$FSTAB_FILE")"
    if [[ ! -f "$BACKUP" ]]; then
      cp -p -- "$FSTAB_FILE" "$BACKUP"
      log "  original backed up to $(basename "$BACKUP")"
    else
      log "  backup already present — keeping the FIRST one"
    fi
  fi
  cp -p -- "$OUR_FSTAB" "$FSTAB_FILE"
  SHA_AFTER="$(sha256_of "$FSTAB_FILE")"
  append_manifest "$(mkjson step=b_fstab target="$FSTAB_FILE" \
    sha_before="${SHA_BEFORE:-null}" \
    sha_after="${SHA_AFTER:-null}" \
    orig_backup="${BACKUP:+$BACKUP}")"
  log "  fstab replaced — system volume is now marked rw (was ro via boot mount + snapshot)"
fi

# ---------------------------------------------------------------------------
# step (c): /var/db seeding (rootfs usually has no /var — check, never assume)
# ---------------------------------------------------------------------------
echo
log "step (c): /var/db seed bookkeeping"
if [[ -d "${ROOT_DIR}/var/db" ]]; then
  mkdir -p -- "${ROOT_DIR}/var/db/hackios"
  python3 - "${WORK_DIR}" >"${ROOT_DIR}/var/db/hackios/provisioned.json" <<'PYSEED'
import json, sys, time
sys.stdout.write(json.dumps({
    "hackios-begonia": True,
    "device": "Xiaomi Redmi Note 8 Pro (begonia), MT6785 Helio G90T",
    "identity": "iPhone10,3 / D22AP",
    "ios": "16.5 (Build 20F66)",
    "provisioned_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "work_dir": sys.argv[1],
}, indent=2))
PYSEED
  append_manifest "$(mkjson step=c_var_db target=/var/db/hackios/provisioned.json created_file=true)"
  log "  /var/db/hackios/provisioned.json written"
else
  log "  /var/db ABSENT in this rootfs tree — EXPECTED: the stock DMG carries no"
  log "  /var (data volume is separate and gets initialized on-device)."
  log "  On-device /var/db seeding hack: see ipsw/LayoutNotes.md §3."
  append_manifest "$(mkjson step=c_var_db result=skipped_no_var_dir)"
fi

# ---------------------------------------------------------------------------
# step (d): MobileGestalt patch
# ---------------------------------------------------------------------------
if (( DO_MG )); then
  echo
  log "step (d): MobileGestalt MGCopyAnswers patch"
  python3 "${SCRIPT_DIR}/patch_mobilegestalt.py" --root "$ROOT_DIR" --all-targets \
    || die "patch_mobilegestalt.py failed — rootfs unusable for launchd quirks check" 4
  append_manifest "$(mkjson step=d_mobilegestalt result=performed_by_patch_mobilegestalt_py)"
fi

# ---------------------------------------------------------------------------
# steps (e)+(f): documentation-only pointers (record in the manifest log too)
# ---------------------------------------------------------------------------
echo
log "step (e): Apple check-service stubs (documentation record — kernel-port scope)"
cat <<'EONOTE'
  These IOKit services DO NOT exist on MT6785 hardware and are answered by
  stub kexts maintained in kexts/ (owned by kernel-port, NOT userland):
    AppleKeyStore / AppleSEPManager -> SEPStub     (keybag unlock MFBUnlockDevice success)
    AppleMobileApnonce              -> SEPStub     (fake apnonce + DeviceTree props)
    IOWatchDogTimer                 -> WatchDogStub (prevents watchdogd 60s panic)
  The fake /DeviceTree (ADT) properties they consume are produced by
  xnuloader (fdt2adt) per docs/hardware_map_mt6785.md §3: /chosen {chip-id
  0x8020, board-id "D22AP", serial-number, unique-chip-id} and /product
  {product-name "iPhone10,3", board-name "D22AP", fake MLB/Serial}.
EONOTE
append_manifest "$(mkjson step=e_service_stubs result=documentation_recorded)"

log "step (f): ADT value cross-pointer also recorded in the manifest"
append_manifest "$(mkjson step=f_adt_pointer reference='docs/hardware_map_mt6785.md §3 /chosen,/product')"

echo
log "ALL STEPS COMPLETE."
log "  manifest : ${MANIFEST}"
log "  next     : review the tree (diff .hackios-orig files), then stage per README §5.1;"
log "             rollback with: $PROG --rollback --work ${WORK_DIR}"
exit 0
