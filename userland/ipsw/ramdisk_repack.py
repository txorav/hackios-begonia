#!/usr/bin/env python3
"""
ramdisk_repack.py — HackiOS-begonia userland :: ipsw utility (M4 helper).

Builds the secondary M1..M4-boot-test ramdisk IMAGE (a plain filesystem
image file) that the boot chain references via the ADT:

    /chosen/memory-map key `RAMDisk` -> reg = <phys-base, size>
    XNU (verified in-tree, xnu/iokit/bsddev/IOKitBSDInit.cpp:766)
    turns that region into /dev/mdx via mdevadd(). Boot args use
    `rd=md0 rootdev=md0`, and PID 1 is exec'd from it — XNU tries
    `/sbin/launchd` (xnu/bsd/kern/kern_exec.c:7344, init_programs[]).

Target formats (choose with --format):
  hfsplus — default; built with hfsprogs' mkfs.hfsplus. XNU mounting of HFS+
            for the ramdisk requires a filesystem kext on the kernel side
            (HFS/APFS are NOT in open-source xnu-8796 — the closed-source
            Apple hfs.kext/apfs.kext were pulled from the open tree; our
            LxStorage/kernel port owns the mount). Made on Linux host tools.
  vfat    — simplest possible format if the kernel side lands a minimal
            FAT kext first (common bring-up shortcut).

File population is best-effort:
  * Linux: sudo-less attempt is impossible (mount needs privileges); with
    root we loop-mount and copy, otherwise we print instructions.
  * macOS: `hdiutil attach -mountpoint` + ditto/cp -a + detach.

The static shell to put inside is built via ../ramdisk/static_shell/build.sh
and must be placed at BOTH /sbin/launchd (PID1 exec target, note the caveat
in ../ramdisk/README.md §3) and /bin/bash or the shell of your choice.

Usage:
  ramdisk_repack.py --out FILE.img [--size-mb N] [--format hfsplus|vfat]
                    [--tree DIR] [--gzip] [--label L] [--dry-run]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from typing import List, Optional

PROG = "ramdisk_repack.py"
VERSION = "1.0"
DEFAULT_SIZE_MB = 256     # roomy: static shell + loader dsc + logs


def log(msg: str) -> None:
    print(f"[{PROG}]", msg)


def die(msg: str, code: int = 4) -> None:
    print(f"[{PROG}] ERROR:", msg, file=sys.stderr)
    sys.exit(code)


def have(cmd: str) -> bool:
    return shutil.which(cmd) is not None


def build_hfsplus(out: str, size_mb: int, label: str) -> None:
    if have("mkfs.hfsplus"):
        # hfsprogs semantics (verified on this host):
        #   mkfs.hfsplus [-v label] SPECIAL-DEVICE
        #   - the "device" IS our image file; NO trailing size arg
        #   - -s means case-SENSITIVE (not size!) — do not pass it
        #   - non-journaled by default (required for Linux loop-mount rw)
        log(f"pre-creating sparse image file {out} ({size_mb} MiB)")
        with open(out, "wb") as fh:
            fh.truncate(size_mb * 1024 * 1024)
        log(f"mkfs.hfsplus: -v {label!r} {out} (non-journaled HFS+)")
        proc = subprocess.run(["mkfs.hfsplus", "-v", label, out], check=False,
                              capture_output=True, text=True)
        if proc.returncode != 0:
            die(f"mkfs.hfsplus failed rc={proc.returncode}:\n"
                f"  {proc.stdout.strip()}\n  {proc.stderr.strip()}")
        if proc.stderr.strip():
            log(proc.stderr.strip())
    elif have("hdiutil"):
        log("hdiutil (macOS) path: create + later attach is the population step")
        subprocess.run(["hdiutil", "create", "-size", f"{size_mb}m", "-fs",
                        "HFS+", "-volname", label, out], check=False)
    else:
        die("neither mkfs.hfsplus (Debian/Ubuntu: apt install hfsprogs) nor\n"
            "  hdiutil (macOS) found. Install hfsprogs and retry.")


def build_vfat(out: str, size_mb: int, label: str) -> None:
    if not have("mkfs.vfat"):
        die("mkfs.vfat not found (DOSFSTOOLS package; may need dosfstools install)")
    # mkfs.vfat SIZE arg is in KiB blocks (sectors when -S? classic semantics: FILE)
    log(f"mkfs.vfat: creating FAT32 image {out} ({size_mb} MiB, label {label!r})")
    subprocess.run(["mkfs.vfat", "-F", "32", "-n", label,
                    "-C", out, str(size_mb * 1024)], check=True)


def populate(image: str, tree: Optional[str], fs: str, os_name: str) -> None:
    MNT = "/mnt/hackios-ramdisk"
    if tree is None:
        log("no --tree given: image left empty (populate manually)")
        return
    if os_name == "Darwin" and have("hdiutil"):
        log(f"hdiutil attach → {MNT}; copying {tree}")
        rc = subprocess.run(["hdiutil", "attach", "-mountpoint", MNT,
                             "-nobrowse", image], check=False).returncode
        if rc != 0:
            die("hdiutil attach failed — populate manually")
        try:
            subprocess.run(["cp", "-a", f"{tree}/.", MNT], check=False)
        finally:
            subprocess.run(["hdiutil", "detach", MNT], check=False)
    elif os_name == "Linux" and have("mount") and os.geteuid() == 0:
        log(f"loop-mount ({fs}) {image} → {MNT}; copying {tree}")
        os.makedirs(MNT, exist_ok=True)
        try:
            subprocess.run(["mount", "-t", fs, "-o", "loop", image, MNT],
                           check=True)
            subprocess.run(["cp", "-a", f"{tree}/.", MNT], check=False)
            subprocess.run(["sync"], check=False)
        finally:
            subprocess.run(["umount", MNT], check=False)
    else:
        log("[manual step required] (no privileges or platform mounter found):")
        log(f"  {os_name} familiarity: run as root:\n"
            f"    mount -t {fs} -o loop {image} {MNT}\n"
            f"    cp -a {tree!r}/. {MNT}/\n"
            f"    umount {MNT}\n"
            f"  (Linux: rw HFS+ requires the hfsplus module and a NON-journaled\n"
            f"   volume; vfat is simplest for bring-up shells)")


def finalize(root: str, out: str, do_gzip: bool) -> None:
    if do_gzip:
        gz = out + ".gz"
        if have("gzip"):
            log(f"gzip → {gz} (note: xnuloader-to-memory decompressor support must "
                "match the packaging format used by mkxnuimg.py)")
            subprocess.run(["gzip", "-k", "-f", out], check=False)
        else:
            die("gzip not found; cannot compress image")


def main() -> int:
    ap = argparse.ArgumentParser(
        prog=PROG, formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description="Build the M4 secondary ramdisk image (HFS+/FAT) for "
                    "HackiOS-begonia early-boot tests.")
    ap.add_argument("--out", required=True, help="output image path")
    ap.add_argument("--size-mb", type=int, default=DEFAULT_SIZE_MB,
                    help="size in MiB (default %(default)s)")
    ap.add_argument("--format", default="hfsplus", choices=("hfsplus", "vfat"))
    ap.add_argument("--label", default="HACKIOSRD")
    ap.add_argument("--tree", default=None, help="directory to copy into the image")
    ap.add_argument("--gzip", action="store_true", help="also produce <out>.gz")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    print("=" * 70)
    print(f"{PROG} v{VERSION} — M4/HackiOS-begonia secondary ramdisk builder")
    print("=" * 70)
    print(f"  out={args.out} size={args.size_mb}MiB format={args.format}")

    if args.dry_run:
        log("(dry run) no filesystem tools invoked")
        return 0

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    if args.format == "hfsplus":
        build_hfsplus(args.out, args.size_mb, args.label)
    else:
        build_vfat(args.out, args.size_mb, args.label)

    # populate only if a tree was given (avoid mount dances on dry placeholders)
    if args.tree:
        populate(args.out, args.tree, args.format, os.uname().sysname)
    else:
        log("no --tree: image created EMPTY. Populate with the paths listed in")
        log("  ../ramdisk/README.md ( * `/sbin/launchd` (PID1) and `/bin/bash`)")

    finalize(args.out, args.out, args.gzip)
    log("RAMDisk contract (for xnuloader + ADT, docs/hardware_map_mt6785.md §3):")
    log("  ADT /chosen/memory-map entry `RAMDisk`, reg = <phys, size>")
    log("  → XNU mdevadd() exposes it as /dev/md0; boot args rd=md0 rootdev=md0")
    log("  → PID1 = /sbin/launchd (init_programs[], kern_exec.c:7344)")
    log(f"done: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
