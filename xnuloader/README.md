# xnuloader

Stage-2 bare-metal loader for the `begonia` HackiOS port (plan §3 Phase 2).

## What it does

`lk` (MediaTek) boots `boot.img` and hands control at physical `0x40080000`,
EL2 (usually), `x0` = FDT address.  `xnuloader`:

1. paints the `lk`-initialized framebuffer (**M1**) and opens UART0 logs,
2. walks the MediaTek FDT (`fdt.c`) for DRAM map / sanity,
3. locates the payload manifest appended by `mkxnuimg.py` (magic `XLDRPAY1`),
4. relocates the (flat `MH_EXECUTE` or prelinked `MH_FILESET`) Mach-O
   kernelcache to `0x40200000` (`macho.c`; BSS zero-filled; entry recovered
   from `LC_UNIXTHREAD` and converted linked-VA -> PA),
5. builds the **Apple DeviceTree** template (`adt.c`, `/chosen`,
   `/chosen/memory-map`, `/product`, `/cpus cpu0..7`, `/arm-io`)
   and copies it right after the relocated kernel,
6. places the RAMDisk (if any) and the `boot_args` struct,
7. flattens EL2 -> EL1, flushes caches, and jumps to XNU with `x0` =
   physical `boot_args`.

## Build

```sh
make          # needs an aarch64 cross gcc/clang on PATH
```

`CROSS_CC` / `CROSS_GNU` / `CLANG` are auto-detected by the Makefile. To get a
toolchain quickly on Linux: see `../tools/README.md` (install_llvm_toolchain.sh).

## Package into a boot image

```sh
make -C .. xnuloader                             # top-level helper
python3 ../mkxnuimg.py --out out/boot.img \
    --kernel out/kernelcache.macho --loader xnuloader.bin \
    --adt out/adt.bin --ramdisk out/ramdisk.img
fastboot boot out/boot.img
```

## Layout it produces (docs/hardware_map_mt6785.md §2)

| item | address |
| --- | --- |
| xnuloader code | `0x40080000` |
| kernelcache segments | `vmaddr - CLS_BASE + 0x40200000` |
| Apple DeviceTree | after kernel top, 32 KiB-aligned (64 KiB reserved) |
| RAMDisk | after ADT, 32 KiB-aligned |
| boot_args | after RAMDisk, 64-byte aligned |
| topOfKernelData | next 16 KiB boundary after boot_args |
| ramoops | `0x47df0000..0x47e00000` (panic record survives warm reset) |

## Files

| file | purpose |
| --- | --- |
| `entry.S` | 64-byte ARM64 Linux boot header + EL2→EL1 hop + cache ops |
| `main.c` | boot flow / payload discovery / boot_args / branch to XNU |
| `console.c(.h)` | UART0 + RGBX8888 framebuffer console + 3x5 font + XLDRPANIC pstore record |
| `fdt.c(.h)` | minimal FDT walker (DRAM size, reserves) |
| `adt.c(.h)` | Apple DeviceTree blob writer (SecureDT-compatible) + begonia template |
| `macho.c(.h)` | kernelcache Mach-O loader (MH_EXECUTE and MH_FILESET) |
| `psci.c(.h)` | PSCI SMC calls (cpu_on/reset/off) |
| `payload.h` | payload manifest shared with `../mkxnuimg.py` |
| `linker.ld` | position-independent image layout |
