# Phase 1 — Hardware Preparation & Early Debugging (`begonia`)

Goal: obtain a recoverable bring-up environment *before* running any HackiOS
code, per the porting plan §3 Phase 1. Everything here runs **on the device**
(from Android or recovery) using `adb`/`adb shell`, or with `mtkclient` in BROM
mode with the device powered off.

## 1. Backup critical partitions (mtkclient, BROM mode)

Never flash `nvram`, `nvdata`, `persist`, `seccfg`, `proinfo`, or `preloader`.
Take full backups before anything else:

```sh
# device fully powered off; hold Vol-up(-)+Vol-down+flash cable to enter BROM
python3 tools/mtkclient/mtk r nvram,nvdata,persist,seccfg,proinfo,out/backup/mt6785
python3 tools/mtkclient/mtk r preloader,pgpt,sgpt,out/backup/mt6785
# full partition table backup (offline image of every partition; ~1h)
python3 tools/mtkclient/mtk rf out/backup/mt6785/full.img
```

The repository helper `scripts/begonia_backup.sh` wraps this with a verified
SHA-256 ledger. Store copies of `out/backup/` on two separate machines.

Then unlock the bootloader (`fastboot flashing unlock`) and install a working
custom recovery (TWRP/OrangeFox) into `recovery` — see
`scripts/begonia_backup.sh -r` for the TWRP flash step. Keep the stock
`boot`/`vbmeta` images saved from backup; `fastboot boot` (temporary boot) is
the preferred bring-up vector because it leaves the device untouched.

## 2. Early console channels (no soldering required)

The plan: two software debug channels backed by the `lk`-initialized LCD and a
DRAM ring that survives watchdog resets.

1. **Framebuffer console.** `lk` initializes the 1080×2340 panel and leaves an
   RGBX8888 framebuffer in high DRAM (typically `0x7f000000`, rowBytes 4320 —
   *verify on device*, §3). `xnuloader` and later XNU (`PE_state.video`,
   `vc_progress_initialize`) print to it directly.

2. **pstore / ramoops.** 64 KiB at `0x47df0000`, warm-reset persistent:
   * `xnuloader` writes an `XLDRPANIC` record (magic + regs + reason).
   * After a successful warm reboot into TWRP:
     `dd if=/dev/block/mmcblk0 skip=<lb of offset 0x47df0000>` — but ramoops DRAM
     is not on eMMC; in *Android Linux* it is `pstore.ramoops_mem=0x47df0000`
     with a kernel module reading live DRAM. Concretely: boot Android with the
     module parameter and inspect `/sys/fs/pstore/console-ramoops`. In TWRP a
     small `dd`-based reader can read live DRAM via `/dev/mem` only if
     `CONFIG_DEVMEM`+`CONFIG_STRICT_DEVMEM` allow it — prefer the pstore path.

## 3. Extract the memory & hardware map from a live device

From a booted Android (or mainline/postmarketOS) shell:

```sh
adb shell su -c "cp /sys/firmware/fdt /data/local/tmp/fdt"   # MediaTek BROM FDT
adb pull /data/local/tmp/fdt fdt.img
dtc -I dtb -O dts -o mt6785-xiaomi-begonia.dts fdt.img        # decompile
adb shell su -c "cat /proc/iomem"                              # reserved carveouts:
#   BL31 (ATF), SCP, SSPM, TEE ranges -> never map into XNU pmap; see
#   docs/hardware_map_mt6785.md (0x47c00000-0x48000000 placeholder)
adb shell su -c "cat /proc/cmdline"                            # lk cmdline hints
adb shell su -c "cat /proc/kmsg | head -c 2000000"              # DRAM layout logs
```

`scripts/begonia_dump_fdt.sh` automates this and `scripts/begonia_discovery.sh`
adds a framebuffer-address prober (writes a test pattern at candidate
addresses with `/dev/mem`, then looks for the pattern via screenshot diff).

Record all findings into `docs/hardware_map_mt6785.md` (the canonical map).

## 4. Safety rules

* Bring-up boots must use `fastboot boot boot.img` (RAM-only) until milestone
  M5; only after M4 validation flash anything (`fastboot flash boot`).
* Never power the display off / unplug mid-dd.
* `arm,gic-v3@0x0c000000`, `ufshcd-mtk@0x11270000`, `msdc1@0x11240000`,
  `uart0@0x11002000`, touch I2C `@0x11007000` are the addresses the ADT
  template (xnuloader) encodes; keep them in sync with the decompiled DTS.
