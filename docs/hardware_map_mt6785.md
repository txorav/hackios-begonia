# Canonical Hardware & Boot-Contract Reference — Xiaomi Redmi Note 8 Pro (`begonia`)

This is the **single source of truth** for the HackiOS MT6785 (`begonia`) port.
Every component (`xnuloader`, `mkxnuimg.py`, XNU port in `xnu/`, platform kexts in
`kexts/`, Linux-to-IOKit shims in `wifi/lx/`, userland scripts) must agree with
the values on this page. If a value changes during device bring-up, update it
here first, then propagate.

Everything in this document is derived from the porting plan
(`hackios_porting_plan_for_redmi_note_8_pro_begonia.md` §1) plus inspection of
the in-tree XNU sources (`xnu/pexpert/pexpert/arm64/boot.h`,
`xnu/osfmk/arm64/arm_vm_init.c`, `xnu/osfmk/mach/arm/vm_param.h`).

---

## 1. SoC identity

| Item | Value |
| :--- | --- |
| Board | Xiaomi Redmi Note 8 Pro, codename `begonia` |
| SoC | MediaTek Helio G90T (`MT6785V/CC`), `mt6785` |
| ISA | ARMv8.2-A. **No PAC (`arm64e`), no APRR/PPL regs, no Apple HID0–HID16, no AMX, no CPM, no Apple Fast-IPI** |
| CPU big | 2 × Cortex-A76 @ 2.05 GHz |
| CPU little | 6 × Cortex-A55 @ 2.00 GHz |
| MMU granules | 4 KiB, 16 KiB, 64 KiB (`ID_AA64MMFR0_EL1.TGran16` supported) — **XNU uses 16 KiB** |
| GIC | ARM GICv3: `GICD` @ `0x0c000000` (~256 KiB region), `GICR` base `0x0c040000` (per-CPU redistributor, 0x20000 stride for 8 CPUs) |
| Generic timer | Architectural timer; MediaTek frequency **13 MHz** (read `CNTFRQ_EL0` at runtime — never hardcode Apple's 24 MHz) |
| Storage | UFS 2.1 `ufshcd-mtk` @ `0x11270000`; MicroSD `msdc1` @ `0x11240000` |
| UART (debug) | UART0 @ `0x11002000` (16550-style, 4-byte reg stride) |
| I2C (touch) | I2C @ `0x11007000`, touchscreen `focaltech,fts` (alt: `goodix,gt9886`) at addr `0x38` |
| Display | 1080×2340 IPS LCD, MIPI-DSI, framebuffer pre-initialized by MediaTek `lk` bootloader |
| Boot ROM / firmware | MediaTek BROM + `lk` (little kernel) + ARM Trusted Firmware BL31 (PSCI ≥ 0.2) |

### MPIDR topology (`Heterogeneous` — must map to logical CPUs 0..7)

| Logical CPU | Cluster | MPIDR_EL1 (`Aff2:Aff1:Aff0`, Lawrence) | Core |
| --- | --- | --- | --- |
| 0 | 0 | 0x000 | A55 (primary boot core) |
| 1 | 0 | 0x001 | A55 |
| 2 | 0 | 0x002 | A55 |
| 3 | 0 | 0x003 | A55 |
| 4 | 0 | 0x004 | A55 |
| 5 | 0 | 0x005 | A55 |
| 6 | 1 | 0x100 | A76 |
| 7 | 1 | 0x101 | A76 |

PSCI (`BL31`, EL3):
* `PSCI_VERSION` = `0x84000000`
* `CPU_ON` = `0xC4000003` — x1 = target MPIDR, x2 = entry point (phys), x3 = context id
* `CPU_OFF` = `0x84000002`, `SYSTEM_RESET` = `0x84000009`, `SYSTEM_OFF` = `0x84000008`
* Invoke with `smc #0`.

---

## 2. DRAM map (physical addresses)

`PHYS_BASE` (low DRAM start): **`0x40000000`**. Total DRAM 4 GiB
(some units ship 6 GiB — `memSize` is taken from the FDT `/memory` node at boot).

| Range | Owner | Notes |
| :--- | --- | --- |
| `0x40000000 – 0x4007FFFF` | lk / buckhead scratch | Below our loader load address |
| `0x40080000 – 0x4017FFFF` | **xnuloader** (1 MiB) | `boot.img` kernel load address: `base 0x40078000 + kernel_offset 0x00008000` |
| `0x40200000 – ~0x41E00000` | **XNU kernelcache segments** | Segment PAs derive from their Mach-O vmaddres (`phys = vmaddr − virtBase + physBase`) |
| above `topOfKernelData` | XNU free pool | `arm_vm_init` sets `avail_start = topOfKernelData` |
| `0x47c00000 – 0x47FFFFFF` | **Reserved carveout (ATF/SCP/SSPM/TEE)** | never map into XNU `pmap_MemRegions` — EL3 SError otherwise (verify on device via `/proc/iomem`) |
| `0x47df0000 – 0x47e00000` | **ramoops / panic ring** | 64 KiB, survives warm (watchdog) reset; loader writes `XLDRPANIC` records |
| `0x7f000000 (~end of DRAM)` | **`lk` framebuffer** | RGBX8888 linear, 1080×2340, rowBytes **4320** |
| tags region | `0x4bc08000` | `base 0x40078000 + tags_offset 0x0bc08000` (Android boot protocol) |

> `framebuffer`/`carveout` addresses must be re-verified on live hardware
> (Phase-1 checklist: `/proc/iomem`, `/sys/firmware/fdt`). The loader accepts
> overrides via the payload manifest so bring-up can iterate without rebuilds.

### Loader/XNU shared boot contract

* `lk` → `boot.img` → kernel image loaded at `0x40080000`, X0 = FDT physical
  address, typically EL2 (we flatten to EL1).
* `xnuloader` builds:
  1. relocated Mach-O kernelcache (PA derived as above),
  2. **Apple DeviceTree (ADT)** at `ADT_BASE` (aligned 32 KiB),
  3. optional RAMDisk (`rd=md0`) right after the ADT (32 KiB aligned),
  4. **`boot_args`** last (64-byte aligned), then
  5. `topOfKernelData = end of boot_args region (page-aligned)`.
* `boot_args.physBase = 0x40000000`, `memSize` from FDT,
  `virtBase = VM_MIN_KERNEL_WINDOW + physBase` where the linker-window is
  `0xffffffe000000000` (xnu‑8796 arm64, non‑KTRR build →
  `osfmk/mach/arm/vm_param.h`: `VM_MIN_KERNEL_ADDRESS = 0xffffffe000000000`,
  `VM_MAX_KERNEL_ADDRESS = 0xfffffffbffffffff`).
* XNU entry: `_start` (`osfmk/arm64/start.s`, `LowResetVectorBase`) — called at
  **EL1**, MMU off, D-cache clean + I-cache invalidated, X0 = physical address
  of `boot_args`. No Apple-proprietary `S3_x_c15` registers may be touched
  before XNU's own code runs.
* DeviceTree + RAMDisk + boot_args live **below** `topOfKernelData` (they are
  inside the kernel-data region; the kernel reads them via `/chosen/memory-map`
  through SecureDT — see `arm_vm_init.c`).

`boot_args` struct (exact, from `xnu/pexpert/pexpert/arm64/boot.h`):
`Revision(2)=kBootArgsRevision2(2) Version(2)=kBootArgsVersion2(2) virtBase physBase memSize topOfKernelData Boot_Video{v_baseAddr,v_display,v_rowBytes,v_width,v_height,v_depth} machineType deviceTreeP deviceTreeLength CommandLine[BOOT_LINE_LENGTH] bootFlags memSizeActual`.

---

## 3. Apple DeviceTree (ADT) template — produced by `xnuloader/fdt2adt`

Root props: `#address-cells=<1>` `#size-cells=<1>` `revision=<5 25>` (D22/A11-era format),
`compatible=<"Xiaomi,begonia","mtk,mt6785">`, `model=<"Redmi Note 8 Pro">`.

* `/chosen` — `bootargs`, randseed (16 B, entropy from packaging host), `chip-id=<0x8020>` (A11 Bionic class),
  `board-id=<8-byte "D22AP\0\0\0">`, chip-revision, `ramoops-phys=<0x47df0000>`,
  `framebuffer=<base size>` passthrough, `serial-number`/`unique-chip-id` (fake, Phase-5).
* `/chosen/memory-map` — child entry per region: `reg = <phys_hi phys_lo size>`:
  `DeviceTree`, `RAMDisk`, `FrameBuffer`, `PanicRings`, `RamBootDebug`.
* `/cpus` — `cpu0..cpu7`, each: `device-type=<"cpu">`, `reg=<cluster-idx>`,
  `cpu-id=<logical>`?? no — each `reg=<MPIDR>` (0x000..0x101),
  `clock-frequency=<2000000000|2050000000>`, `compatible=<"arm,cortex-a55"|"arm,cortex-a76">`,
  `state=<"running"| "on">`, cluster-affinity.
* `/arm-io` — MMIO children (32-bit cells):
  * `gic`   : `reg=0x0c000000 size=0x40000`, `redistributor=0x0c040000 size=0xC0000`, `interrupt-type=<"gicv3">`
  * `uart0` : `reg=0x11002000 size=0x1000`, `interrupt=<UART0_SPI>` (SPI fallback 65, verify on device)
  * `msdc1` : `reg=0x11240000 size=0x10000`, `interrupt=MSDC1_SPI`, `bus-width=<8>`
  * `ufs0`  : `reg=0x11270000 size=0x100000`, `interrupt=UFS_SPI`
  * `i2c0`  : `reg=0x11007000 size=0x1000`
  * `touchscreen` : `i2c-address=<0x38>`, `interrupt=TS_PI`, `compatible=<"focaltech,fts">`
  * `disp-fb`     : `reg=<fb_base> size=<0x9a5c00>`, width 1080, height 2340, stride 4320, depth 32, format RGBX8888
* `/product` — `product-name=<"iPhone10,3">`, `board-name=<"D22AP">`, `product-type=<"iPhone10,3">`, fake MLB/Serial (Phase-5).

Any property marked "verify on device" is provisional; the FDT passed by `lk`
remains authoritative for DRAM size and where it conflicts, FDT wins.

---

## 4. Kernel command line (canonical for M2/M3 bring-up)

```
serial=3 -v debug=0x8e keepsyms=1 panicdebug=1 rd=md0 rootdev=md0 \
cs_enforcement_disable=1 amfi_get_out_of_my_way=0x1 wpdebug=1 \
serial_baud=115200 panic_button=1
```

## 5. Android `boot.img` packaging parameters (`mkxnuimg.py`)

```
--base 0x40078000 --kernel_offset 0x00008000 --ramdisk_offset 0x07c08000 \
--tags_offset 0x0bc08000 --pagesize 2048 --header_version 2
```
kernel image = gzip( payload{xnuloader.bin, adt.bin, kernelcache.macho, ramdisk.img, cmdline} );
`lk` guarantees the kernel lands at `0x40080000` with the ARM64 image header
magic `ARM\x64` (0x644d5241) already handled by `xnuloader/image64.S`.

## 6. Ownership (directory) map

| Path | Owner |
| --- | --- |
| `xnuloader/`, `mkxnuimg.py`, `xl`, top `Makefile`, `docs/`, `scripts/`, root `README.md` | core (porting plan execution) |
| `xnu/**` (kernel edits + kernld + platform glue) | kernel-port |
| `kexts/MT6785GICv3`, `MT6785PlatformExpert`, `AMFIStub`, `SEPStub`, `WatchDogStub` | kernel-port |
| `kexts/LxStorage`, `FocalTouchIOHID`, `MT6785Framebuffer` | shims |
| `wifi/lx/**` | shims |
| `userland/**`, `tools/**`, `uefi/**` | userland |
| `xnu-deps/**` | deps |
