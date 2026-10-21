# TODO: HackiOS Begonia Porting & Evaluation Roadmap

Comprehensive master task list and engineering plan for porting Darwin (XNU 8796.121.2 + iOS 16.5 userland) to the **Redmi Note 8 Pro** (`begonia` / MediaTek Helio G90T `MT6785`, ARMv8.2-A).

> **Status Legend**:
> - `[x]` Completed & verified
> - `[~]` In progress / scaffolded / pending on-device validation
> - `[ ]` Pending implementation
> - `[!]` Critical risk / blocker / requires specific mitigation

---

## 1. Executive Summary & Architectural Reality Check

| Dimension | Target Profile | Architectural Impact |
| :--- | :--- | :--- |
| **SoC / ISA** | MediaTek Helio G90T (`MT6785`), ARMv8.2-A | **No Pointer Authentication (`PAC` / `arm64e`)**. Must strictly target plain `arm64` (`iPhone10,3` / A11 userland). |
| **MMU Granule** | $16\text{ KiB}$ supported by Cortex-A76 & Cortex-A55 (`TGran16`) | iOS 16 userland frameworks (`dyld_shared_cache_arm64`) align to $16\text{ KiB}$. XNU `pmap` must run with $16\text{ KiB}$ page tables. |
| **Interrupts** | ARM GICv3 (`0x0c000000`) vs Apple AIC | AIC replaced by `MT6785GICv3` IOInterruptController driver and ICC system registers (`ICC_SRE_EL1`, `ICC_IAR1_EL1`, `ICC_EOIR1_EL1`). |
| **Timers** | Architectural Timer @ $13\text{ MHz}$ (`CNTFRQ_EL0`) | Apple's hardcoded $24\text{ MHz}$ crystal frequency must be replaced by dynamic `CNTFRQ_EL0` read. |
| **SMP Wakeup** | Heterogeneous cluster ($6\times\text{A55} + 2\times\text{A76}$) | Replaces Apple PMGR with standard ARM PSCI `CPU_ON` (`0xC4000003`) into MediaTek BL31 ($\text{EL3}$). |
| **Graphics** | Mali-G76 MC4 vs Apple AGX | No hardware Metal/AGX driver exists. UI requires linear scanout fallback (`IOMobileFramebuffer`) and software rasterization. |

---

## 2. Milestone Track (M1 → M6)

```
[M1: Stage-2 Loader]  ---> [M2: Early XNU Banner] ---> [M3: GICv3 & SMP]
        |                           |                         |
        v                           v                         v
[M4: Single-User Shell] ---> [M5: Stock launchd]  ---> [M6: LockScreen & Touch]
```

### Milestone M1: Stage-2 Bare-Metal Loader (`xnuloader` + `mkxnuimg.py`)
- [x] **64-byte Linux boot header**: `magic = 0x644d5241` (`ARM\x64`) at offset `0x38`, header size `0x40` at offset `0x3c` (`xnuloader/entry.S`).
- [x] **EL2 $\rightarrow$ EL1 flattening**: Detect `CurrentEL`, configure `HCR_EL2`, enable timer access in `CNTHCTL_EL2`, and drop cleanly to EL1 with MMU off.
- [x] **EL1 BSS stack provisioning**: Provide 64 KiB in-image BSS stack (`xldr_entry_stack`) so bare-metal & QEMU entry without SP never faults.
- [x] **Android `boot.img` v2 packaging**: `mkxnuimg.py` packs `xnuloader.bin` + `XLDRPAY1` manifest + ADT + kernelcache + ramdisk with begonia offsets (`base=0x40078000`, `kernel_offset=0x00008000`, `page_size=2048`).
- [x] **Boot header page alignment**: Ensure boot header is padded to 2048 bytes so kernel payload sits at page 1.
- [x] **Early UART console**: MTK 16550 UART driver (`0x11002000`) and QEMU PL011 fallback (`0x09000000`).
- [x] **Early Framebuffer test pattern**: Draw RGB test bars and `HACKIOS XLDR` text directly into the $1080 \times 2340$ linear DRAM buffer (`0x7f000000`).
- [x] **Ramoops warm-reset persistence**: Write `XLDRPANIC` structure to physical DRAM `0x47df0000` (64 KiB) before parking with `wfe`.
- [x] **Host test harness**: Host-compiled test (`make tests`) validating synthetic FDT parsing and SecureDT ADT binary formatting.
- [x] **QEMU bring-up rehearsal**: `bash qemu/run_qemu.sh` running `xnuloader_qemu.elf` under QEMU `-M virt` (verifying entry $\rightarrow$ UART $\rightarrow$ FDT $\rightarrow$ panic).
- [ ] **On-device fastboot boot validation**: Run `./xl boot out/boot.img.m1` on live Redmi Note 8 Pro; verify green text banner on LCD and UART output.

### Milestone M2: Early XNU Kernel Entry & Memory Initialization
- [x] **MT6785 Board Config**: `ARM64_BOARD_CONFIG_MT6785` in `xnu/pexpert/pexpert/arm64/board_config.h` and `xnu/config/MT6785.mk`.
- [x] **Apple System Register Neutralization**: Stubbed or removed Apple-proprietary implementation-defined registers (`S3_x_c15_*`, `HID0..16`, `APRR`, `PPL`, `AMX`, `CPM`) that trap on Cortex-A76/A55 (`xnu/osfmk/arm64/proc_reg.h`).
- [x] **Mach-O Kernelcache Loader**: `xnuloader/macho.c` parsing `MH_EXECUTE` and `MH_FILESET` kernels, relocating segments to physical DRAM (`0x40200000`).
- [x] **XNU `boot_args` protocol**: Build revision 2 `boot_args` (`virtBase=0xffffffe040000000`, `physBase=0x40000000`, `memSize`, `Video`, `deviceTreeP`, `topOfKernelData`).
- [x] **Memory-map handover**: Pass `DeviceTree`, `RAMDisk`, `FrameBuffer`, and `PanicRings` in `/chosen/memory-map` so XNU claims them before memory initialization.
- [ ] **Exclude BL31/SCP/SSPM/TEE carveouts**: Verify that `0x47c00000..0x48000000` is strictly excluded from `pmap_MemRegions` and `vm_page_free_list` in `osfmk/arm64/pmap.c`.
- [ ] **Full XNU MT6785 build**: Compile `xnu-8796.121.2` with cross toolchain into `build/kernelcache.macho`.
- [ ] **On-device M2 test**: Verify XNU displays `-v` verbose banner (`Darwin Kernel Version ...`) on the begonia LCD.

### Milestone M3: Interrupts, Architectural Timer, and Heterogeneous SMP
- [x] **MT6785GICv3 Driver**: `kexts/MT6785GICv3` managing GICD (`0x0c000000`) and GICR (`0x0c040000`), enabling System Register Interface (`ICC_SRE_EL1`).
- [x] **PSCI Secondary Core Bring-up**: `xnuloader/psci.c` implementing `PSCI_CPU_ON_SMC64` (`0xC4000003`).
- [ ] **Dynamic Timebase Initialization**: Confirm `pe_arm_init_timebase()` reads `CNTFRQ_EL0` ($13\text{ MHz}$) dynamically instead of Apple's $24\text{ MHz}$.
- [ ] **Heterogeneous MPIDR Topology Map**: Update `ml_parse_cpu_topology()` in XNU:
  - Cluster 0: CPUs 0..5 (Cortex-A55, MPIDR `0x000`..`0x005`)
  - Cluster 1: CPUs 6..7 (Cortex-A76, MPIDR `0x100`..`0x101`)
- [ ] **GICv3 PPI Timer Hookup**: Route CNTP PPI (IRQ 27 or 30) into `rtclock_intr()`.
- [ ] **On-device M3 test**: Verify all 8 cores come online and scheduler timeslicing ticks without watchdog lockup.

### Milestone M4: Single-User Minimal Shell (RAMDisk `rd=md0`)
- [x] **RAMDisk Scripting**: `userland/ramdisk/mkramdisk.sh` generating an HFS+/APFS ramdisk image.
- [x] **Boot Arguments Configuration**: Default cmdline: `serial=3 -v debug=0x8e keepsyms=1 rd=md0 rootdev=md0 cs_enforcement_disable=1 amfi_get_out_of_my_way=0x1 wpdebug=1`.
- [ ] **Static ARM64 Diagnostic Shell**: Cross-compile static `/bin/sh` or `/bin/bash` without PAC instructions.
- [ ] **Root Device Matching**: Verify `IOService::waitForMatchingService` matches `AppleARMPE` / `IOMedia` RAMDisk nub.
- [ ] **On-device M4 test**: Verify Mach IPC, BSD syscalls, and an interactive shell console over UART0 (`0x11002000`).

### Milestone M5: Full iOS 16.5 Userland (`launchd` + `dyld_shared_cache_arm64`)
- [x] **IPSW Ingestion Tooling**: `userland/fetch_ipsw.sh` and `userland/unpack_ipsw.py` supporting `iPhone10,3` 16.5 (`20F66`).
- [x] **RootFS `fstab` rw Patch**: `userland/apply_rootfs_patches.sh` patching `/private/etc/fstab` to bypass sealed volume verification.
- [x] **MobileGestalt Cache Injector**: `userland/patch_mobilegestalt.py` injecting hardware model (`D22AP`), product type (`iPhone10,3`), chip ID (`0x8020`), and $1080 \times 2340$ resolution.
- [x] **AMFI Stub Driver**: `kexts/Stubs/AMFIStub.cpp` providing unconditional signature approval and `CS_PLATFORM_BINARY` flags.
- [x] **SEP & KeyStore Stubs**: `kexts/Stubs/SEPStub.cpp` returning success for keybag unlock (`MKBUnlockDevice`) with software AES.
- [x] **Watchdog Stub**: `kexts/Stubs/WatchdogStub.cpp` stubbing `IOWatchDogTimer` so `watchdogd` does not reboot the device.
- [ ] **Cryptex dyld Cache Merge**: Ensure `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64` is unified into rootfs.
- [ ] **On-device M5 test**: Verify `/sbin/launchd` boots as PID 1, spawns `logd`, `configd`, and `cfprefsd`.

### Milestone M6: Graphics, UI Compositing, and Touch Input
- [x] **Framebuffer Driver**: `kexts/MT6785Framebuffer/MT6785Framebuffer.cpp` publishing linear framebuffer for `IOSurface`.
- [x] **Touchscreen Driver**: `wifi/lx/drivers/mt6785/focaltech_fts.c` + `kexts/FocalTouchIOHID`.
- [x] **Storage Shim Drivers**: `wifi/lx/drivers/mt6785/mtk_msdc_msdc1.c` (MicroSD) and `mtk_ufshcd.c` (UFS 2.1).
- [ ] **Software Rasterization Bridge**: Software rendering backend for `backboardd` / `CAWindowServer` without Apple AGX GPU.
- [ ] **On-device M6 test**: Render SpringBoard Home Screen and receive multi-touch digitizer events.

---

## 3. Hardware Register & Memory Map Audit

| Peripheral | Physical Base | Size | Driver / Component | Verification Status |
| :--- | :--- | :--- | :--- | :--- |
| **DRAM Base** | `0x40000000` | 4–6 GiB | `xnuloader`, `boot_args.physBase` | [x] Verified against `begonia` FDT |
| **Loader Entry** | `0x40080000` | 1 MiB | `xnuloader/entry.S`, `mkxnuimg.py` | [x] Matches `lk` boot.img offset |
| **Kernel Base** | `0x40200000` | ~30 MiB | Mach-O segment target | [x] Configured in `main.c` |
| **Secure Carveout** | `0x47c00000` | 4 MiB | ATF BL31 / SCP / TEE | [!] Must be excluded from XNU pmap |
| **Ramoops Ring** | `0x47df0000` | 64 KiB | `xldr_write_panic_record` | [x] Verified warm-reset persistent |
| **Linear FB** | `0x7f000000` | ~10 MiB | `MT6785Framebuffer`, `boot_args.Video` | [~] Subject to unit DRAM size |
| **GICv3 Dist** | `0x0c000000` | 256 KiB | `kexts/MT6785GICv3` | [x] Verified against MT6785 spec |
| **GICv3 Redits** | `0x0c040000` | 768 KiB | `kexts/MT6785GICv3` ($8\times 0\text{x}20000$) | [x] Verified against MT6785 spec |
| **UART0** | `0x11002000` | 4 KiB | `xnuloader/console.c` (16550 4B-stride, SPI 115 / `0x73`) | [x] Verified in begonia DTS (baud 921600 or 115200) |
| **MicroSD (MSDC1)**| `0x11240000` | 64 KiB | `wifi/lx/drivers/mt6785/mtk_msdc_msdc1.c` (SPI 102 / `0x66`)| [x] Verified in begonia DTS (200 MHz max) |
| **UFS 2.1 (UFS0)** | `0x11270000` | 1 MiB | `wifi/lx/drivers/mt6785/mtk_ufshcd.c` (SPI 104 / `0x68`) | [x] Verified in begonia DTS |
| **Touch I2C0** | `0x11007000` | 4 KiB | `wifi/lx/drivers/mt6785/focaltech_fts.c` | [x] I2C slave address `0x38` |

---

## 4. Automated CI/CD & Testing Matrix

| Test Suite | Purpose | Execution | Status |
| :--- | :--- | :--- | :--- |
| **FDT/ADT Host Tests** | Validates Flattened Device Tree parsing and SecureDT serialization. | `make tests` (`xnuloader/test_host.c`) | [x] Passing |
| **Begonia Contract Test**| Evaluates boot header, payload wire format, MMIO registers, and config against begonia. | `python3 -m unittest -v tests/test_begonia_contract.py` | [x] Passing |
| **Code Syntax Linters** | Python (`py_compile`) and Shell (`bash -n`) syntax checks for all tools. | `ci.yml` `host-tests` job | [x] Passing |
| **aarch64 Loader Build**| Cross-compiles `xnuloader.bin` and packages `out/boot.m1.img`. | `make xnuloader` / `loader-aarch64` | [x] Passing |
| **QEMU Boot Rehearsal** | Boots loader under QEMU `-M virt` and validates UART banner, FDT parse, and clean panic. | `make qemu` (`qemu/run_qemu.sh`) | [x] Passing |
| **KEXT Static Check** | Validates IOKit C++ driver syntax and register references against XNU headers. | `make kexts` (`kexts/build.mk`) | [~] Inlined build |
| **Live Device Harness** | Automates `fastboot boot`, logs streaming, and ramoops extraction. | `./xl boot out/boot.img` / `./xl pstore` | [~] Ready for hardware |

---

## 5. Critical Risks & Mitigations

1. **PAC Undefined Instruction Traps**:
   - *Risk*: Running `arm64e` binaries on Cortex-A76/A55 results in `SIGILL` / undefined instruction panics (`EC=0x00`).
   - *Mitigation*: Strictly enforce `iPhone10,3` (iPhone X, A11) iOS 16.5 IPSW. The dyld shared cache and system daemons are compiled for plain `arm64` without PAC instructions.
2. **Apple Proprietary System Registers**:
   - *Risk*: XNU boot code touching `S3_0_c15_*` (HID0–HID16, APRR, PPL) causes early EL1 trap before console is initialized.
   - *Mitigation*: Gated out in `ARM64_BOARD_CONFIG_MT6785` and neutralized in `xnu/osfmk/arm64/proc_reg.h`.
3. **EL3 SError from Secure Carveouts**:
   - *Risk*: Mapping physical DRAM regions reserved by BL31, SCP, or TEE (`0x47c00000..0x48000000`) in XNU page tables triggers fatal EL3 SError panic.
   - *Mitigation*: Exclude carveout range in `pmap_bootstrap()` and verify against device `/proc/iomem`.
4. **Watchdog Panic Loop**:
   - *Risk*: Apple's `watchdogd` reboots the device after 60 seconds if system daemons fail to check in.
   - *Mitigation*: `kexts/Stubs/WatchdogStub.cpp` stubs `IOWatchDogTimer` to prevent hardware reset.
5. **No GPU Acceleration**:
   - *Risk*: `backboardd` and SpringBoard depend on Apple AGX and Metal.
   - *Mitigation*: CPU-composited `IOSurface` software scanout path via `MT6785Framebuffer`.
