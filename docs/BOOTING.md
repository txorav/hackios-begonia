# Booting guide — begonia end to end

Operational manual that executes **HackiOS-begonia-guide.md** with this repo's
artifacts, from a fresh phone to a serial console running XNU. It folds in the
guide's §2 (phone prep), §3 (environment), §5 (QEMU-first), §8 (real
hardware), and §13 (recovery), pinned to this repo's actual commands.

> Golden rules (from the guide's §0/§13):
> 1. **Back up every partition before anything else** (step 1 below).
> 2. Never redistribute Apple binaries — extract the IPSW locally, keep it out
>    of git (`.gitignore` already excludes the IPSW/runtime dirs).
> 3. Development is always `fastboot boot` (RAM only, flash untouched).
>    Flashing (`fastboot flash boot out/boot.img`) is reserved for a *verified
>    stable* image, with `scripts/begonia_backup.sh` output on hand for the
>    instant BROM restore.

---

## 0. Artifact map

| Artifact | Produced by | Meaning |
|---|---|---|
| `out/xnuloader.bin` | `make xnuloader` | stage-2 loader raw image (lk loads this) |
| `build/xnuloader.elf` | same | ELF with symbols, for QEMU/GDB |
| `build/xnuloader_qemu.elf` | `make qemu` (`qemu/run_qemu.sh`) | QEMU variant (PL011 console, RAM-linked) |
| `build/kernelcache.macho` | XNU build (`xnu-patches/0000-overview.md`) | MT6785 kernelcache |
| `out/ramdisk.img` | `make ramdisk` (`userland/ramdisk/mkramdisk.sh`) | static `bash` + diag tools for `rd=md0` |
| `out/boot.m1.img` | `make boot.img` (M1 fallback) | loader-only image (no kernel yet) |
| `out/boot.img` | `make boot.img` (when kernelcache exists) | full boot image |
| `xl` | host | device control: `xl boot/logo/panic` |
| `docs/hardware_map_mt6785.md` | — | canonical register/memory/boot contract |

---

## 1. One-time phone preparation (guide §2)

```bash
# 1a. bootloader unlock (Mi Unlock tool, 168 h waiting period)
# 1b. tooling
sudo apt-get install -y adb fastboot libusb-1.0-0-dev python3-pip git
pip install --user mtkclient          # or: git clone github.com/bkerler/mtkclient tools/mtkclient

# 1c. FULL backup - nvram/nvdata/proinfo/persist hold IMEI + calib; losing them is fatal
./scripts/begonia_backup.sh            # wraps: mtk r preloader,lk,boot,recovery,vbmeta,dtbo,nvram,nvdata,proinfo,protect1,protect2,persist preloader_backup/

# 1d. drill: enter BROM with the phone OFF, hold Vol+ & Vol-, plug USB,
#     confirm: python3 -m mtk da --restore  ships. Do this now, not during a brick.

# 1e. UART (non-negotiable for kernel bring-up)
#     - locate the UART test pads (teardown diagrams for begonia; board edge)
#     - 1.8 V levels ONLY. A 3.3 V adapter on a 1.8 V line kills the SoC.
#     - tx/rx land on the pads' RX and TX respectively; keep GND tight.
./scripts/begonia_discovery.sh         # pads, baud, FDT dump capture (guide §8.2)
./scripts/begonia_dump_fdt.sh          # snapshot lk's full DTB over the UART console
```

Baud: lk's console typically runs at **921600 8N1** on MTK boot chains;
115200 is the fallback if you see garbage. The loader matches whatever lk
programmed (it does not re-init the UART).

---

## 2. Host environment (guide §3, no macOS needed for this target)

```bash
./scripts/setup_host.sh     # aarch64 cross gcc/clang, mkbootimg deps, qemu
# verify (tools/ owned by the userland lane):
make syscheck               # runs tools/check_env.py when present
```

Minimum: `gcc-aarch64-linux-gnu` (or `aarch64-none-elf-gcc`),
`python3`, `qemu-system-aarch64` for the step-0 rehearsal.

---

## 3. Build the boot stack

```bash
make tests        # host unit tests: FDT parser + ADT blob writer (no cross toolchain)
make xnuloader    # out/xnuloader.bin + build/xnuloader.elf (M1 artifact)
make kernel       # XNU MT6785 build → build/kernelcache.macho (see xnu-patches/0000-overview.md)
make kexts        # MT6785GICv3 / PlatformExpert / AMFIStub / ... (kexts/build.mk)
make ramdisk      # out/ramdisk.img
make boot.img     # mkxnuimg.py → out/boot.img (M1-only image if kernelcache missing)
```

Full-image equivalent (matches `docs/hardware_map_mt6785.md` §2 layout):

```bash
python3 mkxnuimg.py --out out/boot.img \
  --loader out/xnuloader.bin --kernel build/kernelcache.macho \
  --ramdisk out/ramdisk.img \
  --cmdline "serial=3 -v debug=0x8e keepsyms=1 rd=md0 rootdev=md0 cs_enforcement_disable=1 amfi_get_out_of_my_way=0x1 wpdebug=1" \
  --fb 1080x2340x4320x0x32 --fb-base 0x7f000000
```

Layout inside the image (loader resolves it at runtime):
`loader@0x40080000 → payload manifest (first 64 KiB after loader) → ADT →
kernelcache@0x40200000 → ramdisk → boot_args → top-of-kernel 16 KiB guard`.

---

## 4. Step 0 — QEMU rehearsal before touching the phone (guide §5)

```bash
make qemu        # bash qemu/run_qemu.sh (github: app runs it on every push)
```

Expected serial (`qemu/out/qemu.log`):

```
HACKIOS xnuloader: lk handoff, FDT @ 0x00000000440000000
memSize   0x0000000080000000
XLDR PANIC: payload manifest not found (mkxnuimg append failed?)
```

PASS = loader banner on UART + clean panic waypoint parked (WFE) with the
ramoops record written. This validates `entry.S` → `xldr_main()` → FDT parse
→ framebuffer write → payload scan → panic path entirely on the host. The
full payload (Mach-O load + ADT + `boot_args` + XNU handoff) remains a device
test until the kernelcache exists.

---

## 5. Device bring-up loop (guide §8 test loop)

```bash
# phone at fastboot mode, flash never touched by this:
fastboot boot out/boot.img.m1       # M1: loader-only image
# then, once the kernel builds:
fastboot boot out/boot.img
# watch UART:  screen /dev/ttyUSB*, 921600 8N1 (try 115200 as fallback)
# or capture:  capt't: cat /dev/ttyUSB* | tee build/uart.log
```

Expected UART transcript, stage by stage (each stage is an M1 gate):

| # | Line on UART | Meaning | Gate |
|---|---|---|---|
| a | `HACKIOS xnuloader: lk handoff, FDT @ 0x...` | `entry.S` reached C at EL1, MMU off | M1 |
| b | `memSize  0x...C0000000` | FDT parsed, true DRAM size from lk's DTB | M1 |
| c | fb test pattern on the panel (green `HACKIOS XLDR`) | framebuffer console alive | M1 |
| d | `payload manifest found; sections: 0x3` | `XLDRPAY1` scan worked | M1.5 |
| e | `kern 0x40200000 entry 0x... ADT ... RD ... BA ...` | payload → mmu-ready layout resolved | M1.9 |
| f | `ENTERING XNU...` | cache cleaned, jumping to LC_UNIXTHREAD entry | M2 handoff |
| g | *(XNU banner via PE_init_printf — kernel lane's first claim)* | MMU on, boot_args accepted | M2 |

On any inconsistency the loader self-diagnoses:

```
XLDR PANIC: <why>
```

…written to UART, marked in red on the panel, recorded at ramoops
`0x47df0000` (64 KiB, warm-reset persistent), then parks with `wfe` so the
pad stays alive for debugging.

Diagnosis loops for later milestones cross-ref `docs/milestones.md`
(M3 uses panic→ramoops→TWRP cycles; `xl` automates the reboot-and-diagnose).

---

## 6. Recovery (guide §13)

- **The phone won't even turn on normally:** BROM (guide §2.4, `python3 -m mtk
  da --restore` / `python3 -m mtk w boot preloader_backup/boot.img` → restores
  the `boot` partition with the copy `begonia_backup.sh` made).
- **`fastboot boot image` doesn't even boot it:** the lk handoff FDT protocol
  on this unit mismatches the layout — run `scripts/begonia_dump_fdt.sh` and
  compare `docs/hardware_map_mt6785.md` §2 offsets; adjust `main.c` constants
  and re-pack; no flash state changed.
- **Worst-case:** restore preloader/flash via mtkclient from your backup
  directory. Because you used only `fastboot boot`, flash was never written,
  so recovery is complete.

---

## 7. Troubleshooting matrix

| Symptom | Likely cause | Fix |
|---|---|---|
| No UART output at all | wrong pad / TX on RX / 3.3 V fried nothing | swap pads try again; check with lk console dBUI |
| Boot device flickers then nothing | lk dropped the image (bad `ARM\x64` header) | `image_head` must be exactly the 64-byte Linux boot header — rebuild, check `xnuloader.bin[0x38] == 0x644d5241` |
| banner prints, then garbage | lk hands cmdline on a *different* UART slot | confirm pad→fdt reg addr; cross-check `begonia_dump_fdt.sh` `reg` for `uart0` |
| Loader loop / random crashes | `-nostdlib` FP register asm (stk bug), broken linker script | run `make tests`, then `make qemu` — both catch this host-side |
| XNU panics before printing anything | `boot_args`/ADT wrong (`serial=3` needed early) | check `build/adt.bin` (mkxnuimg with `--adt build/adt.bin`) replay via QEMU |

<!-- cross-ref: guide §10-11 (Phase F does not begin until M4; Phase G/M6
     follow the shims' kexts — those milestones live in docs/milestones.md) -->
