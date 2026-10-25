# HackiOS Begonia: Master Porting Roadmap & Architectural Blueprint
**Target Device:** Xiaomi Redmi Note 8 Pro (`begonia` / `begoniain`)  
**SoC:** MediaTek Helio G90T (`MT6785` / `MT6785V/CD`) — ARMv8.2-A (2x Cortex-A76 @ 2.05GHz + 6x Cortex-A55 @ 2.0GHz)  
**Target OS:** Apple iOS 16.5 / 16.7.16 (Apple XNU `xnu-8796.121.2` Darwin Kernel + Unmodified iOS Userland)  
**Spoofed Hardware Target:** iPhone X (`d22ap` / `iPhone10,3`, `iPhone10,6`)

---

## 1. Architectural Philosophy: Strict 3-Stage Gated Bring-Up

Unlike Snapdragon devices (`redwood`, `sweet`, `topaz`) which use Qualcomm SBL/XBL/ABL firmware, MediaTek Helio G90T uses a Preloader $\rightarrow$ Little Kernel (LK) $\rightarrow$ ARM Trusted Firmware (ATF BL31) boot sequence with strict TrustZone and hardware watchdog restrictions.

To prevent unrecoverable device bricks and blind debugging, this project enforces a **strict 3-stage gated execution model**:

```
[STAGE 1: COMPLETED]                         [STAGE 2: NEXT]                      [STAGE 3: FINAL]
EDK2-MTK UEFI Firmware (boot-begonia.img) ──► Standalone XNU 8796 (xnuloader) ──► iOS 16 Userland
 • TopRGU Watchdog Disabled                   • MT6785 GICv3 IRQ Routing          • Minimal Ramdisk (rd=md0)
 • DRAM & ATF/Kinibi TEE Carveouts            • Helio G90T SMP 8-Core Bring-Up    • UFS 2.1 Storage Drivers
 • 1080x2340 SimpleFB Display                 • Clean vfs_mountroot Halt          • launchd -> SpringBoard
```

---

## 2. Hardware Register Truth & Memory Map

Derived directly from stock MIUI Android 11 (`V12.5.15.0.RGGEUXM`) kernel device trees (`begonia_stock.dts`):

| Subsystem | Hardware Component | Physical Address Range | IRQ / Config | Architectural Handling |
| :--- | :--- | :--- | :--- | :--- |
| **System DRAM** | LPDDR4x (6GB / 8GB) | `0x40000000` – `0x1BFFFFFFF` | — | Base of physical memory |
| **ATF BL31 & Kinibi TEE** | Secure EL3 / S-EL1 | `0x43000000` – `0x44600000` | Secure | **CRITICAL CARVEOUT:** Must never be mapped as normal RAM. Access triggers immediate hardware reboot. |
| **SSPM / SPM** | Power & System Co-proc | `0x44600000` – `0x44800000` | Secure | Carved out / reserved |
| **Watchdog (TopRGU)** | `mediatek,mt6785-wdt` | `0x10007000` | SPI 139 | Write `0x22000000` to `WDT_MODE` (offset `0x0`) to disable 30s auto-reset. |
| **GICv3 Distributor** | `arm,gic-v3` (`GICD`) | `0x0C000000` (size `0x40000`) | GIC PPI 9 | Interrupt distributor |
| **GICv3 Redistributor** | `arm,gic-v3` (`GICR`) | `0x0C040000` (size `0x200000`)| 8 cores | Stride `0x20000` per core |
| **UART Console** | `apuart0` | `0x11002000` (size `0x400`) | SPI 91 | 115200 8N1 early debug console |
| **Display Panel** | In-Cell MIPI DSI | `0xBE000000` | — | 1080x2340 32bpp BGRA (`v_rowBytes = 4320`) |
| **UFS 2.1 Storage** | `mediatek,mt6785-ufs` | `0x11270000` | SPI 104 | UFSHCI controller for iOS rootfs |

---

## 3. Current Progress: Stage 1 (UEFI Bring-Up) — COMPLETE

Stage 1 is fully implemented, compiled, and packaged:
- **Prebuilt Boot Image:** `prebuilt/uefi/boot-begonia.img` (1.1 MB)
- **Raw UEFI Firmware:** `prebuilt/uefi/BEGONIA_UEFI.fd` (7.0 MB)
- **Pre-kernel Trampoline:** `prebuilt/uefi/BootShim.bin` (4.0 KB)

### Key Achievements in Stage 1:
1. **Watchdog Neutralization:** Written in `PrePi` and `PlatformPrePiLib` to disarm TopRGU before any timeout occurs.
2. **DRAM Map Carveouts:** Configured in `PlatformMemoryMapLib.c` to reserve `0x43000000`–`0x44800000` as `EfiReservedMemoryType`.
3. **Display Protocol:** Rewrote `SimpleFbDxe` with portable 64-bit integer bitwise operations, removing NEON intrinsics and host libc header conflicts.
4. **Boot Image Compatibility:** Wrapped into Android Boot Header v2 with the required MediaTek command-line string `bootopt=64S3,32N2,64N2 buildvariant=user` at offset `0x40`.

---

## 4. Stage 2: Apple XNU Kernel Bring-Up (Next Phase)

### Milestones:
- **M1 (EFI Loader Handoff):** `xnuloader.efi` executed from UEFI environment. Allocates memory for kernelcache, parses Mach-O headers, and builds the Apple Device Tree (`begonia.adt`).
- **M2 (Early Kernel Output):** Early `kprintf` directed to MT6785 UART (`0x11002000`) and the graphical framebuffer console (`0xBE000000`).
- **M3 (Interrupts & SMP):**
  - Configure MT6785 GICv3 (`0x0C000000` / `0x0C040000`).
  - Implement PSCI `CPU_ON` (`0x84000003`) to bring up all 8 CPU cores (2x A76 + 6x A55).
- **Stage 2 Gate:** Kernel boots cleanly and halts at `vfs_mountroot` with `cpus=8` online:
  ```
  panic(cpu 0 caller 0xfffffff0075xxxxx): "Cannot mount root, errno = 19"
  ```

---

## 5. Stage 3: iOS 16 IPSW & Userland Bring-Up

### Milestones:
- **M4 (Minimal Ramdisk Boot):**
  - Boot XNU with a 64MB HFS+ ramdisk passed via ADT `/chosen/memory-map` (`RAMDisk`).
  - Kernel argument: `rd=md0 rootdev=md0`.
  - Validate PID 1 execution of a statically-linked shell masquerading as `/sbin/launchd`.
- **M5 (UFS Storage & AMFI Bypass):**
  - Implement lightweight UFS 2.1 block storage driver for MT6785 (`0x11270000`).
  - Patch `AMFI` (Apple Mobile File Integrity) and `AppleMobileFileIntegrity.kext` to allow unsigned binaries and bypass SIP.
  - Mount genuine iOS 16.5 rootfs APFS/HFS+ partition.
- **M6 (Full Userland UI):**
  - Start Apple `launchd`.
  - Launch `backboardd` and `SpringBoard`.
  - Implement IOHID driver for Novatek `nt36672a` / Focaltech `ft8719` touchscreen panels.

---

## 6. How to Test on Hardware

> [!CAUTION]
> Never flash untested images to physical disk partitions (`boot`, `recovery`, `lk`). Always test transiently in RAM using `fastboot boot`.

1. Boot your Redmi Note 8 Pro into Fastboot Mode (`Power` + `Volume Down`).
2. Run transient boot from your host machine:
   ```bash
   fastboot boot prebuilt/uefi/boot-begonia.img
   ```
3. Observe display transitions and serial console output.
