#!/usr/bin/env python3
"""
check_env.py — HackiOS-begonia tools/: PASS/FAIL host environment matrix.

Verifies presence (and, where meaningful, runs a version probe) of:

    clang (+ aarch64-apple-darwin Mach-O target support)
    lld · ldid · codesign (macOS) · dtc · mkbootimg · fastboot · adb ·
    mtk (mtkclient) · python3.8+ · git · curl ·
    mkfs.hfsplus (hfsprogs) · mkfs.vfat (dosfstools) · openssl

Output: a fixed-width PASS/FAIL table plus an optional JSON dump (--json)
so CI and other agents can consume it. Exit code is 0 only when no
CRITICAL tool is missing — tools marked critical below:

    CRITICAL (absence fails the run):
        clang · clang apple-darwin target · dtc · python3 · git
    informational (absence only warns):
        lld, ldid, codesign, mkbootimg, fastboot, adb, mtk, curl,
        mkfs.hfsplus, mkfs.vfat, openssl

(Yes — clang is critical: even `xnuloader` and the static shell build
with clang. lld/ldid/mkfs.* get installed by tools/install_*.sh and are
listable before device-time work, so they warn instead.)

Device-tied tools (mkbootimg/fastboot/adb/mtk) are intentionally
non-critical: phases 1–3 are pure host work and those binaries are
frequently absent from CI containers.

Usage:
    check_env.py [--json] [--json-out PATH] [--only clang,dtc] [--quiet]

On macOS, codesign is additionally recognized when provided via xcrun.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from typing import Any, Dict, List, Optional, Tuple

PROG = "check_env.py"
VERSION = "1.0"

OK = "PASS"
NO = "FAIL"

# Tools understood by --only, in table order below.
SUPPORTED = ("clang", "lld", "ldid", "codesign", "dtc",
             "mkbootimg", "fastboot", "adb", "mtk",
             "python3", "git", "curl",
             "mkfs.hfsplus", "mkfs.vfat", "openssl")


def probe(cmd: List[str], timeout: int = 15) -> Tuple[bool, str]:
    """Run a version probe; return (rc_ok, first informative line). Never raises."""
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = (proc.stdout + "\n" + proc.stderr).strip().splitlines()
        first = out[0].strip() if out else ""
        return proc.returncode == 0, first
    except FileNotFoundError:
        return False, "not found"
    except subprocess.TimeoutExpired:
        return False, "timeout"
    except Exception as exc:  # probe must never crash the check
        return False, f"probe error: {exc}"


def find_tool(name: str) -> Optional[str]:
    """Find a tool, including versioned variants (clang-16 .. clang-18 …)."""
    if shutil.which(name):
        return name
    for alt in (f"{name}-18", f"{name}-17", f"{name}-16", f"{name}-15"):
        if shutil.which(alt):
            return alt
    return None


def check(name: str, is_ok: bool, detail: str, critical: bool,
          used: Optional[str] = None) -> Dict[str, Any]:
    """Build one uniform result row."""
    return {"tool": name, "status": OK if is_ok else NO, "detail": detail,
            "critical": critical, "used": used}


def presence_check(name: str, critical: bool,
                   display: Optional[str] = None) -> Dict[str, Any]:
    """
    Presence-based check: PASS when the binary is on PATH; the --version
    probe is informational only (some device tools lack it on old distros).
    """
    found = find_tool(name)
    if not found:
        return check(display or name, False, "not found", critical)
    ok, first = probe([found, "--version"])
    detail = first if (ok and first) else f"present: {found}"
    return check(display or name, True, detail, critical, found)


def main() -> int:
    ap = argparse.ArgumentParser(
        prog=PROG, formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description="PASS/FAIL environment matrix for HackiOS-begonia.")
    ap.add_argument("--json", action="store_true",
                    help="also dump machine-readable results")
    ap.add_argument("--json-out", default="check_env_results.json",
                    help="where --json writes the results file")
    ap.add_argument("--only", default=None,
                    help="comma-separated tool names to check (e.g. clang,dtc)")
    ap.add_argument("--quiet", action="store_true",
                    help="no banner/table, exit code only")
    args = ap.parse_args()

    if args.only:
        only = [s.strip() for s in args.only.split(",") if s.strip()]
        unknown = [t for t in only if t not in SUPPORTED]
        if unknown:
            ap.error(f"unknown --only entries: {', '.join(unknown)} "
                     f"(supported: {', '.join(SUPPORTED)})")
        want = set(only)
    else:
        want = set(SUPPORTED)

    rows: List[Dict[str, Any]] = []

    is_mac = platform.system() == "Darwin"

    def selected(key: str) -> bool:
        return key in want

    # --- clang: critical; also probe the aarch64-apple-darwin target -------
    if selected("clang"):
        clang = find_tool("clang")
        if clang is None:
            rows.append(check("clang", False, "not found", True))
        else:
            ok, first = probe([clang, "--version"])
            apple_ok = False
            if ok:
                apple_probe = subprocess.run(
                    [clang, "-target", "aarch64-apple-darwin", "-c",
                     "-x", "c", "-", "-o", os.devnull],
                    input=b"int x;\n", capture_output=True, timeout=20)
                apple_ok = apple_probe.returncode == 0
            rows.append(check("clang", ok, first, True, clang))
            if apple_ok:
                rows.append(check("clang apple-darwin", True,
                                  "Mach-O emission ok", False, clang))
            elif ok:
                rows.append(check(
                    "clang apple-darwin", False,
                    "no Mach-O target — run tools/install_llvm_toolchain.sh, "
                    "see tools/sysroot.md", False, clang))

    # --- lld / ldid ---------------------------------------------------------
    if selected("lld"):
        lld = find_tool("lld")
        if lld:
            ok, first = probe([lld, "--version"])
            rows.append(check("lld", ok, first, False, lld))
        else:
            rows.append(check("lld", False, "not found", False))

    if selected("ldid"):
        ldid = find_tool("ldid")
        if ldid:
            ok, first = probe([ldid, "-V"])
            rows.append(check("ldid", True, first or ldid, False, ldid))
        else:
            rows.append(check("ldid", False, "not found", False))

    # --- codesign (macOS only; SKIP elsewhere — informational) --------------
    if selected("codesign"):
        if is_mac:
            cs = "codesign" if shutil.which("codesign") else (
                "xcrun codesign" if shutil.which("xcrun") else None)
            if cs:
                ok, first = probe([cs, "--help"])
                rows.append(check("codesign", True, first or cs, False, cs))
            else:
                rows.append(check("codesign", False, "not found", False))
        else:
            rows.append(check("codesign", True, "SKIP (Linux)", False))

    # --- dtc: critical (xnuloader/fdt2adt and kext DTS build steps) ---------
    if selected("dtc"):
        dtc = find_tool("dtc")
        if dtc:
            ok, first = probe([dtc, "--version"])
            rows.append(check("dtc", ok, first or dtc, True, dtc))
        else:
            rows.append(check("dtc", False, "not found", True))

    # --- device tools (mkbootimg/fastboot/adb/mtk): presence-based, warn ----
    for tool in ("mkbootimg", "fastboot", "adb"):
        if selected(tool):
            rows.append(presence_check(tool, critical=False))

    if selected("mtk"):
        mtk = shutil.which("mtk") or shutil.which("mtkclient")
        if mtk:
            ok, first = probe([mtk, "--help"])
            rows.append(check("mtk (mtkclient)", True, first or mtk, False, mtk))
        else:
            rows.append(check("mtk (mtkclient)", False, "not found", False))

    # --- python3: critical, must be >= 3.8 ----------------------------------
    if selected("python3"):
        ok = sys.version_info >= (3, 8)
        rows.append(check("python3", ok,
                          ".".join(map(str, sys.version_info[:3])), True))

    # --- git (critical) / curl (informational) -------------------------------
    for tool, crit in (("git", True), ("curl", False)):
        if selected(tool):
            found = shutil.which(tool)
            if found:
                ok, first = probe([tool, "--version"])
                rows.append(check(tool, ok, first, crit, found))
            else:
                rows.append(check(tool, False, "not found", crit))

    # --- image tooling (hfsprogs / dosfstools) --------------------------------
    for tool, desc in (("mkfs.hfsplus", "hfsprogs"),
                       ("mkfs.vfat", "dosfstools")):
        if selected(tool):
            found = shutil.which(tool)
            if found:
                rows.append(check(f"{tool} ({desc})", True, found, False, found))
            elif is_mac and tool == "mkfs.hfsplus" and shutil.which("hdiutil"):
                rows.append(check("mkfs.hfsplus (hfsprogs)", True,
                                  "hdiutil covers HFS+ on macOS", False, "hdiutil"))
            else:
                rows.append(check(f"{tool} ({desc})", False, "not found", False))

    # --- openssl: informational (only the image/manifest side uses it) -------
    if selected("openssl"):
        found = shutil.which("openssl")
        if found:
            ok, first = probe([found, "version"])
            rows.append(check("openssl", ok, first, False, found))
        else:
            rows.append(check("openssl", False, "not found", False))

    # -----------------------------------------------------------------------
    # report
    # -----------------------------------------------------------------------
    critical_missing = [r["tool"] for r in rows
                        if r["status"] == NO and r["critical"]]

    if not args.quiet:
        width = 78
        print("=" * width)
        print(f"{PROG} v{VERSION} — HackiOS-begonia host environment "
              f"({platform.system()} {platform.release()})")
        print("=" * width)
        print(f"{'TOOL':28s} {'STATUS':6s} {'CRIT':5s} DETAIL")
        print("-" * width)
        for r in rows:
            flag = "yes" if r["critical"] else "no "
            print(f"{r['tool'][:27]:28s} {r['status']:6s} {flag:5s} "
                  f"{r['detail'][:width - len('TOOL') - len('STATUS') - len('CRIT') - 6]}")
        print("=" * width)
        if critical_missing:
            print("MISSING CRITICAL:", ", ".join(critical_missing))
            print("→ apt side first:  tools/install_hosttoolchain.sh")
            print("→ clang/ld64 side: tools/install_llvm_toolchain.sh")
        else:
            print("all CRITICAL tools present — good to build")

    if args.json:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump({"schema": "hackios-begonia/check_env/v1",
                       "OS": platform.platform(),
                       "rows": rows,
                       "critical_missing": critical_missing},
                      fh, indent=2)
        if not args.quiet:
            print(f"json results: {args.json_out}")

    return 0 if not critical_missing else 1


if __name__ == "__main__":
    sys.exit(main())
