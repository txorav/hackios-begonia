# HackiOS Begonia: Porting Apple XNU & iOS 16 to Xiaomi Redmi Note 8 Pro

[![CI](https://github.com/txorav/hackios-begonia/actions/workflows/ci.yml/badge.svg)](https://github.com/txorav/hackios-begonia/actions/workflows/ci.yml)
[![Build Begonia UEFI](https://github.com/txorav/hackios-begonia/actions/workflows/build-uefi.yml/badge.svg)](https://github.com/txorav/hackios-begonia/actions/workflows/build-uefi.yml)
[![Status: Stage 1 Complete](https://img.shields.io/badge/Stage%201-UEFI%20Complete-brightgreen.svg)](#stage-1-uefi-firmware--boot-image-completed)

Bare-metal port of Apple's **XNU kernel (`xnu-8796.121.2`)** and unmodified **iOS 16.5 / 16.7.16 userland** to the **Xiaomi Redmi Note 8 Pro** (`begonia` / `begoniain`, MediaTek Helio G90T / `MT6785`), spoofing an iPhone X (`d22ap` / `iPhone10,3`, `iPhone10,6`).

📖 **Read the Master Roadmap:** [`docs/ROADMAP.md`](docs/ROADMAP.md)  
🗺️ **Hardware Register & IRQ Map:** [`docs/BEGONIA_HW_MAP.md`](docs/BEGONIA_HW_MAP.md)

---

## 3-Stage Gated Architecture

Unlike Qualcomm Snapdragon reference ports (`redwood`, `sweet`, `topaz`), the MediaTek Helio G90T has strict TrustZone (ATF BL31) memory protection, an aggressive hardware watchdog (TopRGU), and proprietary display handoffs. To guarantee zero unrecoverable bricks, this project follows a strict 3-stage bring-up pipeline:

```
[STAGE 1: COMPLETED]                         [STAGE 2: NEXT]                      [STAGE 3: FINAL]
EDK2-MTK UEFI Firmware (boot-begonia.img) ──► Standalone XNU 8796 (xnuloader) ──► iOS 16 Userland
 • TopRGU Watchdog Disabled                   • MT6785 GICv3 IRQ Routing          • Minimal Ramdisk (rd=md0)
 • DRAM & ATF/Kinibi TEE Carveouts            • Helio G90T SMP 8-Core Bring-Up    • UFS 2.1 Storage Drivers
 • 1080x2340 SimpleFB Display                 • Clean vfs_mountroot Halt          • launchd -> SpringBoard
```

---

## Stage 1: UEFI Firmware & Boot Image [COMPLETED]

Stage 1 is fully compiled, validated against freestanding build rules, and ready for testing.

### Prebuilt Artifacts
- **Bootable Android Boot Image (Header v2):** [`prebuilt/uefi/boot-begonia.img`](prebuilt/uefi/boot-begonia.img) (`1.1 MB`)
- **Raw UEFI Firmware Volume:** [`prebuilt/uefi/BEGONIA_UEFI.fd`](prebuilt/uefi/BEGONIA_UEFI.fd) (`7.0 MB`)
- **Pre-kernel Trampoline:** [`prebuilt/uefi/BootShim.bin`](prebuilt/uefi/BootShim.bin) (`4.0 KB`)

### Critical Hardware Protections Solved:
1. **TopRGU Watchdog Neutralized:** MediaTek Little Kernel arms a hardware watchdog at `0x10007000`. Our `PrePi` writes magic `0x22000000` to `WDT_MODE` on entry to prevent 30-second reboots.
2. **ATF & Kinibi TEE Carveout:** DRAM covers `0x40000000`–`0x1BFFFFFFF`. Secure monitor firmware occupies `0x43000000`–`0x44600000` and SPM at `0x44600000`–`0x44800000`. These regions are marked `EfiReservedMemoryType` so neither UEFI nor XNU touches them, avoiding EL3 resets.
3. **Display Framebuffer:** Native 1080x2340 32bpp BGRA (`0xBE000000`) supported via `SimpleFbDxe` using 64-bit integer bitwise blending.
4. **LK 64-bit Command Line:** Header v2 includes `bootopt=64S3,32N2,64N2 buildvariant=user` at offset `0x40`.

---

## Quickstart: Transient Boot via Fastboot (RAM Only)

> [!CAUTION]
> Never flash untested images to physical partitions (`boot`, `recovery`, `lk`). Always boot transiently in RAM:

```bash
# 1. Boot Redmi Note 8 Pro into Fastboot Mode (Power + Volume Down)
# 2. Transiently boot the UEFI image:
fastboot boot prebuilt/uefi/boot-begonia.img
```

---

## Repository Structure

```
hackios-begonia/
├── docs/                     # Canonical hardware truth, register maps, and roadmap
│   ├── ROADMAP.md            # Complete 3-stage bring-up roadmap
│   ├── BEGONIA_HW_MAP.md     # MT6785 physical addresses, IRQs, and registers
│   └── dts/                  # Genuine stock DTB and decompiled DTS
├── prebuilt/                 # Compiled release binaries
│   └── uefi/                 # Stage 1 boot-begonia.img, BEGONIA_UEFI.fd, BootShim.bin
├── uefi/                     # UEFI porting notes & EDK2 build recipes
│   └── edk2-port/            # Complete begoniaPkg, MT6785Pkg, BootShim, and patches
├── xnuloader/                # Bare-metal EL1 bootloader: FDT -> Apple DeviceTree
├── xnu/                      # Apple XNU 8796 kernel tree and MT6785 platform patches
├── userland/                 # iOS 16.5 IPSW unpack, ramdisk builder, AMFI bypass
├── kexts/                    # IOKit drivers (MT6785GICv3, Framebuffer, Touch, Storage)
├── tools/                    # Toolchains and boot image pack/unpack scripts
├── Makefile                  # Stage-2 xnuloader build pipeline
├── mkxnuimg.py               # Android boot image packer (loader+ADT+kernel+ramdisk)
└── xl                        # Host CLI utility for fastboot boot, serial logs, and panics
```

---

## Next Milestones (Stages 2 & 3)

- **Stage 2 (M1–M3):** Boot standalone XNU 8796 kernel via UEFI (`xnuloader.efi`), configure GICv3 (`0x0C000000` / `0x0C040000`), wake secondary CPU cores via PSCI, and reach clean `vfs_mountroot` halt.
- **Stage 3 (M4–M6):** Minimal 64MB HFS+ ramdisk (`rd=md0`), MT6785 UFS 2.1 storage driver, AMFI code-signing bypass, and start `launchd` $\rightarrow$ `backboardd` $\rightarrow$ `SpringBoard`.
