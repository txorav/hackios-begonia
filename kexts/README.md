# kexts/ — HackiOS begonia platform kexts (Phases 3 & 4 of the plan)

The IOKit personality layer for the MT6785 (`begonia`) port.  Every kext
here builds against the vendored `xnu/` headers through `xnu-deps/` (see
`xnu-deps/README.md`) and is driven by the Apple DeviceTree that
`xnuloader` produces (see `docs/hardware_map_mt6785.md` §3).

## Per-kext summary (all phases referenced from the porting plan)

| Kext                    | Phase | Role                                                            |
| ----------------------- | ----- | --------------------------------------------------------------- |
| `MT6785GICv3`           | 3.3   | `IOInterruptController` for the SoC GICv3 (GICD @0x0c000000, GICR @0x0c040000).  GNUC `ICC_SRE_EL1` / `ICC_IAR1_EL1` / `ICC_EOIR1_EL1` interrupt dispatch (kext-external path via `IOService::resolveInterrupt`). |
| `MT6785PlatformExpert`  | 4.1   | Root `IOPlatformExpert` for the ADT root node. `pe_halt_restart()` via PSCI (`SYSTEM_RESET`/`SYSTEM_OFF`), populate speakers/timebase via CNTFRQ (never 24 MHz). |
| `MT6785Framebuffer`     | 4.2   | Publishes the lk-preinitialized framebuffer (0x7f000000, 1080×2340, 32bpp) as the PE video surface — XNU's `vc_progress_initialize` draws the `-v` console through it. |
| `LxStorage`             | 4.3   | Host kext that contains the whole lxkit runtime (`wifi/lx/lxkit`) + the storage drivers (`wifi/lx/drivers/mt6785/mtk_msdc_msdc1.c`, `mtk_ufshcd.c`); wraps the result into real `IOMedia` nubs GPT-style for `AppleAPFSContainer`. |
| `FocalTouchIOHID`       | 5.4   | Runs `focaltech_fts.c` against the LxKitTouchNub, translating `lxkit_touch_event` frames into `IOHIDEventService` digitizer events (`backboardd` matching). |
| `AMFIStub`              | 5.2   | Minimal stand-in: `cs_enforcement_disable`, unconditional `CS_PLATFORM_BINARY` approval, entitlements-software path (see userland/README §AMFI). |
| `SEPStub`               | 5.3   | Fake `AppleKeyStore` / `AppleSEPManager` UserClients so `securityd`/`UserEventAgent` never wait on a physical SEP.  `MKBUnlockDevice` succeeds; AES is software-backed. |
| `WatchdogStub`          | 5.3   | Fake `AppleARMWatchDogTimer` so `watchdogd` never reboots the box while bring-up still has dead daemons. |

## Build model

There is no Apple KEXT SDK to link against from Linux, so the SUPPORTED
build mode is **kernel-source-inlined**: each kext's `.mk` supplies its
`Info.plist`-equivalent (MODULE lists) and the sources; product builds
happen via the host harness (`kexts/build.mk`) — see the caveat in
`build.mk`.  The kexts' C++ sources are also cross-checked by the QEMU
early-boot host harness (see `../qemu/run_qemu.sh`).

The IOKit headers come straight from the vendored tree:

```
xnu/iokit/IOKit/*.h, xnu/libkern/c++/*.h, and the ADT codes in
xnuloader/adt.h (DT_ node names).
```

## Which node in the ADT each kext matches

| ADT node (xnuloader/adt.c produces)              | Matches / consumes                   |
| ------------------------------------------------ | ------------------------------------ |
| root `/compatible = "xiaomi,begonia","mtk,mt6785"` | MT6785PlatformExpert                |
| `/chosen` (console properties, memory-map)        | MT6785Framebuffer                   |
| `/arm-io/gic` (MMIO 0x0c000000/0x0c040000)        | MT6785GICv3                          |
| `/arm-io/msdc1`, `/arm-io/ufs0`                   | LxStorage (via LxKitService)         |
| `/arm-io/i2c0` + `/arm-io/touchscreen` (SPI 0x78) | FocalTouchIOHID (via LxKitTouchNub)  |
