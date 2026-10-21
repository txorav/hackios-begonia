/*
 * entry.S — 64-byte ARM64 Linux boot-image header + xnuloader entry.
 *
 * lk (MediaTek Little Kernel) loads a boot.img kernel at 0x40080000 and
 * branches to it with x0 = FDT PA.  The Linux-protocol header sits at file
 * offsets 0..0x40 (code0 branch at 0, magic "ARM\x64" at 0x38, header size
 * 0x40 at 0x3c); exec code starts at 0x40.
 *
 * Everything is PC-relative — the image is position independent.
 */
	IMAGE_SIZE = 0x200000
	.section .image_head, "ax"
	.globl _image_header_start
_image_header_start:
	b	xldr_entry_real         /* 0x00 code0 */
	.word	0                       /* 0x04 code1 */
	.quad	0                       /* 0x08 text_offset */
	.quad	IMAGE_SIZE              /* 0x10 image_size */
	.quad	0                       /* 0x18 flags */
	.space	0x18                    /* 0x20..0x37 reserved */
	.word	0x644d5241              /* 0x38 magic: "ARM\x64" */
	.word	0x40                    /* 0x3c header size */
	/* 0x40: exec code */
	b	xldr_entry_real
	.space	0x1c

	.text
	.align	2
	.globl	xldr_entry_real

xldr_entry_real:
	mov	x19, x0                     /* preserve lk's FDT pointer */

	/* mask async/irq/fiq for a clean handoff */
	msr	daifset, #0xF

	/* CurrentEL: bits[3:2] 0=EL0, 1=EL1, 2=EL2, 3=EL3 */
	mrs	x1, CurrentEL
	lsr	x1, x1, #2
	cmp	x1, #2
	b.eq	.L_hop_el2
	cmp	x1, #3
	b.eq	.L_at_el3
	b	.L_at_el1

.L_at_el3:
	/* MTK lk should never hand us at EL3 (BL31 sits there). Park. */
	mov	x0, #0xdead
	b	.L_dead

.L_hop_el2:
	/* --- flatten EL2 -> EL1 --- */
	mov	x2, #0
	msr	sctlr_el1, x2               /* SCTLR_EL1: MMU off            */
	dsb	sy
	isb

	mov	x4, #(1 << 31)              /* HCR_EL2: RW = 1 (AArch64)     */
	orr	x4, x4, #(1 << 34)          /* TGE = 1                       */
	orr	x4, x4, #(1 << 7)           /* AMO                            */
	orr	x4, x4, #(1 << 4)           /* IMO                            */
	orr	x4, x4, #(1 << 3)           /* FMO                            */
	msr	hcr_el2, x4

	/* allow EL1 access to the physical/virtual timers */
	mov	x3, #3                      /* CNTHCTL.EL1PCEN|EL1PCTEN      */
	msr	cnthctl_el2, x3
	mov	x3, #0
	msr	cntvoff_el2, x3

	mov	x5, #0x1C5                  /* SPSR_EL2: EL1h, DAIF masked   */
	msr	spsr_el2, x5
	adr	x6, .L_return_el1
	msr	elr_el2, x6
	dsb	sy
	isb
	eret

.L_return_el1:
	mov	x0, x19                     /* rebuild FDT PA               */
	b	.L_at_el1

.L_at_el1:
	mov	x2, #0
	msr	sctlr_el1, x2               /* ensure MMU is off            */
	dsb	sy
	isb

	mov	x0, x19
	bl	xldr_main                   /* C entry, x0 = FDT PA         */

.L_dead:
	wfe
	b	.L_dead
