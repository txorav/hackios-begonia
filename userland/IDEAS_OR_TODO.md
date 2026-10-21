# IDEAS_OR_TODO — userland scope (Phase 5 items 2-4 + polish)

Status key: `[x]` done in this repo · `[~]` partially done (delta noted) ·
`[ ]` not done, owner flagged · `?` open question blocking.

Phase numbering refers to the porting plan §3 Phase 5 items:

1. Extract & prepare the root filesystem (this repo · DONE-ish)
2. Patch AMFI & code-signing enforcement in XNU
3. Stub missing Apple coprocessors (SEP, AppleKeyStore, MobileGestalt)
4. Graphics & touch (SpringBoard/backboardd, IOMobileFramebuffer, IOHID)

---

## Phase 5.1 — rootfs extraction & preparation (this repo)

* [x] `fetch_ipsw.sh` — iPhone10,3_iPhone10,6_16.5_20F66_Restore.ipsw,
      resumable, `--dry-run`, SHA1 check + api.ipsw.me fallback.
* [x] `unpack_ipsw.py` — zip enumeration, rootfs/ramdisk classification,
      BuildManifest digest verification vs whichever digest key the bundle
      uses (`Digest` / `FileChecksum`), `build_manifest_summary.json`.
* [x] `apply_rootfs_patches.sh` (+ deprecated alias `patch_rootfs.sh`) —
      fstab rw patch, MobileGestalt patch, /var/db bookkeeping,
      patch_manifest.json, `--rollback`.
* [x] `patch_mobilegestalt.py` — real MG key names (hashed CacheExtra),
      chip-id 0x8020/32800, iPhone10,3/D22AP, canonical screen numbers
      (documented: the plan's `HwModel`/`MainScreenPitch` names don't exist;
      the real keys are HWModelStr/ProductType/main-screen-*).
* [x] `patches/fstab`, `patches/com.apple.MobileGestalt.plist`,
      `patches/sandbox-notes.md`.
* [x] `ipsw/merge_rd.py` — arm64 dyld cache classify+merge (used when a
      rootfs copy lacks the merged cache; A11 is non-cryptex).
* [x] `ipsw/ramdisk_repack.py`, `ramdisk/mkramdisk.sh`, `ramdisk/static_shell`.
* [~] macOS `hdiutil` path in `apply_rootfs_patches.sh` — only points the
      operator at manual detach; no scripted attach (hdiutil attach can
      hang without a real VM; needs a macOS test host to script safely).

### Open questions (Phase 5.1)

* ? Does the iOS 16.5 iPhone10,3 payload's rootfs DMG actually still ship the
  *full merged* `dyld_shared_cache_arm64` (assumed YES for A11-era non-
  cryptex bundles), or a pruned variant whose merge candidate is the restore
  ramdisk? Verify on the first real unpack; `merge_rd.py` handles both but
  classification thresholds (currently 1 GiB-ish) may need tuning.
* ? Exact fstab layout shipped inside the 20F66 DMG (`disk0s1s1` snapshot
  suffix? `nosuid` on /var?) — `apply_rootfs_patches.sh` keeps the original
  at `fstab.hackios-orig`; diff on first unpack and reconcile `patches/fstab`.
* ? Should the patched rootfs ALSO strip leftover `.symbolic` sidecar dylib
  files, if present, to save UFS space? decide after first unpack.

## Phase 5.2 — AMFI & code-signing enforcement (kernel-port owns; userland consumes)

* [ ] AMFI-stub kext unconditionally approves signatures + adds
      CS_PLATFORM_BINARY → `kexts/AMFIStub` (**kernel-port**).
* [ ] `cs_enforcement_disable=1` flag plumbing — boot args already in the
      canon (`docs/hardware_map_mt6785.md` §4); the XNU parse sites are
      `kern_codesigning.c` (verified in-tree).
* [x] userland side: everything we ship (`ldid -S`-signed static shell) is
      valid under both "cs off" and "AMFI stub" paths; nothing else needed
      here.
* ? Does kernel-port want a shared "bring-up signing profile" (an ldid
      entitlement set that userland artifacts claim) so entitlement checks
      stay consistent between userland binaries and stub kext allowances?
      (See plan §Phase 5.2 wording; awaiting kernel-port input.)

## Phase 5.3 — stub missing Apple coprocessors (kernel-port owns; userland documents)

* [ ] `kexts/SEPStub` (AppleKeyStore / AppleSEPManager / AppleMobileApnonce)
      — keybag unlock success, software AES backstop, fake apnonce.
      **kernel-port**.
* [ ] `kexts/WatchDogStub` (IOWatchDogTimer) — prevents watchdogd's 60 s
      check-in panic. **kernel-port**.
* [x] The fake DeviceTree (ADT) properties the stubs + MobileGestalt
      consume are specified in `docs/hardware_map_mt6785.md` §3 and
      xnuloader's fdt2adt work; userland has cross-pointers in
      `apply_rootfs_patches.sh` steps (e)/(f), and the MG cache covers
      key lookups that fall back to the ADT.
* ? MobileGestalt cache: write BOTH `/Library/Preferences` (rootfs copy, our
  patch does this) AND `/var/mobile/Library/Caches` (on-device overlay;
  documented in `ipsw/LayoutNotes.md`) — confirm on device which path the
  16.5 libMobileGestalt treats as authoritative cold cache.

## Phase 5.4 — graphics & touch (shims + kernel-port; userland inputs)

* [ ] `IOMobileFramebuffer` backed by the lk framebuffer
      (`0x7f000000`, 1080×2340, RGBX8888, rowBytes 4320) — **shims team**
      (`kexts/MT6785Framebuffer`).
* [ ] IOSurface-backed software rasterizer so backboardd/SpringBoard
      composite into the scanout buffer (plan §Phase 5.4). **shims**.
* [ ] Touch route: `focaltech,fts` @ I2C `0x11007000` addr `0x38` →
      `IOHIDEventService` (plan). **shims** (`kexts/FocalTouchIOHID`).
* [x] From userland's side, the only Phase-5.4 deliverable is documentation
      of screen numbers (`patch_mobilegestalt.py --screen-mode`) — the
      aspect-ratio match (19.5:9 iPhone X vs begonia panel) is recorded
      there. Nothing else to do until the framebuffer kext lands.
* ? SpringBoard UI assets for iPhone X assume 1125×2436 @3x; begonia's true
  canvas at 3× is 360×780 points. First boot of SpringBoard will test
  whether 360×780 renders acceptably or we letterbox. Pick after first
  live attempt (no decision needed now).
* ? Metal/game research: plan suggests a Vulkan/Panfrost bridge for M6-era
  work — out of userland scope; tracked in plan only.

## Non-Phase-5 hygiene (userland)

* [x] `console/README.md` — pointer to root `xl`, correct facts about UART
      and ramoops.
* [ ] Once `xl` exists at repo root: replace manual `stty + cat` snippet in
      `console/README.md` with actual `xl` usage (blocked on `xl` landing).
* [ ] run `tools/check_env.sh` and record output in this file as a
      "known-good host state" block (needs a toolchain-equipped host).
