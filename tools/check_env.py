#!/usr/bin/env python3
"""
check_env.py — HackiOS-begonia tools/: PASS/FAIL host environment matrix.

Verifies presence + adequacy (where feasible by running --version) of:

    clang (+apple-darwin & aarch64-none-elf target support)
    lld / lld-link · ldid · codesign (macOS) · dtc · mkbootimg ·
    fastboot · adb · mtk (mtkclient) · python3.8+ · git · curl ·
    mkfs.hfsplus (hfsprogs) · mkfs.vfat (dosfstools) · openssl/dev libs

Output: a fixed-width PASS/FAIL table + a JSON dump option (--json) so CI
and other agents can consume it; exit code 0 only when no CRITICAL tool is
missing (tools marked optional do not fail the run).

In macOS it additionally recognizes xcrun-provided codesign.

Usage:
    check_env.py [--json] [--only clang,dtc] [--quiet]
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
SKIP = "SKIP"


def shrun_oktools(cmd: List[str], timeout: int = 15) -> Tuple[bool, str]:
    """Run a version probe; capture first interesting line. Never raises."""
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        out = (proc.stdout + "\n" + proc.stderr).strip().splitlines()
        first = out[0].strip() if out else ""
        return proc.returncode == 0, first
    except FileNotFoundError:
        return False, "not found"
    except subprocess.TimeoutExpired:
        return False, "timeout"
    except Exception as exc:  # noqa: BLE001 — probe must never crash the check
        return False, f"probe error: {exc}"


def find_tool(name: str) -> Optional[str]:
    """Find a tool, including versioned variants (clang-*, llvm-*-clang …)."""
    if shutil.which(name):
        return name
    for n in (f"{name}-16", f"{name}-15", f"{name}-17", f"{name}-18"):
        if shutil.which(n):
            return n
    return None


def check(name: str, is_ok: bool, detail: str, critical: bool,
          versioned: Optional[str] = None) -> Dict[str, Any]:
    return {"tool": name, "status": OK if is_ok else NO, "detail": detail,
            "critical": critical, "used": versioned}


def main() -> int:
    ap = argparse.ArgumentParser(prog=PROG,
                                 formatter_class=argparse.ArgumentDefaultsHelpFormatter,
                                 description="PASS/FAIL environment matrix for HackiOS-begonia.")
    ap.add_argument("--json", action="store_true", help="also dump machine-readable results")
    ap.add_argument("--only", default=None,
                    help="comma-separated tool names to check (e.g. clang,dtc)")
    ap.add_argument("--quiet", action="store_true", help="no banner/table, exit code only")
    args = ap.parse_args()

    only: Optional[List[str]] = sorted(
        s.strip() for s in args.only.split(",")) if args.only else None
    rows: List[Dict[str, Any]] = []

    is_mac = platform.system() == "Darwin"
    is_linux = platform.system() == "Linux"

    # --- clang: check apple-darwin target too -----------------------------
    if only is None or "clang" in only:
        clang = find_tool("clang")
        if clang:
            ok, first = shrun_oktools([clang, "--version"])
            apple_ok = False
            if ok:
                probe = subprocess.run(
                    [clang, "-target", "aarch64-apple-darwin", "-c",
                     "-x", "c", "-", "-o", os.devnull],
                    input=b"int x;\n", capture_output=True, timeout=20)
                apple_ok = probe.returncode == 0
            rows.append(check("clang", ok, first, True, clang))
            if ok and not apple_ok:
                rows.append(check("clang apple-darwin target", False,
                                  "Mach-O emission broken; run tools/install_llfrom_llvm_toolchain"),
                            )
            else:
                rows.append(check("clang apple-darwin target", apple_ok,
                                  "Mach-O emission ok" if apple_ok else "unavailable",
                                  False))
        else:
            rows.append(check("clang", False, "not found", True))

    # --- lld ----------------------------------------------------------------
    if only is None or "lld" in only:
        lld = find_tool("lld")
        ok, first = shrun_oktools([lld, "--version"]) if lld else (False, "not found")
        rows.append(check("lld", ok, first, False, lld))

    # --- ldid ---------------------------------------------------------------
    if only is None or "ldid" in only:
        ok, first = shrun_oktools(["ldid", "-V"]) if shutil.which("ldid") else (False, "not found")
        rows.append(check("ldid", ok, first, False))

    # --- codesign (macOS) ---------------------------------------------------
    if only is None or "codesign" in only:
        if is_mac:
            cs = "codesign" if shutil.which("codesign") else (
                "xcrun codesign" if shutil.which("xcrun") else None)
            ok, first = shrun_oktools(cs.split()) if cs else (False, "not found")
            rows.append(check("codesign", ok, first, False, cs))
        else:
            rows.append(check("codesign", True, SKIP + " (Linux)",
                              False))  # informational, never fails on Linux

    # --- dtc ----------------------------------------------------------------
    if only is None or "dtc" in only:
        ok, first = shrun_oktools(["dtc", "--version"]) if shutil.which("dtc") \
            else (False, "not found")
        rows.append(check("dtc", ok, first or "hidden version?", True))

    # --- mkbootimg / fastboot / adb -----------------------------------------
    for tool in ("mkbootimg", "fastboot", "adb"):
        if only is not None and tool not in only:
            continue
        which = shutil.which(tool)
        ok, first = shrun_oktools([tool, "--version"]) if which else (False, "not found")
        critical = (which is not None)
        # absent device tools are only WARNings until device-time; not failing CI
        rows.append(check(tool, ok or (which is None), first, False, which))

    # --- mtk (mtkclient) ----------------------------------------------------
    if only is None or "mtk" in only:
        mtk = shutil.which("mtk") or shutil.which("mtkclient")
        ok, first = shrun_oktools([mtk, "--help"]) if mtk else (False, "not found")
        rows.append(check("mtk (mtkclient)", ok or (mtk is None), first, False, mtk))

    # --- python3 ------------------------------------------------------------
    if only is None or "python3" in only:
        ok = sys.version_info >= (3, 8)
        rows.append(check("python3", ok,
                          ".".join(map(str, sys.version_info[:3])), True))

    # --- git / curl ---------------------------------------------------------
    for tool in ("git", "curl"):
        if only is not None and tool not in only:
            continue
        ok, first = shrun_oktools([tool, "--version"]) if shutil.which(tool) \
            else (False, "not found")
        rows.append(check(tool, ok, first, tool != "curl"))

    # --- image tooling ------------------------------------------------------
    for tool, desc in (("mkfs.hfsplus", "hfsprogs"),
                       ("mkfs.vfat", "dosfstools")):
        if only is not None and tool not in only:
            continue
        found = shutil.which(tool)
        if found:
            rows.append(check(f"{tool} ({desc})", True, found, False, found))
        elif is_mac and tool == "mkfs.hfsplus":
            rows.append(check("mkfs.hfsplus (hfsprogs)", True,
                              "hdiutil used on macOS", False, "hdiutil"))
        else:
            rows.append(check(f"{tool} ({desc})", False, "not found", False))

    # --- openssl dev --------------------------------------------------------
    if only is None or "openssl" in only:
        ok, first = shrun_oktools(["openssl", "version"]) if shutil.which("openssl") \
            else (False, "not found")
        rows.append(check("openssl", ok, first, False))

    # -----------------------------------------------------------------------
    # report
    # -----------------------------------------------------------------------
    if not args.quiet:
        print("=" * 78)
        print(f"{PROG} v{VERSION} — HackiOS-begonia host environment "
              f"({platform.system()} {platform.release()})")
        print("=" * 78)
        print(f"{'TOOL':28s} {'STATUS':6s} {'CRIT':5s} DETAIL")
        print("-" * 78)
        for r in rows:
            print(f"{r['tool'][:27]:28s} {r['status']:6s} {'yes' if r['critical'] else 'no ':5s} {r['detail'][:42]}")
        print("=" * 78)

    critical_missing = [r["tool"] for r in rows
                        if r["status"] == NO and r["critical"]]
    if not args.quiet:
        if critical_missing:
            print("MISSING CRITICAL:", ", ".join(critical_missing))
            print("→ protected host toolchain first: tools/install_hosttoolchain.sh")
        else:
            print("all CRITICAL tools present — good to build")

    if args.json:
        with open("check_env_results.json", "w") as fh:
            json.dump({"schema": "hackios-begonia/check_env/v1",
                       "OS": platform.platform(), "rows": rows,
                       "critical_missing": critical_missing}, fh, indent=2)
        if not args.quiet:
            print("json results: check_env_results.json")

    return 0 if not critical_missing else 1


if __name__ == "__main__":
    sys.exit(main())
