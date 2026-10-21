# Boot flow — from `lk` to XNU (`begonia`)

```
+------------------+     boot.img v2 (mkxnuimg.py)
|   MediaTek lk    | ------------------------------------------------+
|  (Little Kernel) |  kernel  = gz( payload )  ->  load @ 0x40080000 |
+------------------+  ramdisk -> 0x47c80000 (we override: embed ours  |
        |              inside the kernel payload instead)             |
        |  x0 = FDT (mt6785-xiaomi-begonia.dtb)                       |
        v                                                             |
  +---------------+   64-byte ARM64 kernel header (ARM\x64)           |
  |  xnuloader    |   built by xnuloader/{image_header.S,...}         |
  |  stage-2      |   sw environment: lk already set UART + display   |
  +---------------+                                                   |
        |  1. parse FDT (fdt.c)  -> DRAM size, reserve map             |
        |  2. build Apple DeviceTree (adt.c) + /chosen/memory-map      |
        |  3. parse kernelcache Mach-O (macho.c), relocate segments    |
        |     PA = vmaddr - virtBase + physBase                        |
        |  4. setup boot_args (boot_args.c)                            |
        |  5. flatten EL2->EL1 (if needed), caches clean+inval         |
        |  6. branch to kernel entry (_start) with x0=boot_args PA     |
        v                                                             |
  +---------------------------------------------------+               |
  |  XNU (osfmk/arm64/start.s)  -v banner to framebuffer |             |
  |  pmap 16K, PE_init_iokit, IOCatalogue               |             |
  |  wait for root: IOService matching (RAMDisk->MSD -> |             |
  |  MicroSD -> UFS)                                    |             |
  +----------------------------------------------------+------------+
                                    |
                                    v
                   launchd (iOS 16.5 handoff), backboardd, SpringBoard
```

## Payload format inside the gz image (`xnuloader/payload.h`)

```
offset  size    field
0x00    8       magic  "XLDRPAY1" (0x3159415052444C58)
0x08    4       manifest_version (=1)
0x0C    4       section_count (=4)
per-section records (16 bytes each): name[8]; then at data_base:
0x30..0x3F   cmdline (NUL-terminated, < 608 bytes)
data sections: [ADT][KERNEL][RAMDISK]  (each 32 KiB aligned)
```

`mkxnuimg.py` produces this blob, gzips it, and wraps it in the Android boot
image (v2 header) with the offsets listed in `docs/hardware_map_mt6785.md` §5.

## Why the framebuffer is "already initialized"

MediaTek `lk` leaves the panel configured; the physical linear framebuffer
address/stride is recorded both in the FDT (`/display` or chosen framebuffer
node — varies by lk build) and, once discovered, frozen into
`docs/hardware_map_mt6785.md` and the ADT `/arm-io/disp-fb` node. XNU then uses
it via `boot_args.Video` and later services (`IOMobileFramebuffer`).

## EL2/EL1 policy

`lk` on MT6785 typically enters the kernel at EL2. `xnuloader` detects the
current EL and, if `EL == 2`, writes `SCTLR_EL2` off, sets `HCR_EL2` so the
generic timer is owned by EL1, then `ERET`s to EL1 (`SPSR_M=1`) before XNU
start. XNU's own `start.s` expects to run at EL1 with MMU off.
