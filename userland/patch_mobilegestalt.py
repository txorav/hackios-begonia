#!/usr/bin/env python3
"""
patch_mobilegestalt.py — HackiOS-begonia userland :: Phase 5 item 4 (d).

Builds and installs a MobileGestalt answer cache so libMobileGestalt reports
the device we are masquerading as:

    iPhone10,3 · board D22AP · chip-id 0x8020 (decimal **32800**) · A11 Bionic

Key facts this script encodes (verified against
https://theapplewiki.com/wiki/List_of_MobileGestalt_keys, fetched 2026-10):

  * The on-disk cache (`/var/mobile/Library/Caches/com.apple.MobileGestalt.plist`
    on-device) stores answers under OBFUSCATED KEY HASHES inside a top-level
    `CacheExtra` dict — NOT under plain names. libMobileGestalt hashes the
    plain key and looks it up.  We therefore write both:
      - `CacheExtra` with the REAL hashed keys (functional), and
      - a `hackios-plain-names` documentation dict (ignored by MG).
  * `ChipID` is "The CPID of the SoC, **as an integer**" → 32800 (0x8020).
    (If a component misparses an integer, --chipid-repr hexstring stores "8020".)
  * `HwModel`/`MainScreenPitch` (names used in earlier project notes) do NOT
    exist in MobileGestalt. The REAL keys are:
      - HardwareModel string → `HWModelStr`  ("D22AP"),
      - device identifier    → `ProductType` ("iPhone10,3"),
      - screen (iOS7-era, kebab) → `main-screen-width/-height/-pitch/-scale`.
    We provide all of the real keys; the non-existent names are documented,
    not written (nonsense keys would be dead weight at best, landmines at worst).
  * HardwarePlatform value for A11 devices is the SoC platform string "t8020".
  * Boot screen keys use iPhone10,3 canonical numbers (1125×2436 @3x, 458 ppi)
    by DEFAULT because SpringBoard/backboardd layout assets are keyed to it.
    begonia's panel is 1080×2340 @ ~395 ppi — SAME 19.5:9 aspect ratio, so a
    3.0 scale render (360×780 logical) is the correct, letterbox-free real
    canvas.  --screen-mode begonia_true switches to true panel numbers.

Paths handled:
  {root}/Library/Preferences/com.apple.MobileGestalt.plist        [macOS-style]
  {root}/private/var/mobile/Library/Caches/com.apple.MobileGestalt.plist  [iOS]
(The first is written only if {root}/Library exists; the second only if
{root}/private/var already exists — the stock rootfs DMG ships neither /var
nor data partitions, in which case see the --staging output instead.)

Usage:
  patch_mobilegestalt.py --root DIR [--out FILE] [--staging DIR] [--all-targets]
                        [--serial S] [--ecid ECID] [--udid UDID]
                        [--screen-mode iphone10_3|begonia_true]
                        [--chipid-repr int|hexstring] [--binary]
                        [--extra KEY=VALUE ...] [--dry-run] [--validate]
"""

from __future__ import annotations

import argparse
import hashlib
import os
import plistlib
import sys
from typing import Any, Dict, List, Optional

PROG = "patch_mobilegestalt.py"
VERSION = "1.0"

# ---------------------------------------------------------------------------
# Canonical identity (begonia masquerading as iPhone10,3 per the plan §3/§5)
# ---------------------------------------------------------------------------
PRODUCT_TYPE = "iPhone10,3"      # iPhone X (A1901 global SKU)
HW_MODEL_STR = "D22AP"           # board-level hardware model
HARDWARE_PLATFORM = "t8020"      # A11 Bionic platform string (MG HardwarePlatform)
CHIP_ID_DEC = 0x8020             # = 32800  (ChipID, integer)
CHIP_ID_HEX_STR = "8020"
IOS_VERSION = "16.5"
IOS_BUILD = "20F66"

SCREEN_MODES = {
    # mode          (width, height, pitch(ppi), scale)
    "iphone10_3":   (1125, 2436, 458, 3.0),   # canonical iPhone10,3 numbers
    # begonia's real panel, MT6785 1080x2340 IPS; scale 1.0 (documents the
    # unfaked numbers; expect cosmetic layout artifacts in UIKit clients)
    "begonia_true": (1080, 2340, 395, 1.0),
}

DEFAULT_SERIAL = "H0CK1OS00000"          # synthetic, 12-char Apple-format-style
DEFAULT_ECID = "0x0051339B1AC4"          # synthetic, NOT production-representative

# ---------------------------------------------------------------------------
# Verified hashed-key map  (source: The Apple Wiki "List of MobileGestalt keys")
# ---------------------------------------------------------------------------
HASHED: Dict[str, str] = {
    "ProductType":        "h9jDsbgj7xIVeIQ8S3/X3Q",
    "HWModelStr":         "/YYygAofPDbhrwToVsXdeA",
    "HardwarePlatform":   "5pYKlGnYYBzGvAlIU8RjEQ",
    "ChipID":             "566JrJVMlDfnslGpwUzNlQ",
    "CPUArchitecture":    "k7QIBwZJJOVw+Sej/8h8VA",
    "CPUType":            "xZm4Ky2/qswyf7ykvlchYg",
    "CPUSubType":         "AWEfP3xWh0q1MjO3JAy/AA",
    "DeviceClass":        "+3Uf0Pm5F8Xy7Onyvko0vA",
    "DeviceName":         "rkqlwPcRHwixY4gapPjanw",
    "BoardId":            "0uthiXrHZ212KvcJizKHEw",
    "ProductVersion":     "qNNddlUK+B/YlooNoymwgA",
    "BuildVersion":       "mZfUC7qo4pURNhyMHZ62RQ",
    "SerialNumber":       "VasUgeSzVyHdB27g2XpN0g",
    "UniqueChipID":       "TF31PAB6aO8KAbPyNKSxKA",
    "UniqueChipIDString": "6WdGcQGw4VLzrgxJo+bcrw",
    "UniqueDeviceID":     "re6Zb+zwFKJNlkQTUeT+/w",
    "DieId":              "Z/TJS0IaLyq10dcSvUTGPg",
    "main-screen-width":    "g7YQ1Djxh4YiKlEeaoGhzg",
    "main-screen-height":   "OjzOua0LkOegX7pQdgMksw",
    "main-screen-pitch":    "0l4wqBtWEAK1tOkeBHkU6Q",
    "main-screen-scale":    "SNfDJgQFV2Xj7+WnozcJPw",
    "main-screen-class":    "fdh+s6j3VijuyrK7xLjd7g",
    "main-screen-orientation": "1qDdT/85SS7sxriK0wIbbg",
    "64-bit":             "V2Ykm/0M3CA6nyNhwNInsg",
    "arm64":              "kKgJsWN/rBUAkimOtm/wbA",
}

# Key names from earlier project notes that DO NOT exist in MobileGestalt.
NONEXISTENT_KEYS_NOTE = {
    "HwModel":            "use HWModelStr / ProductType instead (see module docstring)",
    "MainScreenPitch":    "use main-screen-pitch (kebab, iOS7-era) instead",
    "MainScreenWidth":    "use main-screen-width (kebab, iOS7-era) instead",
}


def dereference_udid(serial: str, ecid_hex: str) -> str:
    """Deterministic synthetic UDID (40 hex digits, SHA1-based).

    Real UDIDs bind to SEP-protected identifiers; ours is a stable placeholder
    derived from the operator-supplied serial + ECID so that app install paths
    that key on UDID form-correctly. Documented as synthetic.
    """
    base = f"hackios-begonia:{serial}:{ecid_hex}:iPhone10,3"
    return hashlib.sha1(base.encode()).hexdigest().upper()


def build_answers(args: argparse.Namespace) -> Dict[str, Any]:
    width, height, pitch, scale = SCREEN_MODES[args.screen_mode]

    chipid_value: Any
    if args.chipid_repr == "int":
        chipid_value = CHIP_ID_DEC
    else:
        chipid_value = CHIP_ID_HEX_STR

    serial = args.serial
    ecid = args.ecid
    udid = args.udid or dereference_udid(serial, ecid)

    answers: Dict[str, Any] = {
        "ProductType": PRODUCT_TYPE,
        "HWModelStr": HW_MODEL_STR,
        "HardwarePlatform": HARDWARE_PLATFORM,
        "ChipID": chipid_value,
        "CPUArchitecture": "arm64",
        "CPUType": 16777228,             # 0x0100000C = CPU_TYPE_ARM64 (Mach-O)
        "DeviceClass": "iPhone",
        "DeviceName": "iPhone X",
        "BoardId": HW_MODEL_STR,         # MG BoardId carries board string
        "ProductVersion": IOS_VERSION,
        "BuildVersion": IOS_BUILD,
        "SerialNumber": serial,
        "DieId": ecid,
        "UniqueChipIDString": ecid.replace("0x", "").upper(),
        "UniqueDeviceID": udid,
        "64-bit": True,
        "arm64": True,
        "main-screen-width": width,
        "main-screen-height": height,
        "main-screen-pitch": pitch,
        "main-screen-scale": scale,
        "main-screen-orientation": 0,    # portrait (Portrait natural on iPhone)
    }
    # operator extras: plain-key syntax; mapped to hashed key when known
    for extra in args.extra or []:
        if "=" not in extra:
            raise ValueError(f"--extra expects KEY=VALUE, got {extra!r}")
        key, value = extra.split("=", 1)
        if value.lower() == "true":
            value = True          # type: ignore[assignment]
        elif value.lower() == "false":
            value = False         # type: ignore[assignment]
        else:
            try:
                value = int(value)  # type: ignore[assignment]
            except ValueError:
                pass
        answers[key] = value

    return answers


def build_plist(answers: Dict[str, Any], plain_doc: bool) -> Dict[str, Any]:
    """Real MG cache shape: {CacheExtra: {<hashed-key>: value}, ...}."""
    cache_extra: Dict[str, Any] = {}
    for plain, value in answers.items():
        h = HASHED.get(plain)
        if h is None:
            # unknown key — MG ignores unknown CacheExtra entries; keep it for
            # operator review and so --validate round-trips it.
            cache_extra[plain] = value
            continue
        cache_extra[h] = value
    out: Dict[str, Any] = {
        "CacheExtra": cache_extra,
        "hackios": {
            "built_by": f"{PROG} {VERSION}",
            "device": "Xiaomi Redmi Note 8 Pro (begonia), MT6785 (Helio G90T)",
            "masquerade": {"ProductType": PRODUCT_TYPE, "BoardId": HW_MODEL_STR,
                           "ChipID": f"0x8020 (= {CHIP_ID_DEC})"},
            "plain_names_for_review": dict(answers),
            "nonexistent_keys_note": NONEXISTENT_KEYS_NOTE,
            "screen_mode": SCREEN_MODES,  # tuples -> plist arrays automatically
        },
    }
    if not plain_doc:
        del out["hackios"]["plain_names_for_review"]
        del out["hackios"]["nonexistent_keys_note"]
        del out["hackios"]["screen_mode"]
        out["hackios"] = {"built_by": out["hackios"]["built_by"],
                          "masquerade": out["hackios"]["masquerade"]}
    return out


def target_files(root: str, all_targets: bool) -> List[str]:
    targets: List[str] = []
    root_lib = os.path.join(root, "Library", "Preferences")
    var_path = os.path.join(root, "private", "var", "mobile",
                            "Library", "Caches")
    if os.path.isdir(root_lib):
        targets.append(os.path.join(root_lib, "com.apple.MobileGestalt.plist"))
    if all_targets and os.path.isdir(var_path):
        targets.append(os.path.join(var_path, "com.apple.MobileGestalt.plist"))
    return targets


def lint() -> int:  # self-check helper (used by tests / --validate)
    args = argparse.Namespace(
        serial=DEFAULT_SERIAL, ecid=DEFAULT_ECID, udid=None,
        screen_mode="iphone10_3", chipid_repr="int", extra=["X=y"],
    )
    answers = build_answers(args)
    assert answers["ChipID"] == CHIP_ID_DEC, "ChipID must be 32800 (0x8020)"
    assert answers["CPUType"] == 0x0100000C
    assert dereference_udid(DEFAULT_SERIAL, DEFAULT_ECID) != ""
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        prog=PROG,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
        description=__doc__.splitlines()[1],
    )
    ap.add_argument("--root", default=None, help="rootfs directory (mount or tree)")
    ap.add_argument("--out", default=None, help="write built plist to FILE")
    ap.add_argument("--staging", default=None,
                    help="directory to receive com.apple.MobileGestalt.overlay.plist")
    ap.add_argument("--all-targets", action="store_true",
                    help="also write the on-device /var/mobile path if present in root")
    ap.add_argument("--serial", default=DEFAULT_SERIAL)
    ap.add_argument("--ecid", default=DEFAULT_ECID)
    ap.add_argument("--udid", default=None)
    ap.add_argument("--screen-mode", default="iphone10_3",
                    choices=sorted(SCREEN_MODES))
    ap.add_argument("--chipid-repr", default="int", choices=("int", "hexstring"),
                    help="ChipID as integer 32800 or string \"8020\"")
    ap.add_argument("--binary", action="store_true",
                    help="write binary plist (real on-device caches are bplist00)")
    ap.add_argument("--no-plain-doc", dest="plain_doc", action="store_false",
                    help="omit the operator-geared review dict from the plist")
    ap.add_argument("--extra", action="append", default=[], metavar="KEY=VALUE",
                    help="add a key (hashed when known, plain otherwise)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--validate", action="store_true", help="self-check and exit")
    args = ap.parse_args()

    if args.validate:
        print(f"[{PROG}] self-check ok")
        return lint()

    banner_lines = (
        "=" * 70,
        f"{PROG} v{VERSION} — MobileGestalt answer cache builder",
        f"identity: {PRODUCT_TYPE} / {HW_MODEL_STR} / chip 0x8020 ({CHIP_ID_DEC}, A11)",
        f"screen  : {args.screen_mode} "
        f"{SCREEN_MODES[args.screen_mode][:3]} @x{SCREEN_MODES[args.screen_mode][3]}",
        "=" * 70,
    )
    for line in banner_lines:
        print(line)

    try:
        answers = build_answers(args)
    except ValueError as exc:
        print(f"[{PROG}] ERROR: {exc}", file=sys.stderr)
        return 1

    plist = build_plist(answers, args.plain_doc)
    fmt = plistlib.FMT_BINARY if args.binary else plistlib.FMT_XML
    payload = plistlib.dumps(plist, fmt=fmt)

    written: List[str] = []

    if args.out:
        if not args.dry_run:
            os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
            with open(args.out, "wb") as fh:
                fh.write(payload)
        written.append(f"--out: {args.out}")
    if args.staging:
        if not args.dry_run:
            os.makedirs(args.staging, exist_ok=True)
            with open(os.path.join(args.staging,
                                   "com.apple.MobileGestalt.overlay.plist"), "wb") as fh:
                fh.write(payload)
        written.append(f"--staging: {args.staging}")
    if args.root:
        for target in target_files(args.root, args.all_targets):
            if not args.dry_run:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                if os.path.exists(target):
                    backup_path = target + ".hackios-orig"
                    if not os.path.exists(backup_path):
                        with open(target, "rb") as old_fh:
                            old_bytes = old_fh.read()
                        with open(backup_path, "wb") as bk_fh:
                            bk_fh.write(old_bytes)
                with open(target, "wb") as fh:
                    fh.write(payload)
            written.append(f"root: {target}")

    if not written and not args.dry_run:
        print(
            f"[{PROG}] NOTE: no installable target existed under --root "
            f"{args.root!r} (stock rootfs DMG has neither /Library nor /private/var).\n"
            "  → use --staging DIR (or --out FILE) and push the produced plist to\n"
            "    /var/mobile/Library/Caches/ on-device at first boot\n"
            "    (see userland/ipsw/LayoutNotes.md).",
            file=sys.stderr)
        return 3

    for w in written:
        print(f"[{PROG}] wrote: {w}")
    if args.dry_run:
        print(f"[{PROG}] (dry run) plist would be {len(payload)} "
              f"({'BINARY' if args.binary else 'XML'}) bytes")
    else:
        print(f"[{PROG}] plist size: {len(payload)} bytes "
              f"({'binary' if args.binary else 'XML'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
