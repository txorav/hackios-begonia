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
	/* --- disable MTK hardware watchdog early at EL2 --- */
	ldr	x0, =0x10007000             /* MTK_WDT_BASE                 */
	mov	w1, #0x1971                 /* MTK_WDT_RESTART_KEY          */
	str	w1, [x0, #0x08]             /* MTK_WDT_RESTART (kick timer) */
	ldr	w1, [x0]                    /* MTK_WDT_MODE                 */
	mov	w2, #0x22000000             /* MTK_WDT_MODE_KEY             */
	orr	w1, w1, w2
	mov	w2, #0xFFB6                 /* clear bit 0 (EN), 3 (IRQ), 6 (DUAL) */
	and	w1, w1, w2
	str	w1, [x0]                    /* disable MTK hardware watchdog */

	/* --- flatten EL2 -> EL1 --- */
	mov	x2, #0x0800                 /* SCTLR_EL1 RES1 bits, MMU off */
	msr	sctlr_el1, x2
	dsb	sy
	isb

	/*
	 * HCR_EL2: RW = 1 (AArch64 execution at EL1).
	 * Must NOT set TGE, E2H, or AMO/IMO/FMO interrupt routing.
	 */
	mov	x4, #(1 << 31)              /* HCR_EL2: RW = 1 (AArch64)    */
	msr	hcr_el2, x4

	/* disable coprocessor and FP/SIMD traps to EL2 */
	mov	x3, #0x33ff                 /* CPTR_EL2 RES1 | no FP traps   */
	msr	cptr_el2, x3

	/* enable FP/SIMD instructions at EL1 */
	mov	x3, #(3 << 20)              /* CPACR_EL1.FPEN = 0b11         */
	msr	cpacr_el1, x3

	/* allow EL1 access to the physical/virtual timers */
	mov	x3, #3                      /* CNTHCTL.EL1PCEN|EL1PCTEN     */
	msr	cnthctl_el2, x3
	msr	cntvoff_el2, xzr

	mov	x5, #0x3C5                  /* SPSR_EL2: EL1h, all DAIF masked */
	msr	spsr_el2, x5
	adr	x6, .L_return_el1
	msr	elr_el2, x6
	dsb	sy
	isb
	eret

.L_return_el1:
	mov	x0, x19                     /* restore lk's FDT PA          */
	b	.L_at_el1

.L_at_el1:
	/* enable FP/SIMD instructions at EL1 */
	mov	x2, #(3 << 20)
	msr	cpacr_el1, x2

	mov	x2, #0x0800
	msr	sctlr_el1, x2               /* ensure MMU is off, RES1 set  */
	dsb	sy
	isb

	/*
	 * Establish the C stack.  lk hands off with a live SP on the
	 * device, but QEMU -kernel (and any bare-metal EL1 entry) starts
	 * with SP=0 — the first prologue store then faults at SP-0xd0.
	 * xldr_entry_stack is a 64 KiB in-image BSS array (main.c); use
	 * its TOP (adrp:, then add the array size).
	 */
	adrp	x7, xldr_entry_stack
	add	x7, x7, :lo12:xldr_entry_stack
	add	x7, x7, #0x10000
	mov	sp, x7

	mov	x0, x19
	bl	xldr_main                   /* C entry, x0 = FDT PA         */

.L_dead:
	wfe
	b	.L_dead
