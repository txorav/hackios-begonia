# HackiOS-begonia — Userland bootstrap (Phase 5)

**Scope owner:** `userland/**` (also see `tools/` and `uefi/` in their own READMEs).
Everything here runs **on the operator's host** (Linux assumed; macOS notes where they
differ). No device-side build is required.

---

## 1. What this produces, end to end

Phase 5 of the porting plan
(`hackios_porting_plan_for_redmi_note_8_pro_begonia.md` §Phase 5, item 1) is:
*Extract & prepare the iPhone10,3 iOS 16.5 root filesystem so it can boot on
begonia's XNU port.* Concretely:

1. Fetch the iOS 16.5 **full-restore IPSW** for iPhone10,3
   (`iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw`, Build **20F66**).
2. Unpack the IPSW (it is a zip): pull the **root filesystem DMG** (the largest DMG),
   the restore ramdisk, and `BuildManifest.plist`/`Restore.plist`; verify SHA digests
   and write a machine-readable summary (`build_manifest_summary.json`).
3. Mount/copy the root FS to a writable workspace, then apply our patches:
   * `dyld_shared_cache_arm64` present/merged (A11 = **non-cryptex** architecture),
   * `/etc/fstab` → `rw` (see `patches/fstab`),
   * MobileGestalt answer cache (see `patch_mobilegestalt.py` + `patches/`),
   * `/var/db` seed bookkeeping,
   * recorded patch manifest (`patch_manifest.json`) so nothing is silent.
4. Verify the tree (checksums, device identity strings, dyld cache presence).
5. Hand the prepared rootfs to the device-side boot flow: `mkxnuimg.py` (root repo)
   packages `xnuloader + ADT + kernelcache + ramdisk` into `boot.img` and `xl`
   streams/logs over USB/serial. For M5, the full rootfs is staged on UFS
   (`ufshcd-mtk` @ `0x11270000`) or MicroSD (`msdc1` @ `0x11240000`).

Everything is **offline-first after the IPSW is fetched**: no step here requires
network access except `fetch_ipsw.sh` and `tools/install_*.sh`.

---

## 2. Canonical versions & hardware (do not drift)

These values are copied from `docs/hardware_map_mt6785.md` (canonical hardware
contract) and the porting plan §1. If something changes, update the hardware doc
**first**, then reflect here.

| Item | Value |
| --- | --- |
| Target device | Xiaomi Redmi Note 8 Pro, codename **begonia** |
| SoC | MediaTek **Helio G90T (`MT6785V/CC`)**, `mt6785` |
| ISA | **ARMv8.2-A** — no PAC (`arm64e`), no APRR/PPL, no Apple HID/HID16, no AMX/CPM, no Apple Fast-IPI |
| CPU climbs | 2 × Cortex-A76 @ 2.05 GHz (`MPIDR 0x100/0x101`) + 6 × Cortex-A55 @ 2.00 GHz (`0x000..0x005`) |
| MMU granule used by XNU | **16 KiB** (CPU supports 4/16/64 KiB) |
| Source iOS | **iOS 16.5, Build 20F66** (iPhone X = A11 Bionic, final full `arm64` non-PAC userland) |
| Simulated device identity | **iPhone10,3**, board **D22AP**, chip-id **`0x8020`** (= decimal **32800**, A11 Bionic class per hardware-map §3 `/chosen` `chip-id`) |
| CPU architecture string | `arm64` (MG `CPUArchitecture`), Mach-O `CPUType = 0x0100000C` masquerade as Apple A11 |
| RAMDisk | HFS+/flat image, referenced by ADT `/chosen/memory-map` key `RAMDisk`, becomes `/dev/md0` (`rd=md0 rootdev=md0` boot args) — see `ramdisk/README.md` |
| Boot flow | `lk (mtk bootloader) → boot.img → xnuloader (0x40080000) → XNU -> beta root file system |

**Host-side expectations** (the scripts check): `python3 (>=3.8)`, `curl`, `shell`
bash `getopts` (` getopt` non-GNU variant not needed). See `tools/check_env.py`.

---

## 3. Work directory layout (what this repo generates)

Default work dir: `~/hackios-begonia-work` — all sub-scripts accept `--work`.
The tree is completely operator-owned and disposable; the repo never depends on
anything in it.

```
hackios-begonia-work/
├── ipsw/
│   ├── iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw     # fetched by fetch_ipsw.sh
│   └── *.ipsw.sha256 / .sha1                             # sidecar we compute
├── unpacked/                     # produced by unpack_ipsw.py
│   ├── build_manifest_summary.json
│   ├── rootfs.dmg              # the largest DMG (root filesystem, APFS)
│   ├── ramdisk.dmg/RestoreRamDisk.dmg (whatever the bundle actually has)
│   ├── <everything else the zip had, listed in unpack_report.json>
│   └── hashes/irrelevant/...     # sha1/sha256 sidecars per extracted file
├── mount/                        # mount point / copied tree
│   └── rootfs/                   # either mounted DMG or extracted file tree
├── patched/                      # produced by apply_rootfs_patches.sh
│   ├── rootfs_patched/           # patched copy (copy-on-first-patch)
│   └── patch_manifest.json      # every mutation + sha before/after
└── package/
    └── rootfs-stage.tar           # transport image for device-side staging
```

> **Why "patched copy" rather than patching in place:** desktop APFS write tools
> on Linux are not trustworthy enough for a 4 GiB root image (fusion
> `apfs-fuse` is read-only-ish). Patching is done on an **extracted file tree**
> (fully real and robust) *or* on a **read-write mount from macOS** via
> `hdiutil`; both paths end in the same `patch_manifest.json`.

---

## 4. SSV (Signed/Sealed System Volume) & what "patched" vs "not" means

This is the section people usually get wrong — read carefully.

* Stock iOS 16.5 on A11 (and everything that runs iOS ≥ 15) uses **Signed System
  Volume (SSV)**: the root volume hash (a Merkle tree/`ISYS`-family seal) is
  checked by Apple's boot chain (iBoot → kernel) — not by open-source XNU code.
  On A11 the enforcement landed with iOS 15.0 (earlier releases had the
  plumbing but didn't enforce it).
* **Our port breaks the SSV seal, by construction.** XNU 8796.121.2 (open
  source) does **not** verify any volume seal, and neither does our platform
  kext set (`kexts/`, owned by kernel-port). We consume the rootfs DMG as plain
  data, so patched files violate a seal we never check.
* Just for completeness — stock-immutability layers on iPhone10,3/iOS 16.5:
  1. boot-chain seal verification (we bypass — see above),
  2. the root volume system snapshot mounted **read-only** and the
     `/private/etc/fstab` of stock iOS which we **replace** with a `rw`
     variant (see `patches/fstab` and `apply_rootfs_patches.sh` step (b)),
  3. kernel **code signing** (`cs_enforcement_disable=1` boot-arg, AMFI-stub
     kext — kernel-port's scope, `kexts/AMFIStub`),
  4. **not cryptexes**: A11/iOS 16.5 does *not* use the (A12+) cryptex scheme;
     the single `dyld_shared_cache_arm64` comes straight from the rootfs.

### Security posture of the result (why this is a research device)

With `cs_enforcement_disable=1` + an AMFI stub + patched fstab + SSV-bypassed,
**any code runs as root with no signature checks**. That is exactly right for
bring-up (unmodified iOS binaries and `ldid -S`-dummy-signed binaries must
co-exist) and *completely wrong* for any everyday use. All of this stays on the
bench; the porting plan's Milestone M5/M6 checklist is the gate.

---

## 5. The pipeline in commands

```bash
# 0. Sanity-check the host (optional but recommended)
../../tools/check_env.sh

# 1. Fetch the IPSW (network; -C resumable; supports --dry-run)
./fetch_ipsw.sh --work ~/hackios-begonia-work

# 2. Unpack + digest + summarize
./unpack_ipsw.py --work ~/hackios-begonia-work

# 3. Make the rootfs writable & patch it:
#    (a) mount the extracted DMG (choices: macOS hdiutil / Linux apfs-markfs,
#        or offline extract-tree mode) into mount/rootfs
#    (b) run:
./apply_rootfs_patches.sh --work ~/hackios-begonia-work

# 4. (M4带入) Build the M4 secondary ramdisk (HFS+ with a static arm64 shell):
./ramdisk/mkramdisk.sh --out ~/hackios-begonia-work/package/ramdisk_m4.img \
                       --shell build-first

# 5. Package & stream (root-repo ownership — NOT this directory):
#    mkxnuimg.py (root) packs xnuloader + ADT + kernelcache + ramdisk.img
#    xl (root) streams/logs over fastboot/USB/serial.
```

Step 3's mount choices are enumerated in `apply_rootfs_patches.sh --help` and
`mount/README.md` notes below (mounting APFS on Linux is the messy bit; the
summarized guidance is in `apply_rootfs_patches.sh --help`).

### 5.1 Then getting it onto the device

The rootfs does **not** go into `boot.img`. `boot.img` only ever carries the
tiny M1..M3 test ramdisk. The patched rootfs is staged on UFS/SD either:

* by writing the restored base image + patch overlay from TWRP/Orangefox,
* or by `dd`ing a full APFS-ish partition image we compose here
  (see `ipsw/LayoutNotes.md`, which also documents the `/var/db` seeding hack).

---

## 6. Optional expansions (explicitly not done here)

| Idea | Status |
| :--- | :--- |
| `ipsw/ramdisk_repack.py` | ships (M4 helper) |
| `ipsw/merge_rd.py` | ships (dyld cache merge utility, reusable standalone) |
| SpringBoard UI/graphics bring-up | Phase-5 plan item 4 (not this repo — see plan, IOMobileFramebuffer / Backboardd work lives with kernel-port + shims) |
| IPSW OTA delta handling | out of scope (A11 full restores only) |
| Deploy TV/companion binary signing flows | out of scope |

---

## 7. Safety & legal notes

* All scripts write **only** within `--work` unless you pass an explicit
  `--in-place`-strength flag. A mistake can be recovered by re-running
  `fetch_ipsw.sh` + `unpack_ipsw.py` from scratch — but not your time.
* The iOS IPSW contains Apple's copyrighted code; it must be obtained from
  Apple's servers (that's also how the SHA training wheels stay valid).
* Every device-targeting step here assumes the Phase-1 checklist from the
  porting plan is complete: bootloader unlocked, `mtkclient` backups of
  `nvram`/`nvdata`/`persist`/`seccfg`/`proinfo` verified on the host.
