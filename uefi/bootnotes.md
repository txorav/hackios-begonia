# uefi/bootnotes.md — design rationale for the (optional) UEFI stage

## 1. Why a UEFI stage at all

The production boot chain for this port does **not** involve UEFI:

```
lk (MediaTek) → Android boot.img (mkxnuimg.py)
              → xnuloader @ 0x40080000  (from hardware_map §2/§5)
              → relocated kernelcache, ADT, RAMDisk, boot_args
              → XNU at "_start",  EL1
```

The UEFI stage exists for exactly three bring-up reasons:

1. **Standard debug asset class.** TianoCore on a new SoC gives DXE-core
   infrastructure (console, file access via SimpleFileSystem, memory-map
   dump, PCI-1x…-free ARM GCCs) — useful while `xnuloader` is being
   brought up in parallel.
2. **Fast iteration of payload discovery** — on a system with a working
   UFS/SD card driver CTor, payload files can be placed on disk by a
   normal MTK recovery flow and picked up without rebuilds.
3. **Independent Termination Test Point** — it can probe the same memory
   contract (`hardware_map §2`) the loader uses, and independently confirm
   or falsify it (fb base, carveout exclusion, ramoops position).

## 2. Entry contract (identical to xnuloader's)

`lk` treats whatever is in the boot.img *kernel slot* as an ARM64 Linux
image and jumps to it at `0x40080000` with **X0 = physical FDT address**
(`hardware_map §2` bullet list, "Loader/XNU shared boot contract").

Consequences for the UEFI path:

* A `head` stub equivalent to `xnuloader/image64.S` (ARM64 Linux image
  header magic `ARM\x64`, i.e. `0x644d5241`) is **required** to make the
  DXE FV launchable that way — that stub lives with the core (see
  ownership map §6: `xnuloader/`, `mkxnuimg.py`); **do not duplicate** the
  stub here. This package only assumes someone later "wires" the DXE FV
  into a boot.img payload (mkxnuimg.py consumes `payload{...}` items).
* MMU is off, EL ranges from lk, and `DEBUG` output is expected on UART0
  `0x11002000` (16550-style, **4-byte register stride**) — hence the
  bundled `Mt6785SerialPortLib`. The baud rate is *not* re-set here: `lk`
  leaves the line at 115200 8N1; reconfiguring would kill the console.

## 3. Memory contract used everywhere in this package

Straight out of `docs/hardware_map_mt6785.md` §2 (keep in sync):

| Region | Address | Use in this package |
| :--- | --- | :--- |
| `PHYS_BASE` | `0x40000000` | DRAM bottom; `boot_args.physBase` |
| xnuloader / our stage slot | `0x40080000` | boot.img kernel load (`base 0x40078000 + kernel_offset 0x00008000`) — *where the DXE stage itself is entered from* |
| kernelcache load | `0x40200000` | relocated XNU segments start here (`phys = vmaddr − virtBase + physBase`) |
| tags | `0x4bc08000` | Android boot protocol tags (`base + tags_offset`) — informational only |
| **Reserved carveout** | `0x47c00000–0x47FFFFFF` | ATF/SCP/SSPM/TEE — *never* map, *never* allocate from |
| ramoops ring | `0x47df0000` + 64 KiB | `XLDRPANIC` records — this stage does not touch it |
| `lk` framebuffer | `≈0x7f000000`, 1080×2340, rowBytes 4320, RGB8888 | `Boot_Video` passthrough candidate (§4) |
| DRAM top | `0xC0000000` (4 GiB units) | whitespace for allocation · `memSize` from FDT |

`boot_args` construction rules (same as the loader builds them):

* structure exactly as in `xnu/pexpert/pexpert/arm64/boot.h`
  (`Revision=2`/`Version=2`, `Boot_Video{v_baseAddr,v_display,v_rowBytes,
  v_width,v_height,v_depth}` — all `unsigned long` = 64-bit),
* `virtBase = 0xffffffe000000000 + 0x40000000` → `0xffffffe400000000`
  (`VM_MIN_KERNEL_WINDOW + physBase`, xnu-8796 non-KTRR),
* ADT base 32 KiB aligned; RAMDisk (optional, `rd=md0`) directly after
  ADT, 32 KiB aligned; **`boot_args` last, 64-byte aligned**;
  `topOfKernelData` = page-aligned end of the `boot_args` region,
* cmdline default copy = `hardware_map §4`:

```
serial=3 -v debug=0x8e keepsyms=1 panicdebug=1 rd=md0 rootdev=md0 \
cs_enforcement_disable=1 amfi_get_out_of_my_way=0x1 wpdebug=1 \
serial_baud=115200 panic_button=1
```

## 4. What the DXE driver does today (and why that is enough)

`Mt6785BootDxe.c`:

1. Hello on UART0 (raw `SerialPortWrite`; not via the DEBUG framework,
   so the banner is immune to PCD妙的 levels).
2. `GetMemoryMap()` and one line per descriptor (Type, phys range,
   attributes) — instant on-device audit of "did we inherit a sane map".
3. Enumerates `gEfiSimpleFileSystemProtocol` volumes and looks for the
   five payload candidates (`\hackios\xnuloader.bin`,
   `\hackios\kernelcache.macho`, `\hackios\adt.bin`,
   `\hackios\ramdisk.img`, `\hackios\cmdline.txt`) — printing size +
   target contract per item, **but does not relocate or boot**.

It then returns and lets the FDF perform whatever comes next — the point
in this state is to be a bring-up probe, not a bootstrap.

## 5. Where this deliberately stops

* `TimerDxe`/GICv3 architectural drivers — cross-team; must be paired
  with whichever GIC bring-up `kexts/MT6785GICv3` settles on, so they
  should only be added when its `GICR` stride per-CPU stance is final.
* ADT generation — that is `xnuloader/fdt2adt`'s single source of truth
  (template in `hardware_map §3`); this package only *reads*.
* `topOfKernelData`/`boot_args` construction — owned by the loader.
