# wifi/lx — the Linux-to-IOKit shim layer (MediaTek MT6785 peripherals)

This directory is the lx port layer: it hosts Linux-derived drivers
(`drivers/mt6785/`) and gives them a **targeted Linux look-and-feel** so
porting effort focuses on the hardware, not on the operating system
boundary. Its runtime executive — **lxkit** (`lxkit/`) — runs inside
IOKit kexts and manufacturing the correct runtime context around them.

## Layout

| Path                    | Purpose                                             |
| ----------------------- | --------------------------------------------------- |
| `include/lx/*.h`        | Public lx contract headers (each mirrors a Linux API family, stripped to what the ported drivers use — see the per-fileiox header comments for the mapping table). |
| `lxkit/`                | lxkit executive compiled into the host kext images: context/service lifecycle (`lxkit_core.cpp`, `lxkit_service.cpp`), workloops/workqueues (`lxkit_work.cpp`), locking (`lxkit_locks.cpp`), MMIO (`lxkit_iospace.cpp`), DMA (`lxkit_mem.cpp`), IRQ glue (`lxkit_irq.cpp`), console sink (`lxkit_console.cpp`), consumer-API (`lxkit_api.cpp`) and the storage driver registration bridge (`lxkit_drivers_glue.c`). |
| `drivers/mt6785/`       | Ported Linux drivers claiming the ADT `/arm-io` children (below). |
| `tests/`                | Host-runnable parser/unit tests (no device I/O).     |
| `lxkit_service.h`       | LxKitService + the nubs it publishes (IOKit bridge).  |

## Drivers

| Driver                    | Claims ADT device      | Physical        | Linux original                  |
| ------------------------- | ---------------------- | --------------- | ------------------------------- |
| `mtk_msdc_msdc1.c`        | `msdc1` (µSD)          | 0x11240000      | `drivers/mmc/host/mtk-sd.c`     |
| `mtk_ufshcd.c`            | `ufs0` (UFSHCI 2.1)    | 0x11270000      | `drivers/ufs/host/ufshcd.c` + `ufs-mediatek.c` |
| `focaltech_fts.c`         | `touchscreen` (i2c0)   | 0x11007000 @0x38| `focaltech_touch/fts5x06.c`     |

The consumers live in kexts: `kexts/LxStorage` (storage, publishes
IOMedia), `kexts/FocalTouchIOHID` (touch → backboardd), and
`kexts/MT6785Framebuffer` (display, stands alone).

## Bring-up priority on begonia (porting plan §4)

1. **Storage**: LWK bring-up uses msdc1 (µSD with no SEC fuse) before ufs0.
   When UFSHCI's `HCI_CTRL_UE` bit fails the 30-tick bring-up gate, ufs0
   defers and µSD stays boot-medium — see `lxkit_get_boot_medium_info()`.
2. **Touch**: the LxKitTouchNub provides *both* i2c0 device memory and the
   touchscreen SPI; focaltech_fts runs on it directly.
3. **Firmware quirks** (UFS VS-`VS_` device list, panel clock trees) stay
   exactly as in `docs/hardware_map_mt6785.md` §3 — check the ADT dump
   before touching the register tables.

## cross-image/ttree note

`lxkit_api.h` is THE shared consumer ABI (`kexts/*` include it only);
drivers never include IOKit headers — everything flows through
`lx_dev.h` / `lx_io.h` / `lx_irq.h`.  This keeps the Linux porting
experience: compile the driver once, on host tests or kernel images.
