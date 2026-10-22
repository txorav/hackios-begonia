# QEMU bring-up (guide Phase A)

The original guide is firm: *"Debugging on real hardware first is a trap.
Get the OS stack running in an emulator, then port."* This directory is the
repo's first QEMU harness and it runs **the actual device loader** — same
boot flow as on the phone, only the two board-specific details are swapped at
compile time:

| Detail | MTK begonia (device) | QEMU `-M virt` |
|---|---|---|
| UART | MTK 16550-esque, 32-bit regs, base `0x11002000`, LSR@0x14 THRE | PL011 at `0x09000000`, FR@0x18 TXFF |
| Image placement | lk copies the raw `boot.img` kernel to `0x40080000` | QEMU's ELF loader places PT_LOAD at its virtual address |
| Console | UART pads + FB panel | stdio serial |
| FB | panel buffer left on by lk | plain RAM writes (no scanout) |

`run_qemu.sh` therefore builds the *camera-variant* of `xnuloader.elf` with:

```
make -C xnuloader LINK_SCRIPT=linker_qemu.ld CFLAGS_EXTRA='-DXLDR_UART_PL011' all
```

…copies it to `build/xnuloader_qemu.elf`, boots it with:

```
qemu-system-aarch64 -M virt -cpu cortex-a57 -smp 1 -m 2048 \
    -kernel build/xnuloader_qemu.elf -display none -serial stdio
```

and asserts on the captured log:

1. `HACKIOS xnuloader`  — banner, i.e. entry.S reached `xldr_main()`,
2. `payload manifest found` — the loader walked its payload scan and located `XLDRPAY1`,
3. `MILESTONE M1 ACHIEVED ON BEGONIA` — complete loader execution, ADT construction, and kernel setup.

Expected serial (Milestone M1 achievement):

```
HACKIOS xnuloader: lk handoff, FDT @ 0x0000000048000000
memSize  0x0000000080000000
payload manifest found; sections: 0x0000000000000003
staging payload to 0x50000000 (0x0000000000008188 bytes)
FB base: 0x000000007e605000
kern 0x0000000040200000 entry 0x0000000000000000 ADT 0x0000000040208000 RD 0x0000000040210000 BA 0x0000000040210000 topo 0x0000000040214000

========================================
[XLDR] MILESTONE M1 ACHIEVED ON BEGONIA!
[XLDR] Stage-2 bare-metal loader executed successfully.
[XLDR] Framebuffer, FDT & ADT parsing validated.
========================================
```

## What is validated

- **EL1 Flatten & Exceptions**: EL2/EL3 drop to EL1 in `entry.S`.
- **UART Console**: PL011 for QEMU (`-DXLDR_UART_PL011`), MTK 16550 on device (`0x11002000`).
- **FDT Parser**: Live QEMU DTB parser extracts DRAM capacity and UART.
- **Payload Manifest**: `XLDRPAY1` header parser and 64-bit quadword payload staging to safe DRAM window (`0x50000000`).
- **Apple DeviceTree (ADT)**: Dynamic construction of `/chosen`, `/arm-io`, `/cpus`, and `/memory-map` nodes matching XNU requirements.
- **Mach-O Loader**: Parsing 64-bit multi-segment Mach-O binaries (`__TEXT`, `__DATA`, `__LINKEDIT`), computing memory bounds, and zero-filling BSS.
- **boot_args & RAMDisk handoff**: Struct `boot_args` built with memory topology, video console arguments, and cmdline.

## Running Full XNU under QEMU

To boot the real 15 MB XNU Mach-O kernel (`prebuilt/mach.development.bcm2837`) along with the APFS ramdisk under QEMU:

```bash
make qemu-ios
# or directly:
bash qemu/run_ios.sh [timeout_seconds]
```
