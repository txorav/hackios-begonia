# MT6785 UEFI Port Plan

**Target:** Xiaomi Redmi Note 8 Pro (`begonia`), MediaTek MT6785 / Helio G90T  
**Reference firmware:** Renegade Project `edk2-msm8998` (Qualcomm Snapdragon 835)  
**Primary payload:** Linux `Image` + initrd  
**Status:** planning draft. No source tree has been modified.

> **Confirmed decisions (2026-10-20):**
> - Target: Xiaomi Redmi Note 8 Pro / `begonia` / MT6785.
> - Payload: Linux first.
> - **No UART access.**
>
> Because there is no UART, the primary bring-up console is the **framebuffer that LK already initializes**, not UART0. The UART material in §3.3 and §6.3 is retained as a fallback for a later hardware UART or MTK USB-UART mode. See §3.5 for the debug/input strategy and the changes to M0/M1.

---

## 0. Executive summary

1. The existing XNU tree in `/media/dev/work/hackios_source` is upstream Apple XNU (`xnu-12377.1.9`, Darwin 25 / macOS 26 / iOS 26). It has no MediaTek platform support, no ACPI/GIC ARM64 boot path, and no MT6785 drivers. **The UEFI project is a bootloader project, not an XNU platform port.** Booting XNU on MT6785 would be a separate, much larger project.
2. `/media/dev/work/uefi-common.img` is a genuine Renegade EDK2 image for **MSM8998**, not MTK. It must not be flashed to the Redmi Note 8 Pro.
3. The MSM8998 port is a useful **architectural** reference: PrePi + DXE, a Linux arm64 header in the FDF, FDT handoff, and SimpleInit as the Linux loader. Its hardware layer is almost entirely Qualcomm-specific and cannot be reused.
4. A practical MT6785 port should progress through four gates:
   - **M0:** a tiny **framebuffer probe** payload that proves the boot chain, load address, x0/FDT handoff, and display/framebuffer access (UART is optional; see §3.5).
   - **M1:** a minimal EDK2 firmware that reaches a **framebuffer console** and runs a scripted UEFI Shell (`startup.nsh`). Interactive input is a separate problem because there is no UART; the first input options are the volume/power keys and, later, a USB keyboard.
   - **M2:** FDT/memory-map support plus SimpleInit.
   - **M3:** storage (SD first, UFS later), then Linux boot; USB/PMIC follow.
5. The first hard problems are **not** EDK2 itself. They are MTK boot-image packaging, the runtime FDT, DRAM/reserved-memory layout, debug-console/input access, the display framebuffer, and clocks/regulators.

---

## 1. Verified starting point

### 1.1 XNU tree

- `/media/dev/work/hackios_source` matches upstream `xnu-12377.1.9` except for an added `README.md`.
- Supported ARM64 machine configs are Apple-only: `T6000 T6020 T6030 T6031 T6041 T8101 T8103 T8112 T8122 T8132 VMAPPLE`.
- The ARM64 path depends on Apple device trees, Apple CPU/PIO/SMP, and Apple AIC interrupts. There is no generic ACPI/GIC ARM64 platform path that could be pointed at MT6785.
- Therefore: **do not plan to boot XNU as the first payload.** A Linux boot is the correct bring-up target.

### 1.2 `/media/dev/work/darwin-kernel`

- All six files are synthetic filler with fake metadata. They are not a usable Darwin/XNU kernel and must not be flashed.
- Treat this directory as untrusted for any future work.

### 1.3 `/media/dev/work/uefi-common.img`

- Valid Android boot image (`ANDROID!`), Renegade EDK2 firmware for **MSM8998 / Snapdragon 835**.
- Header is v0: `kernel_addr = 0x0`, gzipped kernel size `0xb13cb7`, no ramdisk.
- It is unsigned and does not match MTK. Flashing it to `begonia` will not boot.

### 1.4 `mt6785-mainline`

- Mainline Linux tree and DTS for MT6785 and `begonia`.
- Contains boot-image backups, `dumped_preloader.bin`, and the extracted stock DTB used below.
- The device identity is confirmed by `mt6785-xiaomi-begonia.dts`:
  - `model = "Xiaomi Redmi Note 8 Pro"`
  - `compatible = "xiaomi,begonia", "mediatek,mt6785"`

### 1.5 `begonia_blobs_code`

- Android vendor blobs, HALs, and device backups.
- Useful for reverse engineering hardware behaviour and for restoring the device.
- The blobs are Android/Linux userspace or proprietary firmware; they are **not** EDK2 drivers and cannot simply be linked into the firmware.

### 1.6 Reference checkout

The `edk2-msm8998`, `simple-init`, and `edk2-prepi` checkouts currently live under an ephemeral external temp path. Before starting the port, copy or re-clone them into a stable location, for example:

```text
/media/dev/work/refs/edk2-msm8998
/media/dev/work/refs/simple-init
/media/dev/work/refs/edk2
/media/dev/work/refs/edk2-platforms
```

This avoids losing the reference when the temp mount is cleaned.

---

## 2. How the reference UEFI project works

### 2.1 Boot flow

```text
preloader
  -> LK / ABL
    -> boot.img (gzip-compressed UEFI FD)
      -> Linux arm64 Image header at FD base
        -> branch to FV at FD base + 0x8000
          -> PrePi (SEC/PEI replacement)
            -> DxeCore
              -> platform DXE drivers
                -> BDS
                  -> UEFI Shell and/or SimpleInit
                    -> Linux Image + initrd
```

### 2.2 Important design points

- **No SEC/PEI core.** The FDF starts with a Linux arm64 `Image` header. The bootloader sees a "kernel", decompresses it, and branches into it with the FDT pointer in `x0`.
- **FD and FV are separate.** In the MSM8998 FDF, the FD base is `0xd0000000`, the first `0x8000` bytes are the Linux header, and the FV starts at `0xd0008000`.
- **`ArmPlatformPeiBootAction` is the first C/asm entry.** It is responsible for:
  - storing the FDT pointer (`x0`) into a fixed scratch address;
  - relocating/copying the FD to its link address if the bootloader loaded it elsewhere;
  - fixing the return address after a copy.
- **`MemoryInitPei` parses the FDT.** It discovers `/memory`, chooses a platform memory map, builds resource HOBs, and installs the MMU table.
- **DXE is a mix of open source and prebuilt Qualcomm drivers.** The MSM8998 port relies on roughly 36 prebuilt Qualcomm DXE binaries for UFS, USB, display, PMIC, clocks, and related blocks.
- **The MSM8998 console is framebuffer-backed.** It uses `FrameBufferSerialPortLib` / `InMemorySerialPortLib`, not a real UART. This is directly useful for MT6785: with no UART access, the MTK port should reuse the same framebuffer-console approach and keep a real UART library only as a fallback.
- **SimpleInit is the Linux loader.** It publishes/consumes `KernelFdtProtocol`, loads a kernel and initrd from a filesystem, patches the FDT, and calls the raw arm64 entry with the DTB pointer.

### 2.3 What to reuse and what to replace

| Reference component | Reuse for MT6785? | Notes |
|---|---|---|
| FDF/FV Linux-header trick | Yes | Change base, size, FV offset, `image_size`. |
| PrePi + DxeCore architecture | Yes | Generic EDK2 code. |
| Build script structure | Yes, adapt | New DSC/FDF, header v2, new DTB path. |
| SimpleInit integration | Yes | It is platform-independent. |
| `ArmPlatformLib` | No, rewrite | MTK memory map and MP table. |
| `ArmPlatformHelper.S` | Adapt | Must store `x0` unconditionally. |
| `MemoryInitPeiLib` | Rewrite | MTK FDT and DRAM handling. |
| `PlatformMemoryMapLib` | Rewrite | Qualcomm-specific map. |
| Console / serial | Adapt | Framebuffer console first; MTK UART as a fallback. |
| UFS/USB/display/PMIC | Rewrite | Qualcomm binaries are unusable. |

---

## 3. MT6785 hardware baseline

### 3.1 Core platform

| Block | Address / value | Notes |
|---|---|---|
| CPU | 2x Cortex-A76 + 6x Cortex-A55 | 8 cores total |
| DRAM base | `0x40000000` | Linux/Android physical base |
| UART0 | `0x11002000` | 16550-like MTK UART, 32-bit registers, `regshift=2` |
| UART0 baud clock | 26 MHz | `clk26m` |
| UART0 bus gate | `infracfg + 0x80`, bit 22 | `0x10001080 = 1 << 22` to enable |
| GIC distributor | `0x0C000000`, size `0x40000` | GICv3 |
| GIC redistributor | `0x0C040000`, size `0x200000` | 8 frames, `0x20000` stride |
| Generic timer | 13 MHz | PPI 13/14 (secure/NS physical), 11/10 (virtual/hyp) |
| `topckgen` | `0x10000000` | clock muxes |
| `infracfg` | `0x10001000` | gates and resets |
| `apmixedsys` | `0x1000C000` | PLLs |
| `pio` / pinctrl | `0x10005000` | GPIO and pinmux |
| `pwrap` | `0x1000D000` | MT6359 PMIC wrapper |
| Watchdog | `0x10007000` | MTK WDT |
| Keypad | `0x10010000` | volume/power keys, SPI 75; disabled in mainline DTS |
| MMC0 | `0x11230000` + `0x11F50000` | internal eMMC/UFS-adjacent controller |
| MMC1 | `0x11240000` + `0x11E10000` | SD card controller |
| UFS HCI | `0x11270000`, size `0x2300` | UFS 2.x |
| UFS PHY | `0x11FA0000`, size `0xC000` | |
| USB3 / MTU3 | `0x11200000` | XHCI + MTU3 |
| Display mmsys | `0x14000000` | display pipeline |
| DSI0 | `0x14014000` | panel |
| DSI1 | `0x1401F000` | secondary |
| PMIC | MT6359 | via pwrap |

### 3.2 Interrupts

- GIC SPI numbers used by the SoC are in the DTS. Relevant ones:
  - watchdog: SPI 72
  - USB: SPI 96 / 97
  - MMC0: SPI 101
  - MMC1: SPI 102
  - UFS: SPI 104
  - pwrap: SPI 212
- EDK2 needs the GICv3 distributor/redistributor bases and the timer PPIs. It does not need every SPI until the corresponding driver is enabled.

### 3.3 UART programming

The MT6785 UART is a 16550-compatible block with MediaTek extensions. The Linux driver uses `UPIO_MEM32` and `regshift = 2`, so **all registers are 32-bit accesses at `base + (reg << 2)`**.

For 26 MHz and 921600 8N1:

```text
quot          = DIV_ROUND_UP(26000000, 256 * 921600) = 1
sample_count  = (26000000 / (921600 * 1)) - 1     = 27
sample_point  = (27 >> 1) - 1                     = 12
fraction      = ((26000000 * 100) / 921600) % 100 = 21
fraction_L    = fraction_L_mapping[2]              = 0x05
fraction_M    = fraction_M_mapping[2]              = 0x00
```

Register writes (offsets are byte offsets; access as 32-bit MMIO):

| Register | Offset | Value |
|---|---:|---:|
| `LCR` (with DLAB) | `0x0C` | `0x83` |
| `DLL` | `0x00` | `0x01` |
| `DLM` | `0x04` | `0x00` |
| `LCR` (8N1) | `0x0C` | `0x03` |
| `HIGHS` | `0x24` | `0x03` |
| `SAMPLE_COUNT` | `0x28` | `27` |
| `SAMPLE_POINT` | `0x2C` | `12` |
| `FRACDIV_L` | `0x54` | `0x05` |
| `FRACDIV_M` | `0x58` | `0x00` |
| `FCR` | `0x08` | `0x07` |
| `MCR` | `0x10` | `0x03` |

`THR = 0x00`, `LSR = 0x14`, `LSR.THRE = bit 5`, `LSR.DR = bit 0`.

Fallbacks to try if the output is garbled:

- `115200 8N1`: `HIGHS=0`, `DLL=14`, `DLM=0`, `SAMPLE_COUNT=0`, `SAMPLE_POINT=0xFF`, fractional registers `0`.
- Verify the UART0 pinmux and the 26 MHz clock gate before changing the divisor.

### 3.4 Watchdog

- Base `0x10007000`.
- `WDT_MODE` at `+0x00`, enable bit 0, key `0x22000000`.
- `WDT_LENGTH` at `+0x04`, key `0x8`.
- `WDT_RESTART` at `+0x08`, magic `0x1971`.
- For bring-up, either disable the WDT or pet it from a timer event. Do not leave it running unhandled; a reset loop looks like a firmware crash.

### 3.5 Debug console and input strategy with no UART

No UART access changes the bring-up plan more than any other constraint. A display gives output, but UEFI also needs **input** for an interactive shell. Plan for output and input separately.

#### Output: framebuffer first

The bootloader (LK) already initializes the panel to draw the boot logo. The cheapest output console is therefore the **framebuffer LK leaves behind**, using the same architecture as the MSM8998 port's `FrameBufferSerialPortLib` and `SimpleFbDxe`:

- a text renderer writing directly into the framebuffer (the MSM8998 library uses a 5x12 font);
- a GOP (`EFI_GRAPHICS_OUTPUT_PROTOCOL`) built on the same framebuffer for the UEFI console;
- no display-pipeline reinitialization in M0/M1.

What is missing is the framebuffer description: physical address, width, height, stride, and pixel format. The static stock DTB has an `mtkfb` node with no `reg`, so the address is almost certainly supplied at runtime (LK patches the DTB or passes a boot tag). Discovery plan:

1. Dump the runtime DTB and print every node/property whose name contains `fb`, `frame`, `display`, `disp`, `lcm`, `panel`, or `memory-region`.
2. Dump the `lk` partition (via `mtkclient`) and search for `mtkfb`, `lcm`, `fb_addr`, `boot_mode`, and display driver strings. The LK source for MT6785 is not in the local tree, so this is binary reverse engineering.
3. In the M0 payload, try a short list of candidate framebuffer addresses/formats and draw a test pattern. If the panel changes, the address is correct; record it in the plan and in a PCD. Candidate addresses should come from the LK strings and from the high end of the first DRAM window (the stock `/memory` ends at `0x7E605000`).
4. If the panel is already displaying the LK logo, the framebuffer is live. A correct write should visibly overwrite part of the logo.

**Important caveat:** LK may blank the panel or power down the display before jumping to the payload. If the screen is dark and framebuffer writes produce nothing, the framebuffer memory may still exist but the panel is off. In that case we need either LK's display-on sequence (dumped from `lk`) or the full mmsys/DSI/panel driver. This is the main reason to dump `lk` before M0, and it is a schedule risk, not a detail to discover late.

**Safety:** only probe addresses inside the DRAM window reported by the runtime FDT, and exclude the FD, UEFI region, FDT store, ramdisk, and runtime DTB. Do not probe MMIO or unknown carveouts; a wrong write can hang the interconnect or corrupt the bootloader's data. A framebuffer probe should be a bounded list of candidates, not a blind sweep.

Do **not** start by implementing the full MT6785 mmsys/DSI/panel pipeline. That is a multi-week task and is not needed if LK's framebuffer can be reused.

#### Input: volume/power keys first, USB keyboard later

A framebuffer console is output-only. The practical input ladder is:

1. **Volume-up / volume-down / power keys** via the MT6785 keypad block (`0x10010000`, currently disabled in the mainline DTS). A small `SimpleTextIn` driver can expose them as a few keys for a custom boot menu. This is the lowest-effort interactive path and should be P0.
2. **USB keyboard** via the MT6785 MTU3/XHCI host controller. This is the proper UEFI input path, but it depends on the USB/PHY driver and is P1.
3. **Touchscreen** via the panel's I2C/SPI touch controller. Useful for SimpleInit, but a larger driver effort; P2.
4. **UART or USB-UART mode**, if it can be recovered (see below); then the standard UEFI serial console works.

For M1, do not block on input. Put a `startup.nsh` in the FV that runs the diagnostic commands automatically (`memmap`, FDT dump, `reset`). The Shell output is visible on the framebuffer; the script drives the tests. Add key input when the menu is needed.

#### Parallel low-cost debug options

- **MTK USB-UART mode.** Strings in `dumped_preloader.bin` include `[USB] Switch to UART mode!!`, `UART META Connected, Switch log port to UART2...`, and `UART DYNAMIC SWITCH`. MediaTek preloaders can route a UART to the USB port in some configurations. Investigate whether a special USB cable/host tool can capture it. If it works, it restores a real serial console and is worth more than any other debug investment.
- **RAM debug buffer + pstore/ramoops.** The stock device already produces `console-ramoops`. A payload can write a structured log to a reserved DRAM page and then reboot to Android; if the page is inside the kernel's `ramoops` region, `/sys/fs/pstore/console-ramoops` may expose it. This requires locating the ramoops address/size (not present in the static DTB) and is a fallback, not the primary path.
- **`mtkclient` DRAM dump.** If a payload writes a known magic plus log to a fixed DRAM address and then hangs, a BROM/preloader dump may be able to read that page. This depends on `mtkclient` support and is best treated as a last resort.
- **UEFI USB serial gadget.** Configure MTU3 as a CDC-ACM device and use a host-side tool for output and input. This is the most complete no-UART console, but it requires a USB device stack and is a larger effort than the framebuffer probe; keep it as a fallback if the panel cannot be used.
- **Hardware UART discovery.** Even without documented test points, check the headphone jack, USB-C SBU pins, and unpopulated pads. A 1.8 V adapter is mandatory. This is a low-cost parallel check, not a dependency.

#### Recommended order

1. M0 framebuffer probe. If it draws, output is solved.
2. In parallel, dump `lk` and the runtime DTB; investigate USB-UART mode.
3. M1 scripted Shell on the framebuffer.
4. Add volume/power key input for the boot menu.
5. Add USB keyboard input when the USB stack is ready.

---

## 4. Boot image and memory layout

### 4.1 Stock boot image facts

The stock `current_boot.img` and `boot_backup_4.14.img` are Android boot image v2:

| Field | Value |
|---|---|
| `kernel_size` | `0xfd2abe` (`current_boot.img`) / `0x152f3dc` (`boot_backup_4.14.img`) |
| `kernel_addr` | `0x40080000` |
| `ramdisk_size` | `0x15c395` |
| `ramdisk_addr` | `0x47c80000` |
| `tags_addr` | `0x4bc80000` |
| `page_size` | `2048` |
| `header_version` | `2` |
| `dtb_size` | `0x2855e` (165214) |
| `dtb_addr` | `0x0` |

**Correction to earlier analysis:** the stock header stores `dtb_addr = 0`, not `0x4bc80000`. `tags_addr` is `0x4bc80000`. Do not assume LK honours `dtb_addr`.

### 4.2 The stock kernel is gzip + appended DTB

The stock kernel region starts with gzip magic `1f 8b`. Decompressing the first gzip member yields a ~44 MB arm64 `Image`, and the bytes after the gzip stream start with the FDT magic `d0 0d fe ed`. In other words:

```text
kernel_size = gzip(Image) + appended DTB
```

The separate `dtb` region after the ramdisk is a different 165214-byte DTB. In the image examined here:

- appended DTB: model `"Xiaomi Redmi Note 8 Pro"`, empty `bootargs`;
- separate DTB: model `"MT6785"`, stock `bootargs` (`console=tty0 console=ttyS0,921600n1 ...`).

This is a hybrid/repacked image. The safe conclusion is: **the runtime DTB that LK actually passes in `x0` is the only authoritative one.** The M0 payload must dump it.

### 4.3 Proposed UEFI memory layout

The first 1 GB of DRAM is the safe early-development window. The stock `/memory` node describes:

```text
0x40000000 + 0x3E605000
```

i.e. up to `0x7E605000` (~998 MB). The device physically has more DRAM, but LK/the runtime DTB is the source of truth for the rest.

Proposed fixed layout:

| Region | Start | Size / end | Purpose |
|---|---:|---:|---|
| Bootloader scratch | `0x40000000` | `0x80000` | tags / bootloader data |
| UEFI FD | `0x40080000` | `0x600000` | gzip decompress target |
| UEFI FV | `0x40088000` | `0x5F8000` | compressed FV |
| Free early DRAM | `0x40680000` | up to `0x41FFF000` | DXE allocations |
| FDT store | `0x41FFF000` | `0x1000` | 8-byte FDT pointer + reserved page |
| LK framebuffer | **TBD (M0)** | ~10 MB for 1080x2340x4 | reserve once discovered; must not overlap FD/UEFI/FDT/ramdisk |
| UEFI region | `0x42000000` | `0x02000000` | PrePi stacks / DxeCore / heap |
| Free DRAM | `0x44000000` | up to `0x47C80000` | conventional memory |
| Stock ramdisk | `0x47C80000` | up to `0x4BC80000` | reserve the 64 MB window (the actual ramdisk is ~1.4 MB) |
| Runtime DTB | `0x4BC80000` | up to `0x4BE00000` | reserve 2 MB |
| Everything else | `0x4BE00000` | to runtime `/memory` end | conventional, minus carveouts |

**Critical correction:** the earlier proposal of `PcdDeviceTreeStore = 0x400FF000` is wrong. That address lies **inside the FV** (`0x40088000..0x40680000`) and would overwrite firmware code. Use `0x41FFF000` (or another page outside the FD and outside the UEFI region), and reserve it in the memory map.

The `PcdSystemMemorySize` used by PrePi should be **64 MB** (`0x04000000`). This only controls the early PrePi window and stack placement (`0x40000000..0x44000000`). The full DRAM map is added later from the runtime FDT.

With `PcdSystemMemoryUefiRegionSize = 0x02000000` (32 MB), PrePi places the UEFI region at `0x42000000..0x44000000` and the primary stack at the top.

### 4.4 Proposed SimpleInit load ranges

The kernel, initrd, and DTB must be in the first 1 GB and outside the UEFI/FDT/ramdisk regions. Suggested PCD values:

| Region | Range | Notes |
|---|---|---|
| Kernel | `0x50000000..0x58000000` | SimpleInit aligns to 2 MB and adds `0x80000`, so the image lands at `0x50080000` |
| Initrd | `0x58000000..0x60000000` | 2 MB aligned |
| FDT | `0x60000000..0x60100000` | 1 MB |
| `load` fallback | `0x0` | unused when specific ranges are set |

These are inside the first 1 GB and below the stock `/memory` end (`0x7E605000`). They may overlap `alloc-ranges` reserved-memory nodes; verify with the runtime DTB and the UEFI memory map, and adjust if Linux reports conflicts. If the LK framebuffer discovered in M0 falls inside `0x50000000..0x60100000`, move the SimpleInit ranges (or the framebuffer reservation) so they do not overlap.

---

## 5. Proposed source tree

Mirror the reference layout, but keep MediaTek code separate from Qualcomm:

```text
Platforms/
  Mt6785Pkg/
    Mt6785.dsc
    Mt6785.fdf
    Device/
      xiaomi/
        begonia/
          begonia.dsc
          begonia.fdf
          DeviceTreeBlob/
            Linux/
              mt6785-xiaomi-begonia.dtb
  Mt6785FamilyPkg/
    Mt6785FamilyPkg.dec
    Mt6785FamilyPkg.dsc.inc
    Mt6785FamilyPkg.fdf.inc
    Include/
    Library/
      ArmPlatformLib/
        ArmPlatformLib.inf
        ArmPlatformLib.c
        ArmPlatformLibMem.c
        AArch64/
          ArmPlatformHelper.S
      Mt6785FbConLib/
        Mt6785FbConLib.inf
        Mt6785FbConLib.c
      Mt6785SerialPortLib/
        Mt6785SerialPortLib.inf
        Mt6785SerialPortLib.c
      PlatformMemoryMapLib/
        PlatformMemoryMapLib.inf
        PlatformMemoryMapLib.c
      Mt6785ClockLib/
      Mt6785GpioLib/
    MemoryInitPei/
      MemoryInitPeiLib.inf
      MemoryInitPeiLib.c
    Drivers/
      Mt6785Dxe/
      Mt6785KeypadDxe/
      Mt6785WatchdogDxe/
      Mt6785SdMmcDxe/
      Mt6785UfsDxe/
      Mt6785UsbPhyDxe/
      SimpleFbDxe/
Silicon/
  MediaTek/
    Mt6785/
      Include/
      Library/
      Drivers/
```

`Mt6785Pkg` is the platform/device package; `Mt6785FamilyPkg` is the family-level library/driver package; `Silicon/MediaTek/Mt6785` is the SoC package. This matches the `Msm8998Pkg` / `Msm8998FamilyPkg` / `QcomPkg` split.

---

## 6. PCD plan

### 6.1 FD/FV and memory

| PCD | Value | Notes |
|---|---:|---|
| `gArmTokenSpaceGuid.PcdFdBaseAddress` | `0x40080000` | must equal LK `kernel_addr` |
| `gArmTokenSpaceGuid.PcdFdSize` | `0x00600000` | 6 MB |
| `gArmTokenSpaceGuid.PcdFvBaseAddress` | `0x40088000` | FD base + `0x8000` |
| `gArmTokenSpaceGuid.PcdFvSize` | `0x005F8000` | 6 MB - `0x8000` |
| `gArmTokenSpaceGuid.PcdSystemMemoryBase` | `0x40000000` | DRAM base |
| `gArmTokenSpaceGuid.PcdSystemMemorySize` | `0x04000000` | 64 MB early PrePi window |
| `gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize` | `0x02000000` | 32 MB UEFI region |
| `gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize` | `0x00020000` | 128 KB |
| `gArmPlatformTokenSpaceGuid.PcdCPUCoreSecondaryStackSize` | `0x00010000` | 64 KB |
| `gSimpleInitTokenSpaceGuid.PcdDeviceTreeStore` | `0x41FFF000` | outside FD and UEFI region |

### 6.2 CPU, GIC, timer

| PCD | Value | Notes |
|---|---:|---|
| `gArmTokenSpaceGuid.PcdCoreCount` | `8` | 6x A55 + 2x A76 |
| `gArmTokenSpaceGuid.PcdClusterCount` | `2` | verify MPIDR affinity encoding |
| `gArmTokenSpaceGuid.PcdArmPrimaryCore` | `0` | verify boot CPU MPIDR |
| `gArmTokenSpaceGuid.PcdArmPrimaryCoreMask` | `0x0000FFFF` | verify; see MPIDR note |
| `gArmTokenSpaceGuid.PcdGicDistributorBase` | `0x0C000000` | GICv3 |
| `gArmTokenSpaceGuid.PcdGicRedistributorsBase` | `0x0C040000` | GICv3 |
| `gArmTokenSpaceGuid.PcdArmGicV3WithV2Legacy` | `FALSE` | |
| `gArmTokenSpaceGuid.PcdArmArchTimerIntrNum` | `14` | NS physical timer PPI |
| `gArmTokenSpaceGuid.PcdArmArchTimerVirtIntrNum` | `11` | virtual timer PPI |
| `gArmTokenSpaceGuid.PcdArmArchTimerFreqInHz` | `13000000` | fallback if CNTFRQ is 0 |

**MPIDR note:** the mainline DTS uses `reg = <0x000>, <0x100>, ..., <0x700>` for the eight CPUs. That is not the usual Aff0-only encoding, so do not assume `PcdArmPrimaryCoreMask = 0xFF` is correct. The M0 payload must print `MPIDR_EL1`; start with a single-core MP table and enable secondary cores only after the encoding is understood.

### 6.3 Framebuffer, UART, and console

The framebuffer is the primary console; UART is a fallback. The framebuffer address/stride/format are **TBD from the M0 probe**.

| PCD | Value | Notes |
|---|---:|---|
| `gMt6785TokenSpaceGuid.PcdMipiFrameBufferAddress` | **TBD (M0)** | physical address LK uses |
| `gMt6785TokenSpaceGuid.PcdMipiFrameBufferWidth` | `1080` | begonia panel |
| `gMt6785TokenSpaceGuid.PcdMipiFrameBufferHeight` | `2340` | begonia panel |
| `gMt6785TokenSpaceGuid.PcdMipiFrameBufferPixelBpp` | `32` | confirm ARGB8888/XRGB8888 in M0 |
| `gMt6785TokenSpaceGuid.PcdMipiFrameBufferStride` | **TBD (M0)** | often `width * 4`, but verify |
| `gMt6785TokenSpaceGuid.PcdMt6785UartBase` | `0x11002000` | UART0 (fallback only) |
| `gMt6785TokenSpaceGuid.PcdMt6785UartClock` | `26000000` | 26 MHz |
| `gMt6785TokenSpaceGuid.PcdMt6785UartBaud` | `921600` | stock console baud |
| `gEfiMdePkgTokenSpaceGuid.PcdUartDefaultBaudRate` | `921600` | fallback |
| `gEfiMdePkgTokenSpaceGuid.PcdUartDefaultDataBits` | `8` | |
| `gEfiMdePkgTokenSpaceGuid.PcdUartDefaultParity` | `0` | none |
| `gEfiMdePkgTokenSpaceGuid.PcdUartDefaultStopBits` | `1` | |
| `gEfiMdePkgTokenSpaceGuid.PcdDefaultTerminalType` | `4` | ANSI; `3` = VT-UTF8; UART only |
| `gEfiMdePkgTokenSpaceGuid.PcdUartDefaultReceiveFifoDepth` | `32` | |

### 6.4 SimpleInit

| PCD | Value |
|---|---:|
| `gSimpleInitTokenSpaceGuid.PcdBootCustomLoadAddress` | `TRUE` |
| `gSimpleInitTokenSpaceGuid.PcdBootLoadAddressStart` | `0x0` |
| `gSimpleInitTokenSpaceGuid.PcdBootLoadAddressEnd` | `0x0` |
| `gSimpleInitTokenSpaceGuid.PcdBootKernelAddressStart` | `0x50000000` |
| `gSimpleInitTokenSpaceGuid.PcdBootKernelAddressEnd` | `0x58000000` |
| `gSimpleInitTokenSpaceGuid.PcdBootRamdiskAddressStart` | `0x58000000` |
| `gSimpleInitTokenSpaceGuid.PcdBootRamdiskAddressEnd` | `0x60000000` |
| `gSimpleInitTokenSpaceGuid.PcdBootFdtAddressStart` | `0x60000000` |
| `gSimpleInitTokenSpaceGuid.PcdBootFdtAddressEnd` | `0x60100000` |

### 6.5 Other EDK2 PCDs

| PCD | Value | Notes |
|---|---:|---|
| `gEmbeddedTokenSpaceGuid.PcdPrePiProduceMemoryTypeInformationHob` | `TRUE` | |
| `gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize` | `44` | keep the reference value |
| `gEfiMdeModulePkgTokenSpaceGuid.PcdEmuVariableNvModeEnable` | `TRUE` | no real NVRAM yet |
| `gEfiMdeModulePkgTokenSpaceGuid.PcdResetOnMemoryTypeInformationChange` | `FALSE` | |
| `gEfiMdeModulePkgTokenSpaceGuid.PcdConOutGopSupport` | `TRUE` | display later |
| `gEfiMdeModulePkgTokenSpaceGuid.PcdConOutUgaSupport` | `FALSE` | |
| `gEfiMdePkgTokenSpaceGuid.PcdPlatformBootTimeOut` | `0` | no boot delay for bring-up |

---

## 7. Phase plan

### Phase 0 — Recon, backup, and the framebuffer probe payload

**Goal:** prove the boot chain, collect the runtime FDT, and establish **some** usable output console before writing EDK2 code.

Tasks:

1. Confirm the device is `begonia` / MT6785 and that the bootloader is unlocked.
2. Back up every relevant partition with `mtkclient` or fastboot:
   - `boot`, `dtbo`, `vbmeta`, `super`, `preloader`, `lk`, `tee`, `nvram`, `persist`, `userdata`.
   - Keep at least two copies off-device.
   - **`lk` is now a primary reverse-engineering target**, not just a backup: it contains the display/framebuffer initialization and the boot-tag format.
3. Dump and inspect:
   - the runtime DTB (see below). **If Android still boots and has root, pull `/sys/firmware/fdt` (or `/proc/device-tree`) immediately**; this gives the exact DTB LK passed without needing M0;
   - the `lk` partition for `mtkfb`, `lcm`, `fb_addr`, `boot_mode`, and display driver strings;
   - the preloader's USB/UART strings for the USB-UART debug mode.
4. Build a tiny AArch64 payload whose entry is at `0x40088000` (FD base + `0x8000`), with a Linux arm64 header at `0x40080000` that branches to it. It must:
   - preserves `x0` (DTB pointer) and records `x1..x3`;
   - validates the FDT magic `0xD00DFEED` and walks `totalsize`, `model`, `/memory`, `/reserved-memory`, `/chosen/bootargs`, `/psci/method`, and any framebuffer/display node;
   - records `MPIDR_EL1` and `CurrentEL`;
   - **probes the framebuffer**: for each candidate address/format derived from `lk` and the runtime DTB, draw a distinctive test pattern and a line of text. Stop at the first candidate that changes the panel;
   - if no framebuffer candidate works, write the same log to a fixed DRAM page with a magic header, then either hang for a `mtkclient` dump or reset to Android and try to recover it via `ramoops`/pstore;
   - optionally initialize UART0 if the USB-UART/hardware path is available (see §3.5); UART is not a dependency;
   - loop, or reset via PSCI `SYSTEM_RESET`.
5. Package the payload as a gzip "kernel" in a boot image v2, using the stock addresses and both DTB layouts (see §8). For this M0 payload only, set the header `text_offset` to `0x8000` so both `code1` and a `text_offset`-aware LK land at `0x40088000`; test the EDK2 header's `0x80000` value separately. Flash `boot` and observe the panel.
6. Repeat with the DTB only appended, and with the DTB only in the v2 `dtb` region, to determine which one LK passes in `x0`.

Acceptance:

- The panel visibly changes, or a framebuffer address/format is otherwise confirmed, or the log is recovered through the RAM/pstore fallback.
- `x0` points to a valid FDT.
- The runtime `/memory` and `/reserved-memory` values are recorded.
- `MPIDR_EL1` and the boot CPU's affinity encoding are recorded.
- The device can be rebooted/recovered.

If no output channel can be established, stop and solve that first. Building EDK2 without any console turns every later bug into a blind reset loop. The framebuffer probe, USB-UART mode, and RAM/pstore logging are the three viable paths; try them in that order.

### Phase 1 — Minimal EDK2 firmware, framebuffer console, and scripted Shell

**Goal:** boot a minimal EDK2 build that shows a UEFI banner and a scripted Shell on the phone's display.

Tasks:

1. Create `Mt6785Pkg` and `Mt6785FamilyPkg` skeletons from the MSM8998 files.
2. Implement the framebuffer console:
   - adapt the MSM8998 `FrameBufferSerialPortLib` (5x12 font, direct framebuffer writes) into `Mt6785FbConLib`;
   - set `PcdMipiFrameBufferAddress/Width/Height/PixelBpp` from the M0 findings;
   - implement `SimpleFbDxe` or an equivalent GOP producer on the same framebuffer;
   - add `GraphicsConsoleDxe` so the UEFI console renders through GOP.
3. Add minimal input:
   - `Mt6785KeypadDxe` for volume-up, volume-down, and power at `0x10010000`, exposed as `SimpleTextIn`;
   - a boot menu driven by those keys;
   - this is P0 because a framebuffer alone cannot interact with the Shell.
4. Add `Mt6785SerialPortLib` **only as a fallback**:
   - 32-bit MMIO at `base + (reg << 2)`;
   - UART gate enable at `0x10001080` bit 22;
   - baud setup as in §3.3;
   - polled TX/RX, FIFO enable, 8N1;
   - `SerialPortInitialize`, `SerialPortWrite`, `SerialPortRead`, `SerialPortPoll`, `SerialPortSetControl`, `SerialPortGetControl`, `SerialPortSetAttributes`.
   - Wire it in only when UART is actually accessible; otherwise leave it out of the console path.
5. Implement the MTK `ArmPlatformLib`:
   - single-core MP table initially;
   - early memory map for `0x40000000..0x44000000`;
   - MMIO mapping for the framebuffer, keypad, GIC, timer, pinctrl, infracfg, topckgen, pwrap, watchdog (and UART if used);
   - FD, DT store, and FDT region reserved;
   - map the PrePi UEFI region (`0x42000000..0x44000000`) in the MMU table. Do **not** add a second resource HOB for it: PrePi's `HobConstructor` already owns that region. Use `HobOnlyNoCacheSetting` or an equivalent "MMU-only" descriptor if the memory-map library supports it.
6. Implement `MemoryInitPeiLib`:
   - read the FDT pointer from `PcdDeviceTreeStore`;
   - validate the FDT magic;
   - fall back to an embedded DTB if invalid;
   - build HOBs for `/memory`, minus FD/UEFI/DT store/ramdisk/DTB;
   - add MMIO descriptors;
   - install the MMU table that includes DRAM, MMIO, the framebuffer, the FDT region, and the UEFI region.
7. Fix `ArmPlatformHelper.S` to store `x0` **before** the relocation check:

   ```asm
   ASM_FUNC(ArmPlatformPeiBootAction)
     mov  x10, FixedPcdGet64 (PcdDeviceTreeStore)
     str  x0, [x10]
     dsb  sy
     adr  x4, .
     ldr  x5, =ArmPlatformPeiBootAction
     cmp  x4, x5
     bne  docopy
     ret
   docopy:
     ... existing copy/relocation ...
   ```

8. Add a minimal `Mt6785WatchdogDxe` that disables or pets the WDT.
9. Build an FDF with the Linux header + FV. Include:
   - `DxeMain`, `PcdDxe`, `RuntimeDxe`, `CpuDxe`;
   - `ArmGicDxe`, `TimerDxe`, `MetronomeDxe`;
   - `SecurityStubDxe`, `CapsuleRuntimeDxe`, `EmbeddedMonotonicCounter`;
   - `ResetSystemRuntimeDxe`, `RealTimeClockRuntimeDxe` (virtual RTC);
   - `ConPlatformDxe`, `ConSplitterDxe`, `TerminalDxe`, `GraphicsConsoleDxe`;
   - `PrintDxe`, `DevicePathDxe`, `BdsDxe`;
   - `Shell`;
   - `VariableRuntimeDxe` (emulated NV);
   - `Mt6785Dxe`, `Mt6785KeypadDxe`, `SimpleFbDxe`;
   - `startup.nsh` with diagnostic commands;
   - optional `KernelFdtDxe` and `SimpleInit` later.
10. Build and package (see §8). Test:
    - UEFI banner appears on the panel.
    - `startup.nsh` runs `memmap`, FDT dump, and `reset -c` automatically.
    - volume/power keys move a simple menu selection.
    - `memmap` shows the FD, UEFI region, DT store, framebuffer, and conventional memory.
    - The device idles for at least 10 minutes without a watchdog reset.

Acceptance:

- Visible UEFI banner and Shell output on the phone display.
- A scripted Shell run completes without input.
- Volume/power key input works for at least a basic menu.
- No memory corruption or reset loop.
- The FDT store survives from PrePi to DXE.

### Phase 2 — FDT, memory map, and SimpleInit

**Goal:** expose the runtime FDT to UEFI applications and start SimpleInit.

Tasks:

1. Harden the FDT handoff:
   - validate magic and totalsize;
   - copy the FDT to a reserved buffer if LK's copy may be overwritten;
   - keep the original pointer for diagnostics;
   - embed a known-good `mt6785-xiaomi-begonia.dtb` in the FV as a fallback.
2. Make `MemoryInitPeiLib` parse:
   - `/memory` (all nodes, not just the first);
   - `/reserved-memory` children with fixed `reg`;
   - `alloc-ranges` entries without `reg`: log them, and treat the range as unavailable until M0 shows how LK patches them.
3. Replace `PlatformMemoryMapLib` with an MTK table:
   - all MMIO regions;
   - FD, FV, DT store, framebuffer, runtime DTB, ramdisk;
   - UEFI region and stacks;
   - no hardcoded 4G/6G/8G selection.
4. Add `KernelFdtDxe` (or reuse SimpleInit's) so `gKernelFdtProtocolGuid` is installed.
5. Add SimpleInit:
   - framebuffer/GOP console first, UART later if available;
   - configure `PcdBootCustomLoadAddress` as in §6.4;
   - verify it can read the FDT, list memory, and load a small UEFI test application.
6. If storage is not ready yet, embed a small UEFI test app in the FV and chainload it from Shell/SimpleInit.

Acceptance:

- A UEFI application can read the runtime FDT by GUID.
- `SimpleInit` starts and can print the DT model and memory map.
- `update_from_memory_map` produces a sane `/memory` node.
- No overlap between UEFI allocations and the FDT/ramdisk.

### Phase 3 — Storage, USB, PMIC

**Goal:** boot a real Linux kernel/initrd from a filesystem.

**Console caveat:** without UART, even a successfully booted Linux is output-blind unless the Linux display driver or a USB CDC-ACM gadget works. Treat the Linux console as a first-class M3 deliverable, not an afterthought; `pstore`/`ramoops` can capture a crash log but not an interactive shell.

**Display note:** the framebuffer/GOP console was already brought up in M1 by reusing LK's framebuffer. Only implement the MT6785 mmsys/DSI/panel pipeline if M0 shows LK's framebuffer is unusable; that is a separate, large task and is not on the critical path.

Priority order:

1. **SD card (MMC1) first.** It is the simplest practical boot medium.
   - Implement `Mt6785SdMmcDxe` as an `EFI_SD_MMC_PASS_THRU_PROTOCOL` producer.
   - Reuse EDK2 `MdeModulePkg/Bus/Sd/SdDxe` and `FatPkg`.
   - Implement clocks, pinmux, card detect, 4-bit/8-bit bus, and DMA/PIO.
   - Reference: Linux `drivers/mmc/host/mtk-sd.c` and LK's MSDC driver.
2. **USB (MTU3 + XHCI + TPHY) next.**
   - Enables a USB keyboard for interactive UEFI Shell/menus and fastboot for development.
   - Consider EDK2's `NonDiscoverablePciDeviceDxe` to reuse `XhciDxe`, or write a platform XHCI driver.
   - Fastboot over USB is useful but optional.
3. **UFS later.**
   - Implement clocks, PHY, resets, and PMIC rails.
   - If the controller is UFSHCI-compliant, reuse `MdeModulePkg/Bus/Ufs/UfsPassThruDxe` and `UfsBlockIoPei`.
   - Reference: Linux `drivers/ufs/host/ufs-mediatek.c` and `ufs-mtk.c`.
4. **PMIC (MT6359).**
   - pwrap at `0x1000D000`.
   - Needed for UFS/SD rails, display, USB, RTC, and battery.
   - Start by relying on LK's regulator setup; add only the rails that fail.
5. **Touchscreen.**
   - Optional; needed for SimpleInit GUI and recovery menus. Volume/power key input already exists from M1.

Acceptance:

- `map -r` shows a FAT32 SD card as `FS0:`.
- `ls FS0:\` works.
- SimpleInit loads `Image` and `initrd` from the card.
- Linux boots to a shell (UART if available; otherwise the panel via the Linux `mtkfb`/DRM driver, which is a separate Linux enablement task).
- The firmware's framebuffer console works up to the Linux handoff; Linux display bring-up is tracked separately.

### Phase 4 — Linux/Android enablement and XNU assessment

**Goal:** make the platform useful and decide whether XNU is worth pursuing.

Tasks:

1. Boot mainline Linux with the provided DTB and verify:
   - UART, GIC, timers, PSCI, CPU idle;
   - SD, UFS, USB, display, touch, WiFi/BT as available.
2. Add a boot menu, fastboot, and recovery entry.
3. Document the exact DTB/cmdline that works.
4. XNU feasibility spike (separate project):
   - audit `pexpert/arm64`, `osfmk/arm64`, and the platform drivers XNU expects;
   - determine whether a non-Apple platform port is realistic;
   - do not promise XNU support as part of this UEFI port.

Acceptance:

- Linux boots reliably from UEFI on the device.
- The firmware can be rebuilt and reflashed without a host-side rescue every time.
- The XNU decision is documented with concrete gaps.

---

## 8. Build and packaging

### 8.0 Prerequisites

- An EDK2 checkout compatible with the reference (the reference pins its own `edk2` and `edk2-platforms` revisions).
- An AArch64 GCC toolchain (the reference uses `GCC5`).
- `python3`, `iasl`, `dtc`, `gzip`, and the reference's `mkbootimg.py`.
- `mtkclient` and `fastboot` for backup, flashing, and recovery.
- A stable copy of the reference repos under `/media/dev/work/refs` (see §1.6).

### 8.1 FDF layout

Start from the MSM8998 FDF and change the base/size:

```text
[FD.MT6785PKG_UEFI]
BaseAddress   = 0x40080000|gArmTokenSpaceGuid.PcdFdBaseAddress
Size          = 0x00600000|gArmTokenSpaceGuid.PcdFdSize
ErasePolarity = 1
BlockSize     = 0x00001000
NumBlocks     = 0x600

0x00000000|0x00008000
DATA = {
  0x01, 0x00, 0x00, 0x10,                         # code0: adr x1, .
  0xff, 0x1f, 0x00, 0x14,                         # code1: b 0x8000
  0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, # text_offset: 512 KB
  0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, # image_size: 6 MB
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, # flags
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, # res2
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, # res3
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, # res4
  0x41, 0x52, 0x4d, 0x64,                         # magic: "ARM\x64"
  0x00, 0x00, 0x00, 0x00                          # res5
}

0x00008000|0x005F8000
gArmTokenSpaceGuid.PcdFvBaseAddress|gArmTokenSpaceGuid.PcdFvSize
FV = FVMAIN_COMPACT
```

Change `image_size` from the reference's 2 MB to the actual FD size (`0x600000`). If LK uses `image_size` to bound the decompressed payload, a 2 MB value would truncate the firmware.

**Note on `text_offset`:** the reference keeps `text_offset = 0x80000` even though the FV starts at `0x8000`. That works because LK follows `code1` (`b 0x8000`) and the FD layout, not the `text_offset` field. If the MT6785 LK validates `text_offset`, either move the FV to `0x80000` (and update `PcdFvBaseAddress`) or set `text_offset = 0x8000` and keep the branch target consistent. Verify this in M0 before relying on it.

### 8.2 Build script changes

From the reference `build.sh`:

- Replace `MSM8998PKG_UEFI.fd` with `MT6785PKG_UEFI.fd`.
- Replace the DSC path with `Platforms/Mt6785Pkg/Device/xiaomi/begonia/begonia.dsc`.
- Replace the DTB path with `Platforms/Mt6785Pkg/Device/xiaomi/begonia/DeviceTreeBlob/Linux/mt6785-xiaomi-begonia.dtb`.
- Keep the gzip + appended-DTB step for the stock-compatible layout.
- Switch from header v1 to header v2 and add the DTB.

Proposed packaging:

```bash
echo > ramdisk

gzip -c workspace/Build/begonia/RELEASE_GCC5/FV/MT6785PKG_UEFI.fd \
  > workspace/uefi-begonia.img.gz

cat workspace/uefi-begonia.img.gz \
    Platforms/Mt6785Pkg/Device/xiaomi/begonia/DeviceTreeBlob/Linux/mt6785-xiaomi-begonia.dtb \
  > workspace/uefi-begonia.img.gz-dtb

python3 ./mkbootimg.py \
  --kernel workspace/uefi-begonia.img.gz-dtb \
  --ramdisk ramdisk \
  --dtb Platforms/Mt6785Pkg/Device/xiaomi/begonia/DeviceTreeBlob/Linux/mt6785-xiaomi-begonia.dtb \
  --base 0x40000000 \
  --kernel_offset 0x00080000 \
  --ramdisk_offset 0x07C80000 \
  --tags_offset 0x0BC80000 \
  --dtb_offset 0x0BC80000 \
  --header_version 2 \
  --pagesize 2048 \
  --os_version 12.0.0 \
  --os_patch_level 2022-08 \
  -o boot-begonia.img
```

This produces:

- `kernel_addr = 0x40080000`
- `ramdisk_addr = 0x47C80000`
- `tags_addr = 0x4BC80000`
- `dtb_addr = 0x4BC80000`

The stock image has `dtb_addr=0`, but setting it to `0x4BC80000` makes the v2 field explicit. If LK ignores it, the appended DTB still provides a fallback. The M0 test determines which path works; simplify afterwards.

### 8.3 Flashing and recovery

- Back up `boot`, `dtbo`, `vbmeta`, `preloader`, `lk`, and `super` before the first flash.
- If AVB is enforced, use a development-only `vbmeta` with verification disabled, or flash a signed/appropriately configured image. Do not disable verification on a daily driver.
- Flash only the `boot` partition for the payload tests.
- Keep `mtkclient` and the preloader backup available for BROM/Preloader recovery.
- Verify the active slot before flashing (`fastboot getvar current-slot`).

---

## 9. Driver inventory and effort

| Driver / library | Source | Effort | Priority |
|---|---|---:|---|
| `Mt6785FbConLib` + `SimpleFbDxe`/GOP | adapt MSM8998 `FrameBufferSerialPortLib`/`SimpleFbDxe` | 2-5 days | P0 |
| `Mt6785KeypadDxe` (volume/power keys) | new, minimal | 1-2 days | P0 |
| `ArmPlatformLib` (single core) | adapt reference | 2-3 days | P0 |
| `MemoryInitPeiLib` + `PlatformMemoryMapLib` | rewrite | 3-5 days | P0 |
| `Mt6785WatchdogDxe` | new, small | 0.5-1 day | P0 |
| FDT handoff / fallback DTB | adapt reference | 1-2 days | P0 |
| `Mt6785SerialPortLib` | new, based on 8250 MTK | 1-2 days | P1 (fallback) |
| `Mt6785SdMmcDxe` | new, Linux/LK reference | 1-2 weeks | P1 |
| `Mt6785UsbPhyDxe` + XHCI (USB keyboard) | new/adapt EDK2 | 1-2 weeks | P1 |
| `Mt6785ClockLib` | new, minimal gates/muxes | 3-5 days | P1 |
| `Mt6785GpioLib` / pinctrl | new, minimal | 3-5 days | P1 |
| `Mt6785UfsDxe` | new, Linux reference | 2-4 weeks | P2 |
| MT6359 pwrap/regulators | new, Linux reference | 1-2 weeks | P2 |
| Touchscreen | new | 1-2 weeks | P2 |
| ACPI | not planned; use DT | - | - |
| XNU platform port | separate project | months+ | out of scope |

The estimates assume a working backup/recovery path and a usable framebuffer. Hardware debugging can dominate, especially without UART; the framebuffer probe is the schedule-critical item, and the USB keyboard path is the gate for an interactive Shell.

---

## 10. Risks and mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **No UART access** | Certain | High | Framebuffer console first; investigate MTK USB-UART mode; RAM/pstore fallback; optional 1.8 V UART discovery. |
| LK framebuffer address/format unknown | High | High | Dump runtime DTB and `lk`; probe candidate addresses/formats with a test pattern; reuse LK's initialized display, do not reimplement mmsys/DSI yet. |
| LK blanks/powers down the panel before handoff | Medium | High | Dump `lk`'s display-on sequence; if needed, re-run the minimal DSI/panel init from the payload; USB-UART/pstore as fallback. |
| No input device for UEFI Shell | Certain initially | Medium | Scripted `startup.nsh` for output-only tests; volume/power key driver for menus; USB keyboard later. |
| Linux console unavailable without UART | High | High | Prioritize the Linux display driver or a USB CDC-ACM gadget; use `pstore`/`ramoops` for post-mortem logs; keep hardware UART/USB-UART as a fallback. |
| Framebuffer overwritten by LK/Android or wrong format | Medium | High | Probe early; record exact address/stride/format; reserve the framebuffer in the UEFI memory map. |
| Bootloader rejects unsigned/AVB boot image | High | High | Unlock/disable verification on a development device; keep stock boot backup. |
| LK does not accept a gzip UEFI FD or the Linux header | Medium | High | M0 payload; try raw and gzip; try both DTB layouts. |
| `dtb_addr=0` means LK ignores the v2 DTB | High | High | M0 dumps `x0`; use the appended DTB as fallback. |
| Runtime `/memory` is only ~998 MB or is patched | High | Medium | Dump the runtime FDT; use the first 1 GB for bring-up. |
| Reserved-memory carveouts overlap load addresses | High | High | Parse `/reserved-memory`; reserve `reg` entries; validate SimpleInit ranges under Linux. |
| UART baud/clock wrong (if UART becomes available) | Medium | Medium | Use the exact MTK 8250 sequence; try 115200 fallback; check the 26 MHz gate. |
| Watchdog reset loop | Medium | High | Disable/pet the WDT in the first DXE driver. |
| FD too small for storage/display stacks | Medium | Medium | Start at 6 MB; grow to 8/12 MB and update `PcdFdSize`, FV size, and `image_size`. |
| `PcdDeviceTreeStore` inside FV | Certain if using `0x400FF000` | High | Use `0x41FFF000`; reserve the page. |
| FDT outside the MMU map | High | High | Map `0x4BC00000..0x4BE00000`; reserve the FDT region. |
| Qualcomm prebuilt drivers are unusable | Certain | High | Write MTK drivers; do not attempt to link Qcom binaries. |
| XNU cannot boot from this firmware | Certain | High | Treat XNU as a separate research project; Linux is the deliverable. |

---

## 11. Open decisions

1. **Target:** confirmed — `begonia` / MT6785.
2. **Payload:** confirmed — Linux first. Android/XNU remain later workstreams.
3. **UART access:** confirmed — none. The open question is whether **MTK USB-UART mode** works on this preloader; that is an investigation, not a dependency.
4. **Framebuffer:** can LK's framebuffer be reused, and what are its address/stride/format? This is the M0 decision gate.
5. **Input:** volume/power keys only for M1, or invest in USB keyboard early?
6. **Boot medium:** SD card first (recommended) or UFS first?
7. **FD size:** 6 MB for the first milestone, or reserve 8/12 MB now?
8. **Display pipeline:** only if LK's framebuffer is unusable. Reimplementing mmsys/DSI/panel is a separate, much larger project.

---

## 12. Immediate next steps

1. Copy the reference repos to a stable path under `/media/dev/work/refs`.
2. Back up all partitions, **including `lk`**, and verify recovery with `mtkclient`.
3. Dump the runtime DTB (from a rooted Android boot via `/sys/firmware/fdt`, or from the M0 payload) and `lk`; search for framebuffer/display constants and the LK boot-tag format.
4. Build and run the M0 **framebuffer probe** payload; record:
   - whether the panel changes, and the winning framebuffer address/stride/format;
   - `x0`, `x1`, `x2`, `x3`;
   - `MPIDR_EL1`, `CurrentEL`;
   - FDT model, `/memory`, `/reserved-memory`, `/chosen/bootargs`, `/psci/method`;
   - which DTB layout LK uses.
5. In parallel, test MTK USB-UART mode and the RAM/pstore fallback. If either works, it becomes the secondary console.
6. Only after M0 produces visible output (or a recoverable log), create `Mt6785Pkg` and implement the framebuffer console + keypad input + minimal FDF/DSC.
7. Reach a visible UEFI banner and a scripted Shell on the panel before touching storage or USB. Add an interactive Shell only after keypad or USB keyboard input works.

---

## Appendix A — Reference file mapping

| MSM8998 reference | MT6785 target | Change |
|---|---|---|
| `Platforms/Msm8998Pkg/Msm8998.dsc` | `Platforms/Mt6785Pkg/Mt6785.dsc` | new PCDs, MTK libs |
| `Platforms/Msm8998Pkg/Device/xiaomi/sagit/sagit.dsc` | `Platforms/Mt6785Pkg/Device/xiaomi/begonia/begonia.dsc` | device name, panel, PCDs |
| `Platforms/Msm8998Pkg/Device/xiaomi/sagit/sagit.fdf` | `.../begonia/begonia.fdf` | FD base/size, drivers |
| `Platforms/Msm8998FamilyPkg/Msm8998FamilyPkg.dsc.inc` | `Platforms/Mt6785FamilyPkg/Mt6785FamilyPkg.dsc.inc` | MTK libs, PCDs |
| `Platforms/Msm8998FamilyPkg/Msm8998FamilyPkg.fdf.inc` | `Platforms/Mt6785FamilyPkg/Mt6785FamilyPkg.fdf.inc` | MTK drivers |
| `Platforms/Msm8998FamilyPkg/Library/ArmPlatformLib/` | `Platforms/Mt6785FamilyPkg/Library/ArmPlatformLib/` | rewrite memory map/MP table |
| `Platforms/Msm8998FamilyPkg/MemoryInitPei/` | `Platforms/Mt6785FamilyPkg/MemoryInitPei/` | parse MTK FDT |
| `Platforms/Msm8998FamilyPkg/Library/FrameBufferSerialPortLib/` | `.../Mt6785FbConLib/` | adapt framebuffer console, not UART |
| `Silicon/QC/Msm8998/QcomPkg/Library/PlatformMemoryMapLib/` | `Silicon/MediaTek/Mt6785/Mt6785Pkg/Library/PlatformMemoryMapLib/` | MTK map |
| `build.sh` | `build-mtk.sh` or `--soc mt6785` | header v2, new DTB path |
| `Config/sagit.conf` | `Config/begonia.conf` | device metadata |

## Appendix B — Useful commands

```bash
# List the boot image header (header v2)
python3 - boot-begonia.img <<'PY'
import struct, sys
p = sys.argv[1]
d = open(p, 'rb').read(4096)
k, ka, r, ra, s, sa, t, ps, hv, osv = struct.unpack_from('<10I', d, 8)
print('magic', d[:8], 'hv', hv, 'ps', ps)
print('kernel', hex(k), hex(ka), 'ramdisk', hex(r), hex(ra), 'tags', hex(t))
if hv >= 2:
    print('dtb_size', hex(struct.unpack_from('<I', d, 1648)[0]),
          'dtb_addr', hex(struct.unpack_from('<Q', d, 1656)[0]))
PY

# Decompress the first gzip member and inspect the appended DTB
python3 - <<'PY'
import struct, zlib
p = 'boot-begonia.img'
d = open(p, 'rb').read()
k, ka, r, ra, s, sa, t, ps, hv, osv = struct.unpack_from('<10I', d, 8)
raw = d[ps:ps + k]
o = zlib.decompressobj(16 + zlib.MAX_WBITS)
out = o.decompress(raw)
print('decompressed', len(out), 'trailing', len(o.unused_data))
print('first bytes', out[:16].hex())
print('trailing magic', o.unused_data[:4].hex())
PY
```

---

*End of plan.*
