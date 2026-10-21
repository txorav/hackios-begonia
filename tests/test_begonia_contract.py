#!/usr/bin/env python3
"""
tests/test_begonia_contract.py — Automated evaluation test suite for HackiOS on Redmi Note 8 Pro (begonia).

Evaluates and tests the codebase against the MediaTek MT6785 (Helio G90T) hardware specifications,
Android Little Kernel (lk) boot contract, Apple DeviceTree (ADT) SecureDT layout, and XNU 8796 port definitions.
"""

import gzip
import os
import struct
import subprocess
import sys
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.path.join(REPO_ROOT, "build")
OUT_DIR = os.path.join(REPO_ROOT, "out")

# Begonia Hardware Contract Constants (docs/hardware_map_mt6785.md)
BEGONIA_DRAM_BASE         = 0x40000000
BEGONIA_XLDR_LOAD_BASE    = 0x40080000
BEGONIA_KERNEL_LOAD_BASE  = 0x40200000
BEGONIA_CARVEOUT_START    = 0x47C00000
BEGONIA_CARVEOUT_END      = 0x48000000
BEGONIA_RAMOOPS_BASE      = 0x47DF0000
BEGONIA_RAMOOPS_SIZE      = 0x10000
BEGONIA_FB_BASE           = 0x7F000000
BEGONIA_FB_WIDTH          = 1080
BEGONIA_FB_HEIGHT         = 2340
BEGONIA_FB_STRIDE         = 4320
BEGONIA_FB_DEPTH          = 32

BEGONIA_GICD_BASE         = 0x0C000000
BEGONIA_GICR_BASE         = 0x0C040000
BEGONIA_UART0_BASE        = 0x11002000
BEGONIA_MSDC1_BASE        = 0x11240000
BEGONIA_UFS0_BASE         = 0x11270000
BEGONIA_I2C0_BASE         = 0x11007000

BEGONIA_TIMER_FREQ_HZ     = 13000000  # 13 MHz (MediaTek CNTFRQ)

# Android boot.img v2 offsets (docs/hardware_map_mt6785.md §5)
BOOT_BASE                 = 0x40078000
BOOT_KERNEL_OFFSET        = 0x00008000  # 0x40078000 + 0x00008000 = 0x40080000
BOOT_RAMDISK_OFFSET       = 0x07C08000  # 0x40078000 + 0x07C08000 = 0x47C80000
BOOT_TAGS_OFFSET          = 0x0BC08000  # 0x40078000 + 0x0BC08000 = 0x4BC80000
BOOT_PAGE_SIZE            = 2048

# Wire format constants (xnuloader/payload.h)
PAYLOAD_MAGIC             = b"XLDRPAY1"
PAYLOAD_MANIFEST_SIZE     = 208
PAYLOAD_ACTIVE_SECTIONS   = 3  # adt, kernel, ramdisk


class TestBegoniaHardwareMap(unittest.TestCase):
    """Verifies that all components in the repo agree with the canonical begonia hardware map."""

    def test_hardware_map_document_exists(self):
        doc_path = os.path.join(REPO_ROOT, "docs", "hardware_map_mt6785.md")
        self.assertTrue(os.path.isfile(doc_path), f"Hardware map doc missing: {doc_path}")
        with open(doc_path, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("0x40000000", content)
        self.assertIn("0x40080000", content)
        self.assertIn("0x0c000000", content)
        self.assertIn("0x11002000", content)
        self.assertIn("0x7f000000", content)
        self.assertIn("13 MHz", content)

    def test_xnuloader_c_headers_constants(self):
        main_c = os.path.join(REPO_ROOT, "xnuloader", "main.c")
        with open(main_c, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("0x40080000", content, "main.c must load at 0x40080000")
        self.assertIn("0x40200000", content, "main.c must place kernelcache at 0x40200000")
        self.assertIn("0x40000000", content, "main.c default DRAM base must be 0x40000000")

        console_h = os.path.join(REPO_ROOT, "xnuloader", "console.h")
        with open(console_h, "r", encoding="utf-8") as f:
            console_content = f.read()
        self.assertIn("0x47df0000", console_content, "console.h RAMOOPS_BASE must be 0x47df0000")

    def test_adt_device_tree_properties(self):
        adt_c = os.path.join(REPO_ROOT, "xnuloader", "adt.c")
        with open(adt_c, "r", encoding="utf-8") as f:
            content = f.read()
        # Verify root properties
        self.assertIn('"device-tree"', content)
        self.assertIn('"Xiaomi,begonia"', content)
        self.assertIn('"mtk,mt6785"', content)
        self.assertIn('"Redmi Note 8 Pro"', content)
        self.assertIn('"MT6785"', content)
        # Verify GIC, UART, MSDC, UFS, I2C MMIO bases
        self.assertIn("0x0c000000", content)
        self.assertIn("0x0c040000", content)
        self.assertIn("0x11002000", content)
        self.assertIn("0x11240000", content)
        self.assertIn("0x11270000", content)
        self.assertIn("0x11007000", content)
        # Verify memory-map reserved nodes
        self.assertIn('"DeviceTree"', content)
        self.assertIn('"RAMDisk"', content)
        self.assertIn('"FrameBuffer"', content)
        self.assertIn('"PanicRings"', content)
        # Verify verified hardware SPI interrupt numbers
        self.assertIn("0x73u", content, "uart0 interrupt must be SPI 115 (0x73)")
        self.assertIn("0x66u", content, "msdc1 interrupt must be SPI 102 (0x66)")
        self.assertIn("0x68u", content, "ufs0 interrupt must be SPI 104 (0x68)")


class TestAndroidBootImgPacking(unittest.TestCase):
    """Verifies that mkxnuimg.py builds a valid Android boot.img v2 conforming to begonia lk."""

    @classmethod
    def setUpClass(cls):
        os.makedirs(BUILD_DIR, exist_ok=True)
        os.makedirs(OUT_DIR, exist_ok=True)
        # Generate placeholder kernel
        cmd_kernel = [sys.executable, os.path.join(REPO_ROOT, "tools", "mk_placeholder_kernel.py"),
                      os.path.join(BUILD_DIR, "test_placeholder.macho")]
        subprocess.check_call(cmd_kernel, cwd=REPO_ROOT)

        # Build xnuloader binary if not present
        loader_bin = os.path.join(OUT_DIR, "xnuloader.bin")
        if not os.path.exists(loader_bin):
            subprocess.check_call(["make", "xnuloader"], cwd=REPO_ROOT)

        # Pack test M1 image
        cls.test_img = os.path.join(OUT_DIR, "test_boot.m1.img")
        cmd_pack = [
            sys.executable, os.path.join(REPO_ROOT, "mkxnuimg.py"),
            "--out", cls.test_img,
            "--loader", loader_bin,
            "--kernel", os.path.join(BUILD_DIR, "test_placeholder.macho"),
            "--cmdline", "serial=3 -v debug=0x8e keepsyms=1 rd=md0",
            "--fb", f"{BEGONIA_FB_WIDTH}x{BEGONIA_FB_HEIGHT}x{BEGONIA_FB_STRIDE}x{BEGONIA_FB_DEPTH}",
            "--fb-base", hex(BEGONIA_FB_BASE),
        ]
        subprocess.check_call(cmd_pack, cwd=REPO_ROOT)

    def test_boot_image_v2_header(self):
        self.assertTrue(os.path.isfile(self.test_img))
        with open(self.test_img, "rb") as f:
            header_bytes = f.read(712)

        magic = header_bytes[0:8]
        self.assertEqual(magic, b"ANDROID!", "Invalid Android boot.img header magic")

        # Unpack header fields according to boot_img_hdr_v2:
        # 8s (magic) + 4I (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr) +
        # 2I (second_size, second_addr) + 2I (tags_addr, page_size) + 2I (header_version, os_version)
        fields = struct.unpack_from("<8s8I2I", header_bytes, 0)
        kernel_size = fields[1]
        kernel_addr = fields[2]
        ramdisk_addr = fields[4]
        tags_addr = fields[7]
        page_size = fields[8]
        header_version = fields[9]

        self.assertGreater(kernel_size, 0, "Kernel size must be > 0")
        self.assertEqual(kernel_addr, BOOT_BASE + BOOT_KERNEL_OFFSET, "Kernel load addr must match 0x40080000")
        self.assertEqual(ramdisk_addr, BOOT_BASE + BOOT_RAMDISK_OFFSET, "Ramdisk addr must match 0x47c80000")
        self.assertEqual(tags_addr, BOOT_BASE + BOOT_TAGS_OFFSET, "Tags addr must match 0x4bc80000")
        self.assertEqual(page_size, BOOT_PAGE_SIZE, "Page size must be 2048")
        self.assertEqual(header_version, 2, "Android boot header version must be 2")

    def test_payload_manifest_structure(self):
        """Uncompresses the kernel payload and inspects the embedded XLDRPAY1 manifest."""
        with open(self.test_img, "rb") as f:
            f.seek(BOOT_PAGE_SIZE)  # Kernel payload begins at page 1 (offset 2048)
            gz_data = f.read()

        payload = gzip.decompress(gz_data)
        self.assertGreater(len(payload), 0x10000, "Payload must be at least 64 KiB")

        # Scan for XLDRPAY1 magic within payload
        manifest_offset = payload.find(PAYLOAD_MAGIC)
        self.assertNotEqual(manifest_offset, -1, "XLDRPAY1 manifest magic not found in decompressed payload")
        self.assertEqual(manifest_offset % 0x10000, 0, "Payload manifest must be 64 KiB page-aligned")

        # Read manifest: magic(8s) + version(I) + section_count(I) + reserved(2I) + fb fields
        hdr = payload[manifest_offset:manifest_offset + 32]
        magic, ver, sect_count = struct.unpack_from("<8s2I", hdr, 0)
        self.assertEqual(magic, PAYLOAD_MAGIC)
        self.assertEqual(ver, 1, "Payload manifest version must be 1")
        self.assertEqual(sect_count, PAYLOAD_ACTIVE_SECTIONS, "Payload active section count must be 3")

    def test_boot_image_with_ramdisk(self):
        ramdisk_path = os.path.join(OUT_DIR, "ramdisk.img")
        if not os.path.exists(ramdisk_path):
            subprocess.check_call(["make", "ramdisk"], cwd=REPO_ROOT)

        rd_boot_img = os.path.join(OUT_DIR, "test_boot.m4.img")
        loader_bin = os.path.join(OUT_DIR, "xnuloader.bin")
        cmd_pack = [
            sys.executable, os.path.join(REPO_ROOT, "mkxnuimg.py"),
            "--out", rd_boot_img,
            "--loader", loader_bin,
            "--kernel", os.path.join(BUILD_DIR, "test_placeholder.macho"),
            "--ramdisk", ramdisk_path,
            "--cmdline", "serial=3 -v rd=md0 rootdev=md0",
        ]
        subprocess.check_call(cmd_pack, cwd=REPO_ROOT)
        self.assertTrue(os.path.isfile(rd_boot_img))
        self.assertGreater(os.path.getsize(rd_boot_img), 50000)


class TestXnuLoaderImageHeader(unittest.TestCase):
    """Verifies that xnuloader binary has a compliant 64-byte ARM64 Linux kernel header."""

    def test_arm64_header_magic_and_size(self):
        loader_bin = os.path.join(OUT_DIR, "xnuloader.bin")
        if not os.path.exists(loader_bin):
            subprocess.check_call(["make", "xnuloader"], cwd=REPO_ROOT)

        with open(loader_bin, "rb") as f:
            hdr = f.read(64)

        # Linux ARM64 boot header specification:
        # Offset 0x38: Magic "ARM\x64" (0x644d5241)
        magic = struct.unpack_from("<I", hdr, 0x38)[0]
        self.assertEqual(magic, 0x644D5241, "ARM64 header magic (ARM\\x64) missing at offset 0x38")

        # Offset 0x3c: Header size = 0x40 (64 bytes)
        hdr_size = struct.unpack_from("<I", hdr, 0x3C)[0]
        self.assertEqual(hdr_size, 0x40, "ARM64 header size must be 0x40 at offset 0x3c")

        # Offset 0x00: Branch instruction (b xldr_entry_real)
        code0 = struct.unpack_from("<I", hdr, 0x00)[0]
        self.assertEqual((code0 >> 26) & 0x3F, 0x05, "Instruction at offset 0x0 must be ARM64 B (unconditional branch)")


class TestXnuMT6785ConfigContract(unittest.TestCase):
    """Verifies MT6785 configuration assertions in xnu/config/MT6785.mk."""

    def test_xnu_config_defines(self):
        mk_path = os.path.join(REPO_ROOT, "xnu", "config", "MT6785.mk")
        self.assertTrue(os.path.isfile(mk_path), f"MT6785.mk not found: {mk_path}")
        with open(mk_path, "r", encoding="utf-8") as f:
            mk = f.read()

        required = ["ARM64_BOARD_CONFIG_MT6785", "__ARM_16K_PG__", "ARM_ARCH_TIMER", "HAS_GIC_V3"]
        for req in required:
            self.assertIn(req, mk, f"Required MT6785 define missing: {req}")

        forbidden = [
            "ARM64_BOARD_CONFIG_T8101", "APPLE_ARM64_ARCH_FAMILY",
            "KERNEL_INTEGRITY_CTRR", "KERNEL_INTEGRITY_KTRR",
            "HAS_IPI", "HAS_APPLE_GENERIC_TIMER", "CPU_HAS_APPLE_PAC"
        ]
        for forb in forbidden:
            self.assertIn(forb, mk, f"Forbidden define check missing: {forb}")


class TestUserlandPatchContract(unittest.TestCase):
    """Verifies userland MobileGestalt and patcher configuration against begonia."""

    def test_mobilegestalt_screen_and_chip(self):
        mg_py = os.path.join(REPO_ROOT, "userland", "patch_mobilegestalt.py")
        with open(mg_py, "r", encoding="utf-8") as f:
            content = f.read()
        self.assertIn("1080", content, "Begonia screen width 1080 missing in patch_mobilegestalt.py")
        self.assertIn("2340", content, "Begonia screen height 2340 missing in patch_mobilegestalt.py")
        self.assertIn("0x8020", content, "A11 ChipID 0x8020 missing in patch_mobilegestalt.py")
        self.assertIn("iPhone10,3", content, "ProductType iPhone10,3 missing")
        self.assertIn("D22AP", content, "HardwareModel D22AP missing")


if __name__ == "__main__":
    unittest.main()
