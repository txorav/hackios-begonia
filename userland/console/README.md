# userland/console — host-side console reading (NOTES)

This directory is deliberately thin: the actual console transport utility is
**`xl` at the repo root** (per the repo §2 layout in the porting plan —
"Host utility to stream kernels/logs over USB fastboot/serial"), owned by the
core/loader team. Do not duplicate it here.

What userland owes to the console story and where it lives:

| Concern | Owner / file |
| :--- | :--- |
| Physical debug outputs on begonia: UART0 `0x11002000` + lk framebuffer `0x7f000000` (RGBX8888, 1080×2340, 4320 rowBytes) + warm-reset ramoops ring `0x47df0000` | `docs/hardware_map_mt6785.md` §1–§2 (canonical) |
| Streaming/reading those channels from the operator host | root-repo `xl` (fastboot/USB/serial); Phase-1 plan §2 |
| What writes to the console on M4 | `../ramdisk/static_shell/main.c` (`/dev/console` writes; falls back to manufactured fds) |
| What writes on M5+ | iOS launchd logd/os_log — routed via `xnu` console plumbing; capture through `xl` |
| Kernel-saved panic rings | loader-side (`xnuloader`, hardware-map §2 `PanicRings`), read back after a warm reset from TWRP: `dd` then decode `XLDRPANIC` records |

## Operator quick reference (assuming TWRP/OrangeFox is installed per Phase 1)

```sh
# early console sanity from the host (no xl yet? this is the placeholder route):
#   1) ensure USB serial gadget sees the device
ls -l /dev/ttyUSB* /dev/ttyACM*
#   2) raw read (115200 8N1):
stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb
cat /dev/ttyUSB0
```

If UART0 is not wired (test pads), capture the dual-channel fallbacks from
the plan instead: framebuffer console (Phase-1 §2) or pstore/ramoops dump via
`dd` from the recovery shell (address `0x47df0000`, per hardware-map §2).

## Open questions (tracked)

* Does `xl` end up supporting a `-c /dev/console`-style passthrough so M4
  shell sessions interleave with kernel logs? (ask core team when `xl`
  lands; placeholder note exists here so userland-side sessions get merged
  into one capture instead of two windows).
