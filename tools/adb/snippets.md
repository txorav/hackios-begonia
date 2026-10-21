# tools/adb/snippets/ — safe device-state probing via adb

Scope of this folder: **read-only or explicitly push-safe helpers** used
over `adb` against the test device (`begonia`, Xiaomi Redmi Note 8 Pro,
MT6785). The device first enters BROM via mtkclient for Phase 1–2 work
(territory of `xnuloader/`, not ours); adb matters in two spots:

1. **Pre-flash state audit** (while normal Android/MIUI is still booted):
   confirming variant, MIUI build, bootloader state, and grabbing the
   on-device evidence the bring-up plan wants saved (cmdline, `/proc/iomem`,
   the kernel FDT). All read-only.
2. **Post-restore research shell** (iPhoneOS 16.5/20F66 rootfs volume):
   snapshots of the files our patch scripts touch
   (`userland/apply_rootfs_patches.sh`) so different agents reproduce the
   *exact same* edits on a fresh volume.

## Ground rules (every snippet here obeys them)

* Read-only unless the snippet itself documents writes limited to
  `/sdcard/hackios/**` (`create_patches.sh` is the only one).
* Nothing in here gates or fails a build — device-state diagnostics only.
* No destructive bootloader flows in this folder; bootloader notice is
  documented once in `tools/adb/snippets.md` — operator-manual, as noted
  in `docs/hardware_map_mt6785.md` §4–§5 context.

## 1. Pre-flash one-liners (all safe / read-only)

With USB debugging enabled (Bootloader-alert engine, normal Android):

```
adb shell getprop ro.product.model                  # model (Redmi Note 8 Pro class)
adb shell getprop ro.product.device                 # expect: begonia
adb shell getprop ro.build.version.release          # Android version
adb shell getprop ro.build.version.incremental      # MIUI increment
```

These need a rooted (Magisk/KernelSU'd) test device, which is fine exactly
at this stage (`su` will run on Android side — a plain `adb shell` line
omits root for cascading these into a single log):

```
adb shell su -c 'cat /proc/cmdline' > haul-proc-cmdline.log
adb shell su -c 'cat /proc/iomem' > haul-proc-iomem.log
adb shell su -c 'cat /sys/firmware/fdt' | base64 > haul-kfdt.b64
```

The `0x47c00000–0x47FFFFFF` carveout (ATF/SCP/SSPM/TEE) from
`hardware_map_mt6785.md` §2 is the block the XNU-side mapping must *never*
include; check `haul-proc-iomem.log` for it before the first flash.

## 2. Bootloader (lk) stage — the one *write* in our flow

```
adb reboot bootloader           # enter lk
fastboot getvar all             # read-only audit first, always
fastboot boot out/boot.img      # read-only alternative: temp boot, nothing written
fastboot flash boot out/boot.img    # the ONE write: our mkxnuimg.py image
```

`hardware_map_mt6785.md` §5 expects kernel landing at `0x40080000` —
check `fastboot getvar all` output matches what mkbootimg declared
(`--base 0x40078000 --kernel_offset 0x00008000 ...`) before flashing.

## 3. Research-shell snapshotting — `create_patches.sh`

One-shot helper (getopts-checked; refuses output outside `/sdcard/hackios/`):

```
adb shell mkdir -p /sdcard/hackios
adb push create_patches.sh /sdcard/hackios/create_patches.sh
adb shell /sdcard/hackios/create_patches.sh -h      # usage
adb shell /sdcard/hackios/create_patches.sh         # run 1 → patches/<name>.baseline
adb pull /sdcard/hackios/patches ./patches-baseline
```

After userland patches land (M4/M5), re-run the same command — it then
emits `<name>.patch` whenever the live file drifted from baseline, so the
on-device patch identity is recorded the way `userland/patch_rootfs.sh`
expects it (same block order, same target set).

## 4. What NOT to do in this folder

* No **`adb remount`- or sealer-adjacent** tricks: our rootfs-side story is
  "we never verify any SSV seal, by construction" (see
  `userland/patches/sandbox-notes.md`); remount games add nothing and only
  trip MIUI integrity tooling on the Android side.
* Do not escalate to *"adb root for everything" habits*: snippets are
  diagnostics; any *write* path belongs in its operator-run script under
  userland/tools with a digest check.
