#!/usr/bin/env python3
"""
merge_rd.py — HackiOS-begonia userland :: ipsw utility.

Verifies/merges the merged dyld shared cache for the A11 (non-cryptex)
rootfs into the prepared rootfs tree:

    <rootfs>/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64

Background (verified constraints — see userland/README.md §4):
  * iPhone10,3/iOS 16.5 is NON-cryptex: ONE merged cache, arm64, lives in
    the rootfs DMG itself. There is NO dyld_shared_cache with cryptex
    sub-volumes (those start at A12/iOS 16). So "merge" here means: check
    the rootfs copy is real (not stripped), and if it is missing or a
    placeholder, copy the real one from a candidate source directory.
  * dyld caches are NOT Mach-O files: the on-disk cache header begins with
    the ASCII magic `dyld_v1` followed by the arch tag (e.g. `dyld_v1  ...arm64`
    — 16 bytes; the format lives in dyld-4.x `mach-o/dyld_cache_format.h`).
  * A REAL iOS 16.5 arm64 system cache is ≈1.4–1.9 GiB. Rootfs DMG trees
    also (incorrectly) can be found with ~tiny "sparse"-stub files in places
    that ship placeholder content — those must be replaced, trusted never.

Candidates are extracted directory trees (already-mounted restore RAMdisks,
ramdisk payloads, or extracted auxiliary DMG trees).  We search each
candidate recursively for files whose first two magic bytes match the dyld
cache format AND contain the arch tag, and pick the biggest arm64 one.

Usage:
  merge_rd.py --rootfs DIR --candidates DIR [DIR ...] [--dry-run]
              [--report PATH] [--force]

Exit codes: 0 ok (merged or already present) · 1 usage · 2 environment ·
            4 no valid cache found in any candidate.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import stat
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

PROG = "merge_rd.py"
VERSION = "1.0"
DYLD_MAGIC_PREFIX = b"dyld_v1"
CACHE_REL = "System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64"
MIN_REAL_SIZE = 512 * 1024 * 1024        # anything smaller than this is a stub/log slice


def log(msg: str) -> None:
    print(f"[{PROG}]", msg)


def die(msg: str, code: int = 4) -> "NoReturn":  # type: ignore[valid-type]
    print(f"[{PROG}] ERROR:", msg, file=sys.stderr)
    sys.exit(code)


def read_magic(path: str) -> Tuple[str, bytes]:
    with open(path, "rb") as fh:
        head = fh.read(32)   # cache magic is 16 bytes ascii; 32 gives slack
    try:
        text = head.decode("ascii", "replace")
    except Exception:  # noqa: BLE001 — defensive, header is ascii
        text = head[:16].decode("latin-1")
    return text, head


def classify_dyld_cache(path: str) -> Optional[str]:
    """Return arch tag if the file looks like a dyld shared cache, else None."""
    text, _ = read_magic(path)
    if not text.startswith(DYLD_MAGIC_PREFIX.decode()):
        return None
    return text.strip() or "unknown"


def is_arm64_cache(text: str) -> bool:
    # magic strings seen in the wild: "dyld_v1     arm64  " / "dyld_v1  arm64 " —
    # match on the substring, never on column offsets
    return "arm64" in text and "arm64e" not in text


def find_rootfs_cache(rootfs: str) -> Optional[Dict[str, Any]]:
    target = os.path.join(rootfs, CACHE_REL)
    if not os.path.isfile(target):
        return None
    text, _ = read_magic(target)
    size = os.path.getsize(target)
    return {
        "path": target,
        "size": size,
        "magic": text.strip(),
        "looks_valid": text.startswith(DYLD_MAGIC_PREFIX.decode())
        and size >= MIN_REAL_SIZE
        and is_arm64_cache(text),
    }


def find_candidates(cand_dirs: List[str]) -> List[Dict[str, Any]]:
    found: List[Dict[str, Any]] = []
    seen = set()
    for base in cand_dirs:
        if not os.path.isdir(base):
            log(f"skip candidate (not a dir): {base}")
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            for name in filenames:
                if not name.startswith("dyld_shared_cache"):
                    continue
                full = os.path.join(dirpath, name)
                if full in seen:
                    continue
                seen.add(full)
                st = os.stat(full)
                if not (stat.S_ISREG(st.st_mode)) or st.st_size < MIN_REAL_SIZE:
                    continue   # dyld caches are GB-scale; stubs are junk
                text, _ = read_magic(full)
                if not text.startswith(DYLD_MAGIC_PREFIX.decode()):
                    continue
                arch = "arm64" if is_arm64_cache(text) else (
                    "arm64e" if "arm64e" in text else "other")
                found.append({"path": full, "size": st.st_size, "magic": text.strip(),
                              "arch": arch})
    found.sort(key=lambda r: r["size"], reverse=True)
    return found


def main() -> int:
    ap = argparse.ArgumentParser(
        prog=PROG,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description="Verify / merge the A11 (non-cryptex) arm64 dyld shared cache "
                    "into a prepared iOS 16.5 iPhone10,3 rootfs tree.",
    )
    ap.add_argument("--rootfs", required=True, help="prepared rootfs directory")
    ap.add_argument("--candidates", nargs="+", required=True,
                    help="directories to search for a source dyld cache "
                         "(extracted ramdisk trees / system volumes)")
    ap.add_argument("--report", default=None,
                    help="write a JSON merge report to PATH")
    ap.add_argument("--force", action="store_true",
                    help="overwrite an existing (even valid) rootfs cache")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    print("=" * 70)
    print(f"{PROG} v{VERSION} — dyld_shared_cache merge (A11, arm64, non-cryptex)")
    print("=" * 70)

    if not os.path.isdir(args.rootfs):
        die(f"rootfs directory missing: {args.rootfs}", 1)

    existing = find_rootfs_cache(args.rootfs)
    report: Dict[str, Any] = {
        "schema": "hackios-begonia/merge_rd/v1",
        "generated_by": f"{PROG} {VERSION}",
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "rootfs": args.rootfs,
        "existing": existing,
        "candidates_scanned": args.candidates,
        "action": None,
    }

    if existing and existing["looks_valid"] and not args.force:
        log(f"rootfs cache already present and valid: System/Library/Caches/"
            f"com.apple.dyld/dyld_shared_cache_arm64 "
            f"({existing['size']/1e9:.2f} GiB, magic={existing['magic']!r})")
        report["action"] = "already_present"
        _finish(report, args)
        return 0

    if existing and not existing["looks_valid"]:
        log(f"rootfs cache exists but looks invalid/placeholder "
            f"(size={existing['size']} bytes, magic={existing['magic']!r}) — replacing")
        if args.force is False and existing["magic"].startswith(DYLD_MAGIC_PREFIX.decode()) \
                and existing["size"] < MIN_REAL_SIZE:
            log("note: placeholder/stub cache in-place; replacing it is the intended path")

    cans = find_candidates(args.candidates)
    log(f"{len(cans)} dyld cache candidate(s) found:")
    for c in cans:
        log(f"  {c['arch']:6s} {c['size']/1e9:6.2f} GiB  {c['path']}")

    arm64_cans = [c for c in cans if c["arch"] == "arm64"]
    if not arm64_cans:
        die("no arm64 dyld_shared_cache found in candidates.\n"
            "  Mount or extract the restore RAMDisk/SystemVolume trees first\n"
            "  (see userland/README.md §5) and re-run with --candidates <dirs>.",
            4)
    src = arm64_cans[0]  # biggest arm64 cache

    dest = os.path.join(args.rootfs, CACHE_REL)
    log(f"merging: {src['path']} -> {dest}")
    log(f"  size={src['size']} ({src['size']/1e9:.2f} GiB) magic={src['magic']!r}")
    if args.dry_run:
        log("(dry run) no writes performed")
        report["action"] = "dry_run"
        _finish(report, args)
        return 0

    os.makedirs(os.path.dirname(dest), exist_ok=True)
    if os.path.exists(dest):
        if existing and os.path.isfile(dest) and not existing["looks_valid"]:
            # preserve nothing: the stub is intentionally replaced; however
            # record the old sha for the operator's audit trail
            import hashlib
            h = hashlib.sha1()
            with open(dest, "rb") as old:
                for chunk in iter(lambda: old.read(4 * 1024 * 1024), b""):
                    h.update(chunk)
            report["replaced_cache_before"] = {"sha1": h.hexdigest(),
                                               "size": existing["size"]}
        os.unlink(dest)
    shutil.copy2(src["path"], dest)
    # iOS caches are 0644, owned root:wheel — file perms copied by copy2; group ok
    os.chmod(dest, 0o644)

    report["action"] = "merged"
    report["merged_from"] = src
    _finish(report, args)
    log("merge complete")
    return 0


def _finish(report: Dict[str, Any], args: argparse.Namespace) -> None:
    if args.report:
        os.makedirs(os.path.dirname(os.path.abspath(args.report)) or ".",
                    exist_ok=True)
        with open(args.report, "w") as fh:
            json.dump(report, fh, indent=2)
        log(f"report: {args.report}")


if __name__ == "__main__":
    sys.exit(main())
