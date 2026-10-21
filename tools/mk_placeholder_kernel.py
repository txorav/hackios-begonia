#!/usr/bin/env python3
"""mk_placeholder_kernel.py — emit a minimal, valid, arm64 MH_EXECUTE
Mach-O so the M1-only loader test can be packaged without a real
kernelcache.  The stub has:
  - one empty __TEXT segment (vmsize 0)
  - one LC_UNIXTHREAD with a valid entry point offset

xnuloader's macho.c accepts it (magic/cputype/filetype pass), while
`macho_load` records a 0-width segment-pool and returns the entry point
— YMMV when it actually _runs_: this artifact exists purely so the
M1 (loader banner + FDT→ADT translation) smoke test path can run end
to end through mkxnuimg.py without kernelcache artifacts present.
"""
import struct
import sys

MH_MAGIC_64      = 0xFEEDFACF
MH_CPUTYPE_ARM64 = 0x0100000C
MH_FILETYPE_EXEC = 2

LC_SEGMENT_64    = 0x19
LC_UNIXTHREAD    = 0x05


def main(out_path):
    cmds = bytearray()

    # LC_SEGMENT_64 __TEXT: empty (vmsize == 0, no sections).
    # magic(4) + cmdsize(4) + segname(16) + 4xQuad(32) + 4xWord(16) = 72
    segname = b"__TEXT".ljust(16, b"\x00")
    cmds += struct.pack("<II16sQQQQiiii",
                        LC_SEGMENT_64, 72, segname,
                        0x0000000040000000,  # vmaddr
                        0x0000000000000000,  # vmsize
                        0x0000000000000000,  # fileoff
                        0x0000000000000000,  # filesize
                        5, 5, 0, 0)          # maxprot(RX), initprot(RX), nsects, flags

    # LC_UNIXTHREAD (arm64 flavor = 6): 16 GP registers + CPSR + PC.
    # cmdsize = 8 (cmd) + 4 (flavor) + 4 (count) + count * 8
    flavor, count = 6, 18
    thread_hdr = struct.pack("<IIII", LC_UNIXTHREAD, 8 + 4 + 4 + count * 8,
                             flavor, count)
    thread_hdr += struct.pack(f"<{count}Q", *([0] * count))

    cmds += thread_hdr

    header = struct.pack("<IiiIII",
                         MH_MAGIC_64,
                         MH_CPUTYPE_ARM64,
                         0x00000000,     # same-subtype (lib64)
                         MH_FILETYPE_EXEC,
                         2,              # ncmds
                         len(cmds))      # sizeofcmds
    header += struct.pack("<i", 0)       # flags: none

    with open(out_path, "wb") as f:
        f.write(header + bytes(cmds))

    print(f"wrote {out_path} ({len(header) + len(cmds)} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mk_placeholder_kernel.py <output.macho>")
    main(sys.argv[1])
