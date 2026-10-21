# tools/adb/snippets/ — safe device-state probing via adb

Scope of this folder: **read-only or explicitly push-safe helpers** used
over `adb` against the test device (`begonia`, Xiaomi Redmi Note 8 Pro,
MT6785). Everything here predates the moment real work happens — the
device first usually enters BROM via mtkclient (that is Phase 1/2
territory in `xnuloader/`, not ours). adb sessions matter in two spots:

1. **Pre-flash state audit** (booted Android): confirming variant, MIUI
   version, unlocked bootloader, partition table sanity. Read-only.
2. **Post-boot research device shell**: once XNU userland M5/M6 work runs,
   snapshots of the files our patch scripts touch
   (`userland/apply_rootfs_patches.sh`) so multiple agents reproduce the
   *exact same* edits.

## Ground rules (every snippet in here obeys them)

* Read-only unless the snippet itself says its writes are limited to
  `/sdcard/hackios/**`.
* Nothing here ever becomes part of a check that *fails* the build —
  adb is a device-state diagnostic, not CI.
* No snippet ever uses `fastboot unlock`-class destructive operations;
  bootloader unlocking is documented once in
  `docs/hardware_map_mt6785.md` §6 and is operator-manual.

## 1. One-liners (all safe/read-only)

Boot the device into its normal Android system, USB debugging on:

```
adb shell getprop ro.product.model          # expect: begonia? (MIUI reports "Redmi Note 8 Pro")
adb shell getprop ro.build.version.release  # Android side version
adb shell getprop ro.build.version.incremental  # MIUI increment
adb shell getprop ro.product.model boston   # NOT a real MTK — reference: A/B partitioning state
```

(the two `getprop` lines that need *root* — run through `su -c`; the phone
must have Magisk/kernelSU first for anything else to be possible)

```
adb shell su -c cat /proc/cmdline                       # confirms which console= ... lk passed
adb shell su -c cat /proc/iomem > HOST_FDT_SNAPSHOT     # verify docs/hardware_map § 0x47c00000-0x47FFFFFF carveout here
adb shell su -c 'cat /sys/firmware/fdt | base64' > host-devicetree.b64
adb shell su -c 'ls -l /sys/firmware/fdt' > HOST_FDT_LSB
```

The `0x47c00000..0x47FFFFFF` carveout (ATF/SCP/SSPM/TEE) from
`hardware_map_mt6785.md` §2 is the one property every XNU-side mapping
must *not* touch; `/proc/iomem` output is the on-device proof our plan
asks for — save it before flashing anything.

```
adb reboot bootloader          # enters lk bootloader
fastboot getvar all            # confirm boot_a/slot state, 'same device' match
fastboot flash boot ../out/boot.img    # our assembled image (mkxnuimg.py output)
```

(`fastboot flash boot …` **is** a write; it is listed here because it is
the contract-relevant one — `hardware_map §5` expects our image+SPL
kernel landing at `0x40080000`. Read-only alternative first:
`fastboot boot out/boot.img`.)

## 2. `create_patches.sh` — baseline + drift diff helper

One-shot helper (getopts-checked; refuses to run outside `/sdcard/hackios/`):

```
adb shell mkdir -p /sdcard/hackios
adb push create_patches.sh /sdcard/hackios/create_patches.sh
adb shell /sdcard/hackios/create_patches.sh -h
adb shell /sdcard/hackios/create_patches.sh       # first run → /sdcard/hackios/patches/<name>.baseline
adb pull /sdcard/hackios/patches ./patches-baseline
```

Then boot the ramdisk (M4/M5 userland moment) locally and re-run — the
second invocation over the same folder produces `<name>.patch` files when
the live file changed from baseline, so you can see exactly what our
patches-vs-stock identity looks like *on the target device*, not just in
our simulated ramdisk.

## 3. What NOT to do here

* Never `adb remount` on a research fruit — it is a no-op for APFS
  volatile disks and it triggers the "SEPreservation" path that can re-seal
  SSV (see `userland/patches/sandbox-notes.md` for the SSV waiver).
* Never test with `adb root` from iOS-side processes — those would be
  Step 1 of the bring-up, not a validate helper.
