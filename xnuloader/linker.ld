/*
 * linker.ld — xnuloader link layout.
 *
 * Physical address 0x40080000 (Linux boot protocol load address) is the
 * runtime ORIGIN; but the image itself is position-independent, so we link
 * purely by file order: .image_head at file/VA 0, code right after.
 * mkxnuimg.py then packs loader.bin + manifest + ADT + kernelcache.
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(xldr_entry_real)

SECTIONS
{
	. = 0x0;

	.image_head : {
		KEEP(*(.image_head))
	}

	. = ALIGN(0x40);
	.text : {
		*(.text .text.*)
	}

	. = ALIGN(64);
	.rodata : {
		*(.rodata .rodata.*)
	}

	. = ALIGN(64);
	.data : {
		*(.data .data.*)
	}

	. = ALIGN(64);
	.got : {
		*(.got .got.*)
	}

	. = ALIGN(64);
	_bss_start = .;
	.bss (NOLOAD) : {
		*(.bss .bss.* COMMON)
	}
	_bss_end = .;

	xldr_bss_size = _bss_end - _bss_start;

	/DISCARD/ : {
		*(.comment)
		*(.interp)
		*(.note.*)
		*(.eh_frame*)
	}
}
