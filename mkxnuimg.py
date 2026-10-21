#!/usr/bin/env python3
"""
mkxnuimg.py — pack xnuloader + Apple DeviceTree + kernelcache (+RAMDisk) into
the boot image lk (MediaTek Little Kernel) loads on begonia.

Output boot.img:
    header  : Android boot_img_hdr_v2 (712 bytes), pagesize 2048
    kernel  = gzip( loader.bin | XLDRPAY1 manifest [+cmdline] | adt.bin |
                    kernelcache.macho | ramdisk.img )
    ramdisk = <empty> (our loader carries everything inside the kernel blob)
    dtb     = <mt6785-xiaomi-begonia.dtb> (v2 dtb slot)

Offsets follow the porting plan exactly (docs/hardware_map_mt6785.md §5):
    base=0x40078000 kernel_offset=0x00008000 ramdisk_offset=0x07c08000
    tags_offset=0x0bc08000 pagesize=2048

Manifest wire format is defined by xnuloader/payload.h — do not diverge.
"""
import argparse
import gzip
import os
import struct
import sys

MAGIC            = b"XLDRPAY1"
PAYLOAD_VERSION  = 1
SECT_MAX         = 4          # [adt][kernel][ramdisk][0]
MANIFEST_SIZE    = 208        # must equal sizeof(struct payload_manifest)
BOOT_HDR_V2_SIZE = 712


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def parse_fb_size_list(s):
    if not s:
        return (0, 0, 0, 0)
    w, h, stride, depth = (int(x, 0) for x in s.split("x"))
    return (w, h, stride, depth)


# ---------------------------------------------------------------- payload --

def read_cmdline_file(path):
    if path == "-":
        return sys.stdin.buffer.read()
    with open(path, "rb") as f:
        return f.read()


def build_payload(args, out_dir):
    """Return uncompressed payload bytes (gzip'ed by the caller)."""
    loader = open(args.loader, "rb").read() if args.loader else b""
    kernel = open(args.kernel, "rb").read()
    if not loader:
        sys.exit("error: --loader is required")

    if len(loader) > 0x40000:
        sys.exit(f"error: loader too big ({len(loader):#x} > 0x40000)")

    m_off = align(len(loader), 0x10000)          # manifest page-aligned gap

    cmdline = bytearray()
    if args.cmdline:
        if args.cmdline == "-":
            raw = read_cmdline_file("-")          # stdin
        elif args.cmdline.startswith("@"):
            raw = read_cmdline_file(args.cmdline[1:])   # @file
        else:
            raw = args.cmdline.encode("utf-8")    # literal bootargs text
        cmdline = bytearray(raw[: 608])
        if not cmdline.endswith(b"\x00"):
            cmdline += b"\x00"

    adt_bin = b""
    if os.path.exists(args.adt):
        adt_bin = open(args.adt, "rb").read()
    rd_bin = b""
    if os.path.exists(args.ramdisk):
        rd_bin = open(args.ramdisk, "rb").read()

    # -- layout (offsets relative to manifest start, i.e. to payload base) --
    cmd_off = MANIFEST_SIZE
    adt_off = align(m_off + MANIFEST_SIZE + len(cmdline), 0x8000) - m_off
    ker_off = align(adt_off + len(adt_bin), 8)
    rd_off  = align(ker_off + len(kernel), 8)

    fb_w, fb_h, fb_stride, fb_depth = parse_fb_size_list(args.fb)
    fb_base = int(args.fb_base, 0) if args.fb_base else 0

    sections = [
        (b"adt",     2,  adt_off, len(adt_bin)),
        (b"kernel",  4,  ker_off, len(kernel)),
        (b"ramdisk", 0,  rd_off,  len(rd_bin)),
        (b"",        0,  0,     0),
    ]
    total = rd_off + len(rd_bin)

    man = bytearray()
    man += MAGIC
    man += struct.pack("<II", PAYLOAD_VERSION, 3)
    man += struct.pack("<Q", total)
    man += struct.pack("<Q", 0x40000000)              # phys_base
    man += struct.pack("<Q", 0xffffffe040000000)      # virt_base (loader recomputes)
    man += struct.pack("<Q", fb_base)
    man += struct.pack("<Q", fb_h * fb_stride if (fb_h and fb_stride) else 0)
    man += struct.pack("<IIII", fb_w, fb_h, fb_stride, fb_depth)
    for name, flags, off, size in sections:
        man += name.ljust(8, b"\x00")
        man += struct.pack("<IIQQ", flags, 0, off, size)
    man += struct.pack("<II", len(cmdline), len(cmdline))
    assert len(man) == MANIFEST_SIZE, f"manifest size {len(man)} != {MANIFEST_SIZE}"

    payload = bytearray()
    payload += loader
    payload += b"\x00" * (m_off - len(loader))
    payload += man
    payload += cmdline
    payload += b"\x00" * (m_off + adt_off - len(payload))
    payload += adt_bin
    payload += b"\x00" * (m_off + ker_off - len(payload))
    payload += kernel
    payload += b"\x00" * (m_off + rd_off - len(payload))
    payload += rd_bin

    with open(os.path.join(out_dir, "boot-payload.img"), "wb") as f:
        f.write(bytes(payload))
    return bytes(payload)


# -------------------------------------------------------------- bootimage --

def parse_args(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", required=True, help="output boot.img path")
    p.add_argument("--loader", required=True, help="xnuloader.bin")
    p.add_argument("--kernel", required=True, help="kernelcache Mach-O")
    p.add_argument("--adt", default="", help="prebuilt ADT blob (optional)")
    p.add_argument("--ramdisk", default="", help="ramdisk image (optional)")
    p.add_argument("--cmdline", default="", help="bootargs text or @file")
    p.add_argument("--dtb", default="", help="MTK device tree blob to append")
    p.add_argument("--dtb-addr", default="0x40100000")
    p.add_argument("--fb", default="", help="fb WxHxSTRIDExDEPTH e.g. 1080x2340x4320x0x32")
    p.add_argument("--fb-base", default="")
    p.add_argument("--os-name", default="HackiOS-begonia")
    p.add_argument("--page-size", type=int, default=2048)
    p.add_argument("--base", default="0x40078000")
    p.add_argument("--kernel-offset", default="0x00008000")
    p.add_argument("--ramdisk-offset", default="0x07c08000")
    p.add_argument("--tags-offset", default="0x0bc08000")
    p.add_argument("--second-offset", default="0x00000000")
    p.add_argument("--header-version", type=int, default=2)
    return p.parse_args(argv[1:])


def make_boot_img(args):
    out_dir = os.path.dirname(os.path.abspath(args.out)) or "."
    os.makedirs(out_dir, exist_ok=True)

    payload = build_payload(args, out_dir)
    kernel_blob = gzip.compress(payload, 9, mtime=0)

    dtb_blob = b""
    if args.dtb and os.path.exists(args.dtb):
        with open(args.dtb, "rb") as f:
            dtb_blob = f.read()

    page = args.page_size
    kernel_addr    = int(args.base, 0) + int(args.kernel_offset, 0)
    ramdisk_addr   = int(args.base, 0) + int(args.ramdisk_offset, 0)
    tags_addr      = int(args.base, 0) + int(args.tags_offset, 0)
    second_addr    = int(args.second_offset, 0)
    dtb_addr       = int(args.dtb_addr, 0)

    cmdline = args.cmdline if args.cmdline else "console=tty0"

    hdr = bytearray(BOOT_HDR_V2_SIZE)
    name_b = args.os_name.encode()[:16].ljust(16, b"\x00")
    cmd_b  = cmdline.encode()[:512].ljust(512, b"\x00")

    off = 0
    hdr[0:8]   = b"ANDROID!";                 off = 8
    hdr[off:off+4]  = struct.pack("<I", len(kernel_blob))
    hdr[off+4:off+8] = struct.pack("<I", kernel_addr);      off = 16
    hdr[off:off+4]  = struct.pack("<I", 0)                  # ramdisk size 0
    hdr[off+4:off+8] = struct.pack("<I", ramdisk_addr);     off = 24
    hdr[off:off+4]  = struct.pack("<I", 0)                  # second size
    hdr[off+4:off+8] = struct.pack("<I", second_addr);      off = 32
    hdr[off:off+4]  = struct.pack("<I", tags_addr)
    hdr[off+4:off+8] = struct.pack("<I", page);             off = 40
    hdr[off:off+4]  = struct.pack("<I", args.header_version)
    hdr[off+4:off+8] = struct.pack("<I", 0)                 # os_version
    off = 48
    hdr[off:off+16] = name_b;                               off = 64
    hdr[off:off+512] = cmd_b;                               off = 576
    hdr[off:off+32] = b"\x00" * 32                          # id[8]
    off = 608
    hdr[off:off+64] = b"\x00" * 64                          # extra_cmdline
    off = 672
    # v1: recovery_dtbo_size / recovery_dtbo_offset / header_size
    hdr[off:off+4]  = struct.pack("<I", 0)
    hdr[off+4:off+12] = struct.pack("<Q", 0)
    hdr[off+12:off+16] = struct.pack("<I", BOOT_HDR_V2_SIZE)
    off = 688
    # v2: dtb_size (+ reserved) + dtb_addr
    hdr[off:off+4]  = struct.pack("<I", len(dtb_blob))
    hdr[off+8:off+16] = struct.pack("<Q", dtb_addr)

    img = bytearray(hdr)
    img += kernel_blob
    img += b"\x00" * (align(len(kernel_blob), page) - len(kernel_blob))
    # ramdisk slot is empty (this loader carries its own)
    if dtb_blob:
        img += dtb_blob
        img += b"\x00" * (align(len(dtb_blob), page) - len(dtb_blob))

    with open(args.out, "wb") as f:
        f.write(bytes(img))

    summary = {
        "boot.img": args.out,
        "boot.img.size": len(img),
        "kernel.gz.size": len(kernel_blob),
        "payload.size": len(payload),
        "loader.size": os.path.getsize(args.loader) if args.loader else 0,
        "kernel.macho": args.kernel,
        "adt": args.adt,
        "ramdisk": args.ramdisk,
        "dtb": args.dtb,
        "kernel_addr": hex(kernel_addr),
        "ramdisk_addr": hex(ramdisk_addr),
        "tags_addr": hex(tags_addr),
        "dtb_addr": hex(dtb_addr),
    }
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    import json  # local import keeps tool shebang importable without deps
    make_boot_img(parse_args(sys.argv))
