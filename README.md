# hackios-begonia

Port of **iOS 16.5 (`xnu-8796.121.2`)** / XNU-based Darwin to the
**Xiaomi Redmi Note 8 Pro** (`xiaomi begonia`, MediaTek Helio G90T `MT6785`,
ARMv8.2-A).  Follows `hackios_porting_plan_for_redmi_note_8_pro_begonia.md`
(§2 repository layout mirrored, all five engineering phases).

> ⚠️ Research project.  There is no Apple SDK and no working iOS build image
> here yet — this is the source tree + bring-up scaffolding that satisfies the
> plan's M1→M6 milestones.  Never flash to a device you cannot recover; use
> `fastboot boot` and BROM-mode `mtkclient` backups (Phase 1).

## Repository layout (plan §2, mirrored)

```
hackios-begonia/
├── uefi/             # optional EDK2 (edk2-mt6785) experiments (skeleton)
├── xnuloader/        # bare-metal EL1 bootloader: FDT -> Apple DeviceTree
│                     #   & Mach-O kernelcache loader (Phase 2)
├── xnu/              # patched XNU 8796.121.2 kernel for MT6785 (Phase 3)
├── xnu-patches/      # in-tree kernel edits index (per-file notes)
├── xnu-deps/         # libsystem headers, availability, dtrace, firehose,
│                     #   kext tools (deps kit for building on Linux)
├── userland/         # iPhone10,3 iOS 16.5 IPSW unpack + fstab/MobileGestalt
│                     #   patches + ramdisk builder (Phase 5)
├── wifi/lx/          # Linux-to-IOKit shim layer for MTK MT6785 devices
├── kexts/            # IOKit platform drivers: MT6785GICv3, platform expert,
│                     #   LxStorage, MT6785Framebuffer, FocalTouchIOHID, AMFI
│                     #   stub, SEP stub, watchdog stub (Phases 3–5)
├── tools/            # cross toolchains (LLVM/clang + cctools + ldid)
├── scripts/          # Phase-1 device prep helpers (mtkclient, FDT dump)
├── docs/             # canonical hardware map, boot flow, milestones
├── Makefile          # xnuloader → boot.img pipeline
├── mkxnuimg.py       # packs loader+ADT+kernelcache+ramdisk → boot.img v2
└── xl                # host utility: fastboot boot / serial log tail / panic pull
```

## Quickstart (bring-up order)

```sh
# 0. Phase-1 device prep (see docs/01_phase1_hardware_prep.md)
./scripts/begonia_backup.sh out/backup

# 1. Build the stage-2 loader (M1)
make xnuloader                      # needs aarch64 cross gcc (tools/)

# 2. Package a loader-only boot image and test M1
python3 mkxnuimg.py --out out/boot.img.m1 --loader out/xnuloader.bin \
                    --kernel build/placeholder.macho
./xl list && ./xl boot out/boot.img.m1

# 3. Kernel artifacts (M2+): XNU build + kernld product
#    see xnu-patches/0000-overview.md and xnu/config/MT6785*
make kernel                         

# 4. Full boot image
python3 mkxnuimg.py \
    --out out/boot.img \
    --loader out/xnuloader.bin \
    --kernel build/kernelcache.macho \
    --ramdisk out/ramdisk.img \
    --cmdline "serial=3 -v debug=0x8e keepsyms=1 rd=md0 rootdev=md0"

# 5. Run & observe (fastboot boot - RAM only)
./xl boot out/boot.img
./xl log -d /dev/ttyUSB0 -o out/console.log     # UART console (optional)
```

## Key provenance (what everything derives from)

| fact | source |
| --- | --- |
| boot_args layout, kBootArgsRevision2=2 | `xnu/pexpert/pexpert/arm64/boot.h` |
| `gVirtBase/gPhysBase` come from boot args | `xnu/osfmk/arm64/arm_vm_init.c` |
| xnu kernel window `VM_MIN_KERNEL_ADDRESS 0xffffffe0...` | `xnu/osfmk/mach/arm/vm_param.h` |
| SecureDT blob format | `xnu/pexpert/gen/device_tree.c` |
| root node must be named `device-tree` (+target-type/model) | `xnu/pexpert/arm/pe_init.c` |
| `/chosen/memory-map` ranges = `{u64 paddr, u64 len}` LE | `xnu/pexpert/pexpert/device_tree.h` |
| boot offsets/pagesize | porting plan §3 Phase 2 item 5 |
| GIC/UFS/MSDC/UART/I2C/fb addresses | plan §1 (+ docs/hardware_map_mt6785.md) |

## Milestones

See [`docs/milestones.md`](docs/milestones.md) — M1 loader-on-screen,
M2 early XNU banner, M3 SMP+timer, M4 shell, M5 launchd, M6 SpringBoard.

## Where to continue

1. `docs/hardware_map_mt6785.md` — addresses to verify on device (framebuffer,
   SPI interrupt numbers for msdc1/ufs/touchscreen).
2. `xnu-patches/` — the kernel-side `ARM64_BOARD_CONFIG_MT6785` in-tree work.
3. `docs/01_phase1_hardware_prep.md` — first commands to run on the phone.
