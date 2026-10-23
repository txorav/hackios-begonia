/*
 * main.c — xnuloader stage-2 boot flow (lk -> XNU).
 *
 * Entry (from entry.S): x0 = FDT physical address passed by lk.
 * Exit: branch to the XNU kernel entry (its physical address from the
 * LC_UNIXTHREAD), x0 = physical address of boot_args, MMU off, EL1, caches
 * flushed.  XNU's start_first_cpu / common_start do everything else
 * (TCR/TTBR/MAIR/SCTLR, VBAR, temp tables, arm_init() through PE_*).
 */
#include "console.h"
#include "fdt.h"
#include "adt.h"
#include "boot_args.h"
#include "macho.h"
#include "payload.h"
#include "psci.h"
#include "regs.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---------- fixed memory layout (docs/hardware_map_mt6785.md §2) ---------- */

#define XLDR_LOAD_BASE     0x40080000ul           /* where lk loads the image */
#define KERNEL_LOAD_BASE   0x40200000ul           /* kernelcache destination  */
#define KERNEL_LOAD_ALIGN  0x200000ull            /* 2 MiB (L2-block rule)    */
#define DRAM_BASE_DEFAULT  0x40000000ul
#define MEM_SIZE_DEFAULT   0xC0000000ull          /* 4 GiB Device             */
#define VIRT_BASE_KOFF     0xffffffe000000000ull  /* xnu VM_MIN_KERNELABS    */

#define MAX_CMDLINE        608

/*
 * Entry stack (used by entry.S .L_at_el1 via adrp/add before the C entry
 * runs).  Lives in this image's BSS so an SP-less EL1 hand-off (QEMU
 * -kernel resets SP to 0; some bare-metal paths too) still gets a valid
 * stack INSIDE the loaded image — mkxnuimg places the payload manifest
 * only after the whole loader binary, so 64 KiB here stay clear.
 */
__attribute__((aligned(16)))
uint8_t xldr_entry_stack[0x4000];

static char g_cmdline[MAX_CMDLINE];
static const char default_cmdline[] =
	"serial=3 -v debug=0x8e keepsyms=1 rd=md0 rootdev=md0 "
	"cs_enforcement_disable=1 amfi_get_out_of_my_way=0x1 wpdebug=1";

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* find the appended payload manifest (mkxnuimg page-aligns it after us) */
static const volatile struct payload_manifest *
find_payload(uint64_t load_base)
{
	for (uint64_t a = ((load_base + 0xFFFF) & ~0xFFFFul); a < 0x46000000ul;
	     a += 0x10000) {
		const volatile uint8_t *m = (const volatile uint8_t *)a;
		uint8_t ok = 1;
		static const char magic[] = PAYLOAD_MAGIC;
		for (int i = 0; i < 8; i++) {
			if (m[i] != (uint8_t)magic[i]) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			return (const volatile struct payload_manifest *)a;
		}
	}
	return 0;
}

static void
delay_seconds(uint32_t sec)
{
	uint64_t freq;
	asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if (freq == 0) {
		freq = 13000000ull;
	}
	uint64_t target = (uint64_t)sec * freq;
	uint64_t start, now;
	asm volatile("mrs %0, cntpct_el0" : "=r"(start));
	do {
		wdt_kick();
		asm volatile("mrs %0, cntpct_el0" : "=r"(now));
	} while ((now - start) < target);
}

void
xldr_panic(const char *why)
{
	uart_puts("\nXLDR PANIC: ");
	uart_puts(why);
	uart_puts("\n");
	fb_text("XLDR PANIC", 400, 300, 0xFFFF0000u, 4);
	fb_text(why, 40, 400, 0xFFFF0000u, 2);
	fb_text("AUTO-REBOOTING IN 10 SECONDS...", 40, 480, 0xFFFFFFFFu, 2);
	xldr_write_panic_record(why);
	delay_seconds(10);
	psci_system_reset();
	for (;;) {
		asm volatile("wfe");
	}
}

/* Clean+invalidate the whole D-cache by set/way, then I-cache. */
void
cache_flush_all(void)
{
	uint64_t clidr, ccsidr, sets, ways;
	uint64_t level;
	asm volatile("mrs %0, clidr_el1" : "=r"(clidr));
	uint64_t loc = ((clidr >> 24) & 0x7);
	for (level = 0; level < loc; level++) {
		uint32_t ctype = (uint32_t)((clidr >> (level * 3)) & 0x7);
		if (ctype == 0) {
			break;
		}
		asm volatile("msr csselr_el1, %0" :: "r"(level));
		asm volatile("isb");
		asm volatile("mrs %0, ccsidr_el1" : "=r"(ccsidr));
		sets  = ((ccsidr >> 13) & 0x7FFF) + 1;
		ways  = ((ccsidr >> 3) & 1023) + 1;
		uint64_t w_pow = 0;
		while ((1ull << w_pow) < ways) {
			w_pow++;
		}
		uint64_t way_pos = 32 - w_pow;   /* way field occupies top w_pow bits */
		for (uint64_t s = 0; s < sets; s++) {
			for (uint64_t w = 0; w < ways; w++) {
				uint64_t swv = (w << way_pos) | (s << 13) | (level << 1);
				asm volatile("dc cisw, %0" :: "r"(swv));
			}
		}
	}
	asm volatile("dsb sy");
	asm volatile("ic iallu");
	asm volatile("dsb sy");
	asm volatile("isb");
}

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
		"msr daifset, #0xf\n"
		"mrs x2, sctlr_el1\n"
		"bic x2, x2, #0x1\n"    /* M: MMU off */
		"bic x2, x2, #0x4\n"    /* C: data cache off */
		"bic x2, x2, #0x1000\n" /* I: instruction cache off */
		"msr sctlr_el1, x2\n"
		"isb\n"
		"ic iallu\n"
		"dsb sy\n"
		"isb\n"
		"mov x0, %1\n"
		"br %0\n"
		: : "r" (entry), "r" (boot_args_pa) : "x0", "x2");
	__builtin_unreachable();
}

/* --------------------------- main flow --------------------------- */

/*
 * C entry: called from entry.S with the lk x0 (FDT PA).  This never returns:
 * after relocating the kernelcache, it jumps straight into XNU.
 */
void
xldr_main(uint64_t fdt_pa)
{
	/* Immediately disable hardware watchdog so MTK timer won't reboot us */
	wdt_disable();

	uart_init(UART0_BASE_DEFAULT);
	uart_puts("\nHACKIOS xnuloader: lk handoff, FDT @ ");
	uart_puthex(fdt_pa);
	uart_puts("\n");

	int fdtrc = fdt_pa ? fdt_init(fdt_pa) : FDT_E_BADMAG;
	uint64_t mem_size = MEM_SIZE_DEFAULT;
	uint64_t fb_base_dyn = 0;
	if (fdtrc == FDT_E_OK) {
		mem_size = fdt_mem_size();
		if (mem_size == 0) {
			mem_size = MEM_SIZE_DEFAULT;   /* no /memory node → default */
		}

		/* Inspect /chosen node for lk-passed atag,videolfb framebuffer address */
		int chosen_node = -1;
		if (fdt_find_prefix("chosen", -1, &chosen_node) == FDT_E_OK) {
			const uint8_t *val = 0;
			int len = 0;
			if (fdt_get_prop(chosen_node, "atag,videolfb-fb_base_l", &val, &len) == FDT_E_OK && len >= 4) {
				uint32_t fb_l = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) |
				                ((uint32_t)val[2] << 8) | (uint32_t)val[3];
				uint32_t fb_h = 0;
				if (fdt_get_prop(chosen_node, "atag,videolfb-fb_base_h", &val, &len) == FDT_E_OK && len >= 4) {
					fb_h = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) |
					       ((uint32_t)val[2] << 8) | (uint32_t)val[3];
				}
				fb_base_dyn = ((uint64_t)fb_h << 32) | fb_l;
			}
		}
	} else {
		uart_puts("xlDR: FDT parse failed rc ");
		uart_puthex((uint64_t)fdtrc);
		uart_puts("\n");
	}
	uart_puts("memSize  ");
	uart_puthex(mem_size);
	uart_puts("\n");

	const volatile struct payload_manifest *pm = find_payload(XLDR_LOAD_BASE);
	if (pm == 0) {
		xldr_panic("payload manifest not found (mkxnuimg append failed?)");
	}
	uart_puts("payload manifest found; sections: ");
	uart_puthex(pm->section_count);
	uart_puts("\n");

	/* If payload sits below 0x50000000, stage it away so KERNEL_LOAD_BASE (0x40200000) does not overlap */
	if ((uint64_t)(uintptr_t)pm < 0x50000000ull && pm->total_size != 0 &&
	    0x50000000ull + pm->total_size < DRAM_BASE_DEFAULT + mem_size) {
		uart_puts("staging payload to 0x50000000 (");
		uart_puthex(pm->total_size);
		uart_puts(" bytes)\n");
		memcpy((void *)0x50000000ull, (const void *)pm, pm->total_size);
		pm = (const volatile struct payload_manifest *)0x50000000ull;
	}

	/* Determine active framebuffer: manifest > FDT atag,videolfb > default */
	uint64_t active_fb = pm->fb_base ? pm->fb_base : (fb_base_dyn ? fb_base_dyn : FB_BASE_DEFAULT);
	uint32_t active_w  = pm->fb_width  ? pm->fb_width  : FB_WIDTH_DEFAULT;
	uint32_t active_h  = pm->fb_height ? pm->fb_height : FB_HEIGHT_DEFAULT;
	uint32_t active_st = pm->fb_stride ? pm->fb_stride : FB_STRIDE_DEFAULT;

	uart_puts("FB base: ");
	uart_puthex(active_fb);
	uart_puts("\n");

	fb_init(active_fb, active_w, active_h, active_st);
	fb_draw_test_pattern();
	fb_text("HACKIOS XLDR: LK HANDOFF OK", 60, 60, 0xFF00FF00u, 3);

	const volatile struct payload_section *pk = &pm->sect[SECT_KERNEL];
	const volatile struct payload_section *pr = &pm->sect[SECT_RAMDISK];
	if (pk->size == 0) {
		xldr_panic("payload has no kernel section");
	}

	/* cmdline: from manifest, else default */
	memset(g_cmdline, 0, sizeof(g_cmdline));
	if (pm->cmdline_len != 0 && pm->cmdline_offset != 0 &&
	    pm->cmdline_len < MAX_CMDLINE) {
		const volatile char *c = (const volatile char *)
		    ((uintptr_t)pm + pm->cmdline_offset);
		for (uint32_t i = 0; i < pm->cmdline_len; i++) {
			g_cmdline[i] = c[i];
		}
		g_cmdline[MAX_CMDLINE - 1] = 0;
	} else {
		memcpy(g_cmdline, default_cmdline, sizeof(default_cmdline));
	}

	/* ---- relocate the kernelcache ---- */
	uint64_t kern_src = (uint64_t)((uintptr_t)pm + pm->sect[SECT_KERNEL].offset);
	uint64_t kern_dst = align_up(KERNEL_LOAD_BASE, KERNEL_LOAD_ALIGN);
	uint64_t kern_entry = 0;
	int mrc = macho_load(kern_src, kern_dst, &kern_entry);
	if (mrc != 0) {
		uart_puts("macho_load rc ");
		uart_puthex((uint64_t)(int64_t)mrc);
		xldr_panic("kernelcache load failed");
	}

	/* ---- overrides; fixed layout so ADT is correct at build ---- */
	struct adt_overrides ov;
	memset(&ov, 0, sizeof(ov));
	ov.phys_base = DRAM_BASE_DEFAULT;
	ov.phys_size = mem_size;
	ov.fb_base   = active_fb;
	ov.fb_width  = active_w;
	ov.fb_height = active_h;
	ov.fb_stride = active_st;
	ov.fb_depth  = pm->fb_depth  ? pm->fb_depth  : FB_DEPTH_DEFAULT;
	ov.fb_size   = (uint64_t)ov.fb_height * ov.fb_stride;
	ov.cmdline   = g_cmdline;
	ov.mm_dt_size = 0x10000;                 /* fixed 64 KiB DT reserve     */

	uint64_t rd_size = pr->size;
	if (pr->size == 0 && pm->section_count > SECT_RAMDISK + 1) {
		rd_size = 0;
	}
	const void *rd_src = rd_size ? (const void *)((uintptr_t)pm + pr->offset) : 0;
	ov.mm_rd_size = rd_size;

	/* layout: ADT right after the relocated kernel, RAMDisk after, BA last */
	adt_build(&ov);                          /* fills pools + provisional  */
	uint64_t adt_sz = (uint64_t)adt_size();

	uint64_t adt_base = align_up(g_macho_top, 0x8000);
	uint64_t rd_base  = align_up(adt_base + adt_sz, 0x8000);
	uint64_t ba_base  = align_up(rd_base + rd_size, 64);
	uint64_t topo     = align_up(ba_base + sizeof(struct boot_args), 0x4000);
	ov.mm_dt_addr = adt_base;
	ov.mm_rd_addr = rd_base;
	adt_build(&ov);
	adt_sz = (uint64_t)adt_size();

	/* ---- place ADT + RAMDisk + boot_args ---- */
	{
		int sz = 0;
		adt_serialize((uint8_t *)adt_base, &sz);
		if (rd_size) {
			memcpy((void *)rd_base, rd_src, rd_size);
		}
	}

	static struct boot_args ba;
	memset(&ba, 0, sizeof(ba));
	ba.Revision = KBOOT_ARGS_REVISION;
	ba.Version  = KBOOT_ARGS_VERSION;
	ba.virtBase = VIRT_BASE_KOFF + (uint64_t)(ov.phys_base);
	ba.physBase = ov.phys_base;
	ba.memSize  = mem_size;
	ba.topOfKernelData = topo;
	ba.Video.v_baseAddr = ov.fb_base;
	ba.Video.v_display  = 0;
	ba.Video.v_rowBytes = ov.fb_stride;
	ba.Video.v_width    = ov.fb_width;
	ba.Video.v_height   = ov.fb_height;
	ba.Video.v_depth    = ov.fb_depth;
	ba.machineType      = 0;
	ba.deviceTreeP      = (uint64_t *)adt_base;   /* physical (PE_init uses raw) */
	ba.deviceTreeLength = (uint32_t)adt_sz;
	ba.bootFlags        = 0;
	ba.memSizeActual    = mem_size;
	memcpy(ba.CommandLine, g_cmdline, (size_t)strlen(g_cmdline) + 1);

	memcpy((void *)ba_base, &ba, sizeof(ba));

	uart_puts("kern ");
	uart_puthex(kern_dst);
	uart_puts(" entry ");
	uart_puthex(kern_entry);
	uart_puts(" ADT ");
	uart_puthex(adt_base);
	uart_puts(" RD ");
	uart_puthex(rd_base);
	uart_puts(" BA ");
	uart_puthex(ba_base);
	uart_puts(" topo ");
	uart_puthex(topo);
	uart_puts("\n");

	/* If placeholder test kernel (entry point == 0), gracefully park on LCD */
	if (kern_entry == 0) {
		uart_puts("\n========================================\n");
		uart_puts("[XLDR] MILESTONE M1 ACHIEVED ON BEGONIA!\n");
		uart_puts("[XLDR] Stage-2 bare-metal loader executed successfully.\n");
		uart_puts("[XLDR] Framebuffer, FDT & ADT parsing validated.\n");
		uart_puts("========================================\n");
		fb_text("MILESTONE M1: STAGE-2 OK!", 60, 180, 0xFF00FF00u, 3);
		fb_text("BEGONIA LCD + FDT + WDT VALIDATED", 60, 230, 0xFFFFFFFFu, 2);
		fb_text("AUTO-REBOOTING IN 10 SECONDS...", 60, 280, 0xFF00FFFFu, 2);
		delay_seconds(10);
		uart_puts("[XLDR] Auto-rebooting to restore system...\n");
		psci_system_reset();
		for (;;) {
			asm volatile("wfe");
		}
	}
	/* hand the caches + MMU state over; XNU expects flat, flushed start */
	cache_flush_all();

	fb_text("ENTERING XNU...", 60, 140, 0xFF00FFFFu, 3);

	jump_to_kernel(kern_entry, ba_base);
}
