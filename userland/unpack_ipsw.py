#!/usr/bin/env python3
"""
unpack_ipsw.py — HackiOS-begonia (userland/ scope) · Phase 5 step 2.

Opens the iPhone10,3 iOS 16.5 (Build 20F66) restore IPSW (a ZIP container,
stdlib `zipfile` only), classifies the restore payload DMGs, verifies their
digests against whichever digest key the bundle carries, extracts the DMGs,
and writes a machine-readable summary for the rest of the pipeline:

    <work>/unpacked/build_manifest_summary.json

Design notes (erroneous assumptions deliberately excluded):
  * A11 / iOS 16.5 is a CLASSIC rootfs restore: one large rootfs DMG
    (APFS) + RestoreRamDisk, NO cryptex (cryptexes started on A12+).
    We do not assume that blindly — we classify by size/name and report.
  * Digest key naming is NOT assumed: BuildManifest identities carry digests
    under `Digest` (IMG4-era) while legacy restore manifests sometimes use
    `FileChecksum`; we search a list of candidate keys and record which one
    matched.  (idevicerestore computes SHA1 of the DMG file bytes and
    compares; that is exactly what we do here for a `Digest`/`FileChecksum`
    match.)
  * SHA1 is what Apple's manifests carry for these payloads; SHA256 is
    additionally recorded by us for internal consistency.

Usage:
    unpack_ipsw.py --work DIR [--ipsw FILE] [--skip-dmg] [--force] [--quiet]

Exit codes: 0 ok, 2 missing inputs/parse failure, 3 digest mismatch,
            4 unsupported/unrecognised bundle shape.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib  # stdlib: parses XML and binary plists (BuildManifest.plist is XML)
import shutil
import sys
import zipfile
from typing import Any, Dict, List, Optional, Tuple

PROG = "unpack_ipsw.py"
VERSION = "1.0"

# Canonical constants (keep in sync with userland/README.md + fetch_ipsw.sh)
IPSW_DEFAULT_SUFFIX = "iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw"
EXPECTED_DEVICE = "iPhone10,3"
EXPECTED_BUILD = "20F66"
EXPECTED_PLATFORM = "8020"      # BuildManifest Platform field = hex chip-id "8020" = A11
EXPECTED_CHIPID_DEC = 0x8020    # 32800 (matches ADT /chosen chip-id + MG ChipID)
DYLD_CACHE_PATH = "System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
DYLD_CACHE_MIN_BYTES = 100 * 1024 * 1024   # real merged caches are >1 GiB, be conservative
STUB_DYLD_MAX_BYTES = 4 * 1024 * 1024      # placeholder/"sparse" caches are tiny

MANIFEST_NAMES = ["BuildManifest.plist", "RestoreManifest.plist"]
DIGEST_KEYS = ("Digest", "FileChecksum", "PAYLOAD_DIGEST_PATH_KEY", "SHA1Digest")
PATH_KEYS = ("Path", "NativePath", "PathInImage")


def log(msg: str) -> None:
    print(f"[{PROG}]", msg)


def warn(msg: str) -> None:
    print(f"[{PROG}] WARNING:", msg, file=sys.stderr)


def sha_of_file(path: str, chunk: int = 4 * 1024 * 1024) -> Tuple[str, str]:
    """Streamed SHA1 + SHA256 of a file -> (sha1_hex, sha256_hex)."""
    h1, h2 = hashlib.sha1(), hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            data = fh.read(chunk)
            if not data:
                break
            h1.update(data)
            h2.update(data)
    return h1.hexdigest().lower(), h2.hexdigest().lower()


def find_ipsw(work: str, explicit: Optional[str]) -> str:
    if explicit:
        if not os.path.isfile(explicit):
            raise FileNotFoundError(f"--ipsw given but missing: {explicit}")
        return os.path.abspath(explicit)
    d = os.path.join(work, "ipsw")
    if not os.path.isdir(d):
        raise FileNotFoundError(
            f"no ipsw directory at {d}; run fetch_ipsh.sh --work {work} first")
    # exact canonical name first, then any *.ipsw
    exact = os.path.join(d, IPSW_DEFAULT_SUFFIX)
    if os.path.isfile(exact):
        return exact
    candidates = sorted(
        os.path.join(d, n) for n in os.listdir(d) if n.endswith(".ipsw"))
    if not candidates:
        raise FileNotFoundError(f"no .ipsw files present in {d}")
    if len(candidates) > 1:
        warn(f"multiple .ipsw files in {d}; picking {candidates[0]} (--ipsw to override)")
    return candidates[0]


def parse_plist_from_zip(zf: zipfile.ZipFile, names: List[str]) -> Tuple[str, Any]:
    for name in names:
        for member in zf.namelist():
            if member.split("/")[-1] == name:
                return member, plistlib.loads(zf.read(member))
    raise FileNotFoundError(
        f"none of {names} found in the IPSW (is this really a restore IPSW?)")


def classify_payload_dmg(
    name: str, size: int, all_sizes: Dict[str, int]
) -> Optional[str]:
    """Classify which zip member is the rootfs DMG.  Returns a role or None.

    Classification policy (reported, never trusted silently):
      * role 'rootfs'   : the largest .dmg member, expected multi-GB
      * role 'ramdisk'  : member whose filename (base, uppercase-insensitive)
        contains 'ramdisk'
      * role 'aux'      : any other .dmg (reported for operator review)
    """
    base = name.split("/")[-1].lower()
    if not base.endswith(".dmg"):
        return None
    if "ramdisk" in base:
        return "ramdisk"
    # every other dmg competes for 'rootfs' by size
    other_dmg_sizes = [
        sz for n, sz in all_sizes.items()
        if n.split("/")[-1].lower().endswith(".dmg") and n != name
        and "ramdisk" not in n.split("/")[-1].lower()
    ]
    if other_dmg_sizes and size > max(other_dmg_sizes):
        return "rootfs"
    if not other_dmg_sizes:
        return "rootfs" if size > 512 * 1024 * 1024 else "aux"
    return "aux"


def manifest_file_entries(manifest: Any) -> List[Any]:
    """Flatten per-file dicts out of BuildManifest/RestoreManifest identities.

    Supports the two shapes:
      BuildManifest: identities[].Manifest.<tag>.{Info{Path}, Digest}
      Restore/FUSION legacy: identities[].Files[] == per-file dicts
    Returns a list of dicts-ish objects (plistlib gives us raw dicts).
    """
    files: List[Any] = []
    def _walk(o: Any) -> None:
        if isinstance(o, dict):
            for key, val in o.items():
                # tag entry shape: dict containing Info{Path,...}, Digest{..}
                if isinstance(val, dict) and "Info" in val:
                    files.append(val)
                _walk(val)
        elif isinstance(o, list):
            for item in o:
                _walk(item)
    _walk(manifest)
    return files


def manifest_digest_matches(
    manifest_entries: List[Any], member_name: str
) -> Tuple[Optional[str], Optional[str]]:
    """Look up the recorded digest for a payload member.

    Returns (digest_kind, digest_hex_lower) where digest_kind is which key
    contained the match ('Digest' | 'FileChecksum' | ...), or (None, None).
    """
    for entry in manifest_entries:
        info = entry.get("Info", {}) if isinstance(entry, dict) else {}
        path = None
        for pk in PATH_KEYS:
            if isinstance(info, dict) and pk in info:
                path = os.path.basename(str(info[pk]))
                break
        if not path or path != os.path.basename(member_name):
            continue
        for dk in DIGEST_KEYS:
            val = entry.get(dk)
            if isinstance(val, bytes):
                return dk, val.hex().lower()
    return None, None


def main() -> int:
    ap = argparse.ArgumentParser(
        prog=PROG,
        description="Unpack + verify + summarise the iOS 16.5 (20F66) iPhone10,3 IPSW.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--work", default=os.path.join(os.path.expanduser("~"),
                      "hackios-begonia-work"), help="work directory (default %(default)s)")
    ap.add_argument("--ipsw", default=None, help="explicit .ipsw path (default: work/ipsw/*)")
    ap.add_argument("--skip-dmg", action="store_true",
                    help="classify + digest + summarize but skip DMG extraction")
    ap.add_argument("--force", action="store_true",
                    help="re-extract even if target files already exist")
    ap.add_argument("--quiet", action="store_true", help="suppress INFO logs")
    args = ap.parse_args()

    if args.quiet:
        def log_noop(m: str) -> None:
            pass
        # rebind for this process scope
        globals()["log"] = log_noop  # type: ignore[assignment]

    banner = (
        "=" * 70,
        f"{PROG} v{VERSION} — HackiOS-begonia userland unpack",
        f"target iOS {16.5:.1f} build {EXPECTED_BUILD} ({EXPECTED_DEVICE}, "
        f"board D22AP, chip 0x{EXPECTED_CHIPID_DEC:04x}/{EXPECTED_CHIPID_DEC})",
        "=" * 70,
    )
    for line in banner:
        print(line)

    try:
        ipsw_path = find_ipsw(args.work, args.ipsw)
    except FileNotFoundError as exc:
        print(f"[{PROG}] ERROR: {exc}", file=sys.stderr)
        return 2

    log(f"IPSW: {ipsw_path}")

    out_dir = os.path.join(args.work, "unpacked")
    hash_dir = os.path.join(out_dir, "hashes")
    os.makedirs(hash_dir, exist_ok=True)

    summary: Dict[str, Any] = {
        "schema": "hackios-begonia/build_manifest_summary/v1",
        "generated_by": f"{PROG} {VERSION}",
        "ipsw": {"path": ipsw_path, "size": os.path.getsize(ipsw_path)},
        "expected": {
            "device": EXPECTED_DEVICE,
            "build": EXPECTED_BUILD,
            "platform": EXPECTED_PLATFORM,
            "chipid_decimal": EXPECTED_CHIPID_DEC,
            "chipid_note": "0x8020 = 32800 (A11 Bionic, per docs/hardware_map_mt6785.md and plan §1)",
        },
        "manifest": {},
        "identities": [],
        "payload": [],
        "derived": {},
    }

    try:
        with zipfile.ZipFile(ipsw_path) as zf:
            manifest_member, manifest = parse_plist_from_zip(zf, MANIFEST_NAMES)
            log(f"manifest: {manifest_member}")

            product_build = str(manifest.get("ProductBuildVersion", "?"))
            product_version = str(manifest.get("ProductVersion", "?"))
            supported = [str(d) for d in manifest.get("SupportedProductTypes", []) or []]
            log(f"ProductVersion={product_version} ProductBuildVersion={product_build}")
            log(f"SupportedProductTypes={supported}")

            if product_build != EXPECTED_BUILD:
                warn(f"ProductBuildVersion={product_build} != expected {EXPECTED_BUILD}"
                     "; the operator may have supplied a different build bundle")

            summary["manifest"] = {
                "member": manifest_member,
                "ProductVersion": product_version,
                "ProductBuildVersion": product_build,
                "SupportedProductTypes": supported,
                "digest_key_used": None,
            }

            # ---- identities -------------------------------------------------
            entries = manifest_file_entries(manifest)
            identities_out = []
            for idx, ident in enumerate(manifest.get("BuildIdentities", []) or []):
                platform = str(ident.get("Platform", "?"))
                board = str(ident.get("BoardConfig", "?"))
                raw_chipid = ident.get("ApChipID", None)
                chipid_dec: Optional[int] = None
                chipid_repr_str: Any = "?"
                if isinstance(raw_chipid, bytes):
                    chipid_dec = int.from_bytes(raw_chipid, "big")
                    chipid_repr_str = f"0x{chipid_dec:04x} ({chipid_dec})"
                elif raw_chipid is not None:
                    chipid_repr_str = str(raw_chipid)
                ubid = ident.get("UniqueBuildID")
                identities_out.append({
                    "index": idx,
                    "BoardConfig": board,
                    "Platform": platform,
                    "ApChipID_hex": chipid_repr_str,
                    "ApChipID_decimal": chipid_dec,
                    "UniqueBuildID": (ubid.hex() if isinstance(ubid, bytes)
                                      else str(ubid)),
                })
                if board.upper() == "D22AP":
                    log(f"identity {idx}: target D22AP (iPhone10,3) — "
                        f"Platform={platform} ApChipID={chipid_repr_str}")
            summary["identities"] = identities_out

            # ---- BuildInfoModelNumber & friends (documented per the plan) ---
            # These keys appear in A9/every-classic-era restores: ModelNumber /
            # BuildInfoModelNumber strings are SKU-specific ("MQxx2LL/A"-style);
            # we do NOT guess them — we surface whatever the bundle carries.
            for key in ("BuildInfoModelNumber", "ModelNumber",
                        "BuildInfo", "SystemVolumeSealing"):
                if key in manifest:
                    v = manifest[key]
                    summary["manifest"][key] = (
                        v.hex() if isinstance(v, bytes) else str(v))

            # ---- classify DMG members --------------------------------------
            infos = {i.filename: i.file_size for i in zf.infolist()}
            dmg_members = [n for n in infos
                           if n.split("/")[-1].lower().endswith(".dmg")]
            if not dmg_members:
                warn("no .dmg members found — bundle shape unrecognised")
                summary["derived"]["rootfs_member"] = None
                summary["derived"]["cryptex_expected"] = False
                summary["derived"]["system_volume_sealing_expected"] = None
                summary["derived"]["notes"] = [
                    "no DMG members; A11-era bundles ship rootfs + ramdisk as DMG — "
                    "if neither is present this IPSW needs manual review"]
                _write_summary(summary, args.work)
                return 4

            # largest non-ramdisk dmg = rootfs candidate
            rootfs_member = max(
                (n for n in dmg_members if "ramdisk" not in n.lower()),
                key=lambda n: infos[n])
            log(f"rootfs DMG (largest): {rootfs_member} ({infos[rootfs_member]/1e9:.2f} GiB)")

            # cryptex / sealing inference (documented, machine-readable only)
            cryptex_flags = [n for n in infos if "cryptex" in n.lower()]
            policy_keys = [k for k in manifest.keys()
                           if "SystemVolume" in k or "LoadPolicy" in k]
            summary["derived"] = {
                "rootfs_member": rootfs_member,
                "rootfs_size": infos[rootfs_member],
                "cryptex_members": cryptex_flags,
                "cryptex_expected": False,
                "cryptex_note": (
                    "A11/iOS 16.x rootfs is the classic consolidated rootfs DMG; "
                    "system-critical cryptexes exist only for A12+ bundles. "
                    "dyld_shared_cache_arm64 lives inside the rootfs DMG at "
                    "System/Library/Caches/com.apple.dyld/."),
                "system_volume_sealing_expected": True,
                "system_volume_sealing_note": (
                    "iOS 15 and later (including A11 devices) enforce Signed "
                    "System Volume; the boot chain (iBoot/kernel) verifies the "
                    "volume seal. Our port does NOT verify any seal — see "
                    "userland/README.md §4. The APFS volume integrity (hash-"
                    "tree members like the 'disk4s1' seal) is consumed by us "
                    "as plain data; patching therefore breaks the seal by "
                    "construction (research device, accepted risk)."),
                "policy_keys_seen": policy_keys,
                "target_identity": {
                    "BoardConfig": "D22AP",
                    "ProductType": EXPECTED_DEVICE,
                    "chipid": f"0x{EXPECTED_CHIPID_DEC:04x} ({EXPECTED_CHIPID_DEC})",
                },
            }

            # ---- extract + digest ------------------------------------------
            for member in dmg_members:
                role = classify_payload_dmg(member, infos[member], infos)
                if role == "rootfs" and member != rootfs_member:
                    role = "aux"   # only one rootfs
                rec: Dict[str, Any] = {"member": member, "size": infos[member],
                                       "role": role}

                kind, expected_hex = manifest_digest_matches(
                    entries, member)
                out_path = os.path.join(out_dir, member.split("/")[-1])
                rec["extracted_path"] = out_path

                rec["manifest_digest"] = (
                    {"kind": kind, "sha1_lower": expected_hex}
                    if expected_hex else None)

                if kind and not summary["manifest"].get("digest_key_used"):
                    summary["manifest"]["digest_key_used"] = kind

                skip = os.path.exists(out_path) and not args.force
                if skip:
                    log(f"already extracted, reusing: {out_path}")
                elif not args.skip_dmg:
                    log(f"extracting {member} -> {out_path} "
                        f"({infos[member]/1e9:.2f} GiB)")
                    with zf.open(member) as src, open(out_path, "wb") as dst:
                        shutil.copyfileobj(src, dst, 8 * 1024 * 1024)
                else:
                    log(f"--skip-dmg: not extracting {member}")
                    rec["extracted"] = False
                    rec["sha1"] = None
                    rec["sha256"] = None
                    summary["payload"].append(rec)
                    continue

                sha1, sha256 = sha_of_file(out_path)
                rec["extracted"] = True
                rec["sha1"] = sha1
                rec["sha256"] = sha256
                with open(os.path.join(hash_dir,
                                       os.path.basename(member) + ".sha1"), "w") as fh:
                    fh.write(f"{sha1}  {member}\n")
                with open(os.path.join(hash_dir,
                                       os.path.basename(member) + ".sha256"), "w") as fh:
                    fh.write(f"{sha256}  {member}\n")

                if expected_hex and expected_hex != sha1:
                    rec["manifest_digest_match"] = False
                    _write_summary(summary, args.work)
                    print(
                        f"[{PROG}] ERROR: SHA1 mismatch for {member}\n"
                        f"  manifest ({kind}): {expected_hex}\n"
                        f"  computed sha1  : {sha1}\n"
                        "The IPSW is likely truncated/corrupt — re-run fetch_ipsw.sh.",
                        file=sys.stderr)
                    return 3
                rec["manifest_digest_match"] = bool(expected_hex) and expected_hex == sha1
                if expected_hex:
                    log(f"{member}: {kind} verified ({sha1[:16]}...)")

            # ---- dyld cache presence check (documented Phase-5 fact) --------
            # On A11/iOS16.x the merged cache ships in the rootfs DMG.  We can
            # only verify presence AFTER extraction; absent here = candidate
            # for ipsw/merge_rd.py from the restore ramdisk instead.
            if not args.skip_dmg:
                log("note: dyld cache presence is verified by "
                    "apply_rootfs_patches.sh (step a) after the rootfs is "
                    "mounted; ipsw/merge_rd.py merges if missing.")

            _write_summary(summary, args.work)
            log(f"summary: {os.path.join(out_dir, 'build_manifest_summary.json')}")
            log(f"rootfs DMG ready at: {os.path.join(out_dir, rootfs_member.split('/')[-1])}")
            return 0
    except zipfile.BadZipFile as exc:
        print(f"[{PROG}] ERROR: not a valid zip/IPSW: {exc}", file=sys.stderr)
        return 2


def _write_summary(summary: Dict[str, Any], work: str) -> None:
    out_dir = os.path.join(work, "unpacked")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "build_manifest_summary.json")
    with open(path, "w") as fh:
        json.dump(summary, fh, indent=2, sort_keys=False, default=str)


if __name__ == "__main__":
    sys.exit(main())
