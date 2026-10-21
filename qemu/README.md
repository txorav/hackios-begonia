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
2. `XLDR PANIC` — the loader walked its payload scan and parked cleanly
   (there is no `XLDRPAY1` manifest in the QEMU launcher yet).

Expected serial (first bring-up):

```

HACKIOS xnuloader: lk handoff, FDT @ 0x00000000044000000 ...
memSize   0x0000000080000000
kern ...
XLDR PANIC: payload manifest not found (mkxnuimg append failed?)
```

## What is validated / not validated

- Validated: EL1 flatten path in `entry.S`, UART console, FDT parser (`fdt.c`)
  against QEMU's real generated DTB, framebuffer write path (RAM), payload
  scan, panic-handoff (ramoops record at `0x47df0000` + WFE park).
- Not validated (needs the payload): `macho_load()`, ADT serialization into
  RAM, `boot_args` construction, XNU handoff — those run in the device
  `boot.img` test loop on first hardware contact (see `docs/BOOTING.md`).

## Next steps toward full XNU under QEMU (guide §5)

1. Build a `XLDRPAY1` payload with a stub kernel to exercise
   `macho_load()` + ADT placement in the emulator before the device.
2. For a full XNU bring-up, follow the community approach for emulating
   Apple SoCs (T8030-style QEMU forks): PL011 is swapped for an Apple
   UART model, `-M virt` GICv3 + generic timer map cleanly onto what the
   GICv3 kext targets, and the ADT builder here already emits the node set
   (`/chosen`, `/arm-io`, `/cpus`, `/memory-map`) XNU's `PE_init_platform`
   expects.
3. `labels` `boot-args` like `-v serial=3 debug=0x14e rd=md0` come from the
   payload cmdline (editable through `mkxnuimg.py --cmdline`).
