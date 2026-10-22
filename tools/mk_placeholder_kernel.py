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

    # LC_SEGMENT_64 __TEXT: 16 KiB dummy segment.
    # magic(4) + cmdsize(4) + segname(16) + 4xQuad(32) + 4xWord(16) = 72
    segname = b"__TEXT".ljust(16, b"\x00")
    cmds += struct.pack("<II16sQQQQIIII",
                        LC_SEGMENT_64, 72, segname,
                        0x0000000040200000,  # vmaddr (KERNEL_LOAD_BASE)
                        0x0000000000004000,  # vmsize (16 KiB)
                        0x0000000000000000,  # fileoff
                        0x0000000000000000,  # filesize
                        5, 5, 0, 0)          # maxprot(RX), initprot(RX), nsects, flags

    # LC_UNIXTHREAD (arm64 flavor = 6): ARM64_THREAD_STATE64 has 34 quads (68 32-bit words).
    # cmdsize = 4 (cmd) + 4 (cmdsize) + 4 (flavor) + 4 (count) + 34 * 8 = 288 bytes
    flavor, count = 6, 68
    thread_hdr = struct.pack("<IIII", LC_UNIXTHREAD, 16 + (count // 2) * 8,
                             flavor, count)
    thread_hdr += struct.pack(f"<{count // 2}Q", *([0] * (count // 2)))

    cmds += thread_hdr

    # mach_header_64 is exactly 32 bytes (8 x 32-bit words)
    header = struct.pack("<IIIIIIII",
                         MH_MAGIC_64,
                         MH_CPUTYPE_ARM64,
                         0x00000000,     # cpusubtype
                         MH_FILETYPE_EXEC,
                         2,              # ncmds
                         len(cmds),      # sizeofcmds
                         0,              # flags
                         0)              # reserved

    with open(out_path, "wb") as f:
        f.write(header + bytes(cmds))

    print(f"wrote {out_path} ({len(header) + len(cmds)} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: mk_placeholder_kernel.py <output.macho>")
    main(sys.argv[1])
