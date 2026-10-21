/*
 * linker_qemu.ld — xtended link layout for the QEMU bring-up variant
 * (qemu/run_qemu.sh).  QEMU's -kernel ELF loader places PT_LOAD segments at
 * their virtual addresses, and -M virt RAM starts at 0x40000000 — so we link
 * the image at the canonical lk load base 0x40080000 instead of the VA-0
 * position-independent layout used for the device boot.img (linker.ld).
 *
 * This keeps every runtime constant in main.c (XLDR_LOAD_BASE, payload scan
 * range, kernel destination) valid under the emulator.
 */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(xldr_entry_real)

SECTIONS
{
	. = 0x40080000;

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
