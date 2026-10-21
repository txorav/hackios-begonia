# LayoutNotes — final on-device `/var` & `/var/db` hack (HackiOS-begonia)

Phase-5 rootfs layout notes. Where `/var` comes from, why the stock rootfs
can't carry it, and the concrete seeding steps. Cross-refs:

* hardware contract …………… `docs/hardware_map_mt6785.md`
* upstream proof of `/dev/mdx` … `xnu/iokit/bsddev/IOKitBSDInit.cpp:766-788`
  (RAMDisk → `mdevadd`), PID1 `/sbin/launchd` exec `xnu/bsd/kern/kern_exec.c:7344`
* fstab replacement ………… `userland/patches/fstab`

---

## 1. Disk and volume layout (bring-up target)

```
UFS 2.1 (ufshcd-mtk @ 0x11270000)          # begonia internal, GPT per plan §Phase 4
├── (GPT) disk0s1  →  /dev/disk0s1s1   system volume (APFS, iBoot-sealed)
│       └── mounted at  /                                           (rootfs)
│           └── /private/var  [EMPTY mountpoint stub from the IPSW rootfs DMG]
└── (GPT) disk0s2  →  /dev/disk0s2     data volume (APFS)
        └── mounted at  /private/var
```

* On stock iPhone10,3 / iOS 16.5 the system volume is mounted from a boot-time
  snapshot of the **sealed** (Signed System Volume) volume, read-only. Our port
  has no seal code and no snapshot: we mount the volume directly, `rw`
  (`userland/patches/fstab`).
* Verify the actual stock `fstab` at patch time: `apply_rootfs_patches.sh`
  records SHA-256 and a backup copy `fstab.hackios-orig` — if the shipped
  iPhone10,3 20F66 image ships a different single-line form, diff it and
  adjust the patched variant accordingly (that backup *is* ground truth).

## 2. Why /var can't ride inside this repo's artifacts

The IPSW's rootfs DMG contains an **empty `/private/var` stub only**. All
`/var` content lives on the data volume (`disk0s2`), which:

1. **is not present in the IPSW** (restores initialize it on-device), and
2. **cannot be fabricated offline on Linux** — no trustworthy APFS writer
   exists in the free ecosystem for the volume formats iOS expects (Apple's
   `apfs.kext` handling of sealing/bookkeeping is closed-source and NOT in
   xnu 8796.121.2).

So the data volume gets initialized **on-device**, in the M4/M5 bring-up
order (options ordered from most preferred to noodliest):

| # | Route | Notes |
| -- | :--- | :--- |
| 1 | Kernel-side APFS container kext creates the container, our machine formats the data volume on first boot (bring-up "Fixed up /var" step in system stub kexts) | kernel-port owns the kext contract (`kexts/LxStorage`, plan §Phase 4 Stage B) |
| 2 | TWRP-style touch: recovery writes EXT4 partitions only — NOT usable for APFS directly | dead end, listed to prevent "just dd it" mistakes |
| 3 | Boot M4 HFS+ ramdisk; our static shell creates the /var skeleton once the system volume passes kext mount | needs both HFS (for ramdisk) and the APFS stack usable |

## 3. The /var/db seeding steps (concrete, on-device)

Once a shell exists (M4 static shell or sshd after M5), create the skeleton —
daemons create their own subpaths at first run, but these are the ones that
MUST exist before launchd.enable/boot-time services try to touch them:

```sh
# our static shell REPL (see ../ramdisk/static_shell) or any rootfs shell:
mkdir -p /var/db/com.apple.xpc.launchd/disabled
mkdir -p /var/db/com.apple.xpc.launchd/loginwindow
mkdir -p /var/db/hackios
mkdir -p /var/mobile/Library/Caches
mkdir -p /var/mobile/Media
mkdir -p /var/root
mkdir -p /var/tmp
mkdir -p /var/log
mkdir -p /var/preferences
# chown everything reachable back to uid 0 /uid 33 mobile via sysctl
chown -R 0:80 /var/mobile  # wheel owner, mobile group — VERIFY the GID on device
```

Then:

1. **MobileGestalt overlay** — put the plist built by
   `patch_mobilegestalt.py --staging` at:
   ```
   /var/mobile/Library/Caches/com.apple.MobileGestalt.plist
   ```
   (libMobileGestalt reads that on-device path as the cold-cache; if none is
   found it synthesises answers from the ADT — `/chosen` `chip-id`
   `0x8020`, `board-id` `D22AP`, `/product` props — which xnuloader already
   provides per `docs/hardware_map_mt6785.md` §3.)
2. **Provision marker** — `/var/db/hackios/provisioned.json` (the host-side
   `apply_rootfs_patches.sh` step (c) writes a copy onto the rootfs tree as
   documentation; the on-device copy is what services will actually see).
3. **Sandbox note** — if sandbox denials appear during the `logd/configd/`
   bring-up, use the checklist in `../patches/sandbox-notes.md` (launch
   plists run unsandboxed first, relax signin only where it blocks).

## 4. Snapshot / mount ordering facts

Verified against the in-tree sources rather than lore:
* Root mount selection uses boot args `rd`/`rootdev`
  (`xnu/iokit/bsddev/IOKitBSDInit.cpp:725-727`).
* The `md` device exists only if the ADT carries a `RAMDisk` entry under
  `/chosen/memory-map` (same file, :766-788). No entry → no /dev/md0 →
  `rd=md0` boot roots nothing and XNU panics wait-for-root.
* PID 1 search order `/usr/appleinternal/sbin/launchd.debug|development`
  then `/sbin/launchd` (`xnu/bsd/kern/kern_exec.c:7337-7345`) — DEBUG-only
  arms guarded by `#if DEBUG / DEVELOPMENT`; release kernels always
  `/sbin/launchd`.

## 5. What NOT to "fix" here

* Don't move MobileGestalt data into `/Library/Preferences/` only: BOTH
  locations matter — the host-side patcher writes the rootfs `/Library`
  copy AND the on-device `/var/mobile` copy (see `patch_mobilegestalt.py`).
* Don't "precreate" `/var/db/recovery`, `/var/db/sysdiagnose` trees blind —
  broken ownership is worse than absent dirs.
* Don't rename GPT labels; volume names are resolved through our IOMedia
  nub naming, not these paths.
