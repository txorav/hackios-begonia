#!/usr/bin/env python3
import struct
import sys
import os

def unpack(boot_img_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    with open(boot_img_path, "rb") as f:
        header = f.read(2048)
        magic = header[0:8]
        if magic != b"ANDROID!":
            print(f"Error: Invalid Android boot image magic: {magic}")
            return False

        kernel_size, kernel_addr = struct.unpack_from("<II", header, 8)
        ramdisk_size, ramdisk_addr = struct.unpack_from("<II", header, 16)
        second_size, second_addr = struct.unpack_from("<II", header, 24)
        tags_addr, page_size, header_version, os_version = struct.unpack_from("<IIII", header, 32)
        cmdline = header[64:576].split(b"\x00")[0].decode("utf-8", errors="ignore")
        extra_cmdline = header[608:1632].split(b"\x00")[0].decode("utf-8", errors="ignore")
        
        print(f"[*] Boot Image Header Version: {header_version}")
        print(f"[*] Page Size: {page_size}")
        print(f"[*] Kernel Size: {kernel_size} (Addr: 0x{kernel_addr:08x})")
        print(f"[*] Ramdisk Size: {ramdisk_size} (Addr: 0x{ramdisk_addr:08x})")
        print(f"[*] Tags Addr: 0x{tags_addr:08x}")
        print(f"[*] Cmdline: {cmdline} {extra_cmdline}")

        dtb_size = 0
        dtb_addr = 0
        if header_version >= 2:
            recovery_dtbo_size, recovery_dtbo_offset, header_size, dtb_size, dtb_addr = struct.unpack_from("<IQIIQ", header, 1632)
            print(f"[*] Recovery DTBO Size: {recovery_dtbo_size} (Offset: 0x{recovery_dtbo_offset:08x})")
            print(f"[*] Header Size: {header_size}")
            print(f"[*] DTB Size: {dtb_size} (Addr: 0x{dtb_addr:08x})")

        def page_align(val):
            return ((val + page_size - 1) // page_size) * page_size

        offset = page_size
        f.seek(offset)
        if kernel_size > 0:
            kernel_data = f.read(kernel_size)
            with open(os.path.join(out_dir, "kernel"), "wb") as kf:
                kf.write(kernel_data)
            print(f"[+] Saved kernel ({kernel_size} bytes)")
            offset += page_align(kernel_size)

        f.seek(offset)
        if ramdisk_size > 0:
            ramdisk_data = f.read(ramdisk_size)
            with open(os.path.join(out_dir, "ramdisk.cpio.gz"), "wb") as rf:
                rf.write(ramdisk_data)
            print(f"[+] Saved ramdisk ({ramdisk_size} bytes)")
            offset += page_align(ramdisk_size)

        if second_size > 0:
            offset += page_align(second_size)

        if header_version >= 1 and recovery_dtbo_size > 0:
            offset += page_align(recovery_dtbo_size)

        if header_version >= 2 and dtb_size > 0:
            f.seek(offset)
            dtb_data = f.read(dtb_size)
            with open(os.path.join(out_dir, "dtb"), "wb") as df:
                df.write(dtb_data)
            print(f"[+] Saved DTB ({dtb_size} bytes)")

        with open(os.path.join(out_dir, "bootimg.cfg"), "w") as cf:
            cf.write(f"header_version={header_version}\n")
            cf.write(f"page_size={page_size}\n")
            cf.write(f"kernel_addr=0x{kernel_addr:08x}\n")
            cf.write(f"ramdisk_addr=0x{ramdisk_addr:08x}\n")
            cf.write(f"tags_addr=0x{tags_addr:08x}\n")
            cf.write(f"dtb_addr=0x{dtb_addr:08x}\n")
            cf.write(f"cmdline={cmdline} {extra_cmdline}\n")
        print(f"[+] Saved config to {out_dir}/bootimg.cfg")
        return True

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <boot.img> <out_dir>")
        sys.exit(1)
    unpack(sys.argv[1], sys.argv[2])
