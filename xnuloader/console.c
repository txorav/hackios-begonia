/*
 * console.c — early console for xnuloader:
 *   - UART0: MediaTek MT6785, 16550-compatible with 32-bit registers,
 *     THRE is bit 5 of LSR at offset 0x14 (lk leaves it configured, so we
 *     only write, we never re-init clocks).
 *   - Framebuffer: RGBX8888 linear buffer left initialized by lk
 *     (1080x2340 stride 4320 by default; override via payload manifest).
 *   - pstore / ramoops: 64 KiB at 0x47df0000 (warm-reset persistent).
 */
#include "console.h"
#include "font_3x5.h"
#include <stdint.h>

static volatile uint32_t *uart_base;
static volatile uint32_t *fb_base;
static uint32_t fb_w = FB_WIDTH_DEFAULT;
static uint32_t fb_h = FB_HEIGHT_DEFAULT;
static uint32_t fb_stride = FB_STRIDE_DEFAULT;   /* bytes */

/* ---------------- UART (16550-style, 32-bit regs) ---------------- */

void
uart_init(uint64_t base)
{
	uart_base = (volatile uint32_t *)(base);
	/* lk has the UART programmed (clock + baud); only clear-parity */
}

static void
uart_write8(uint32_t off, uint8_t v)
{
	uart_base[off] = v;
}

static void ramoops_putc(char c);

void
uart_putc(char c)
{
	ramoops_putc(c);
	if (!uart_base) {
		return;
	}
	/* MTK 16550: LSR at 0x14, THRE = bit 5 (ready when set).
	 * QEMU -M virt runs a PL011: FR at 0x18, TXFF = bit 5 (busy when set).
	 * Select at compile time via -DXLDR_UART_PL011 (qemu/run_qemu.sh). */
	uint32_t spin = 4000000;
#ifdef XLDR_UART_PL011
	while ((uart_base[0x18 >> 2] & 0x20) && spin--) {
		;
	}
#else
	while (!(uart_base[0x14 >> 2] & 0x20) && spin--) {
		;
	}
#endif
	uart_write8(0, (uint8_t)c);
	if (c == '\n') {
		uart_write8(0, '\r');
	}
}

void
uart_puts(const char *s)
{
	while (*s) {
		uart_putc(*s++);
	}
}

static const char hexd[] = "0123456789abcdef";

void
uart_puthex(uint64_t v)
{
	uart_puts("0x");
	for (int i = 60; i >= 0; i -= 4) {
		uart_putc(hexd[(v >> i) & 0xf]);
	}
}

/* ---------------- framebuffer (direct scanout writes) ---------------- */

void
fb_init(uint64_t base, uint32_t width, uint32_t height, uint32_t stride)
{
	if (!base) {
		return;
	}
	fb_base = (volatile uint32_t *)(base);
	if (width)  { fb_w = width; }
	if (height) { fb_h = height; }
	if (stride) { fb_stride = stride; }
}

void
fb_fill(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t rgb)
{
	if (!fb_base) { return; }
	volatile uint32_t *row = fb_base + (uint64_t)y0 * (fb_stride / 4);
	for (uint32_t y = y0; y < y1 && y < fb_h; y++, row += fb_stride / 4) {
		volatile uint32_t *px = row + x0;
		for (uint32_t x = x0; x < x1 && x < fb_w; x++) {
			*px++ = rgb;
		}
	}
}

void
fb_clear(uint32_t rgb)
{
	if (!fb_base) { return; }
	volatile uint32_t *row = fb_base;
	const uint64_t words = ((uint64_t)fb_stride / 4) * fb_h;
	for (uint64_t i = 0; i < words; i++) {
		row[i] = rgb;
	}
}

/* draw one glyph, scaled; scale>=1 -> scale px per font pixel, in BOTH axes */
static void
fb_glyph(char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t scale)
{
	char uc = (char)((c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c);
	uint32_t idx = (uint32_t)(unsigned char)uc;
	if (idx < 32 || idx > 126) {
		idx = 0;
	} else {
		idx -= 32;
	}
	const uint8_t *rows = font3x5[idx];
	for (uint32_t r = 0; r < 5; r++) {
		uint8_t bits = rows[r];
		for (uint32_t cnum = 0; cnum < 3; cnum++) {
			if (!(bits & (4 >> cnum))) {
				continue;
			}
			uint32_t gx = x + cnum * scale;
			uint32_t gy = y + r * scale;
			fb_fill(gx, gy, gx + scale, gy + scale, fg);
		}
	}
}

void
fb_text(const char *s, uint32_t x, uint32_t y, uint32_t fg, uint32_t scale)
{
	if (!fb_base) { return; }
	while (*s) {
		fb_glyph(*s++, x, y, fg, scale);
		x += 4 * scale;
	}
}

void
fb_draw_test_pattern(void)
{
	if (!fb_base) { return; }
	/* Clean solid deep black canvas */
	fb_clear(0xFF000000u);

	/* Clean solid white border */
	fb_fill(0, 0, fb_w, 12, 0xFFFFFFFFu);
	fb_fill(0, fb_h - 12, fb_w, fb_h, 0xFFFFFFFFu);
	fb_fill(0, 0, 12, fb_h, 0xFFFFFFFFu);
	fb_fill(fb_w - 12, 0, fb_w, fb_h, 0xFFFFFFFFu);

	/* Solid status card header */
	fb_fill(30, 30, fb_w - 30, 130, 0xFF142414u);
	fb_text("HACKIOS BEGONIA - STAGE-2 OK", 60, 50, 0xFF00FF00u, 4);
	fb_text("HELIO G90T (MT6785) - DARWIN 22.5.0", 60, 95, 0xFFFFFFFFu, 2);
}

/* ---------------- MediaTek MT6785 TOPRGU Watchdog & Reset ---------------- */

void
wdt_kick(void)
{
	volatile uint32_t *restart = (volatile uint32_t *)MTK_WDT_RESTART;
	*restart = MTK_WDT_RESTART_KEY;
}

void
wdt_disable(void)
{
	wdt_kick();
	volatile uint32_t *mode = (volatile uint32_t *)MTK_WDT_MODE;
	uint32_t v = *mode;
	v |= MTK_WDT_MODE_KEY;
	v &= ~(1u | 8u | 0x40u); /* clear ENABLE, IRQ, DUAL_MODE */
	*mode = v;
}

void
wdt_arm(uint32_t seconds)
{
	if (seconds > 31) {
		seconds = 31;
	}
	if (seconds == 0) {
		seconds = 1;
	}
	/*
	 * MTK TOPRGU WDT_LENGTH:
	 * Resolution: 512 ticks / 32768 Hz = 1/64 s (~15.625 ms) per tick.
	 * Length field is bits [15:5]. Key is bits [3:0] = 0x8.
	 */
	uint32_t ticks = seconds * 64u;
	if (ticks > 0x7FFu) {
		ticks = 0x7FFu;
	}
	volatile uint32_t *length = (volatile uint32_t *)MTK_WDT_LENGTH;
	*length = (ticks << 5) | MTK_WDT_LENGTH_KEY;

	wdt_kick();

	/* Enable watchdog: bit 0 (EN) + bit 2 (EXRST to MT6359 PMIC) with key 0x22000000 */
	volatile uint32_t *mode = (volatile uint32_t *)MTK_WDT_MODE;
	*mode = MTK_WDT_MODE_KEY | (1u << 0) | (1u << 2);
}

void
system_reboot_hardware(void)
{
	uart_puts("\n[XLDR] Triggering hardware reset...\n");

	/* 1. MTK Toprgu mode: Enable + External PMIC reset */
	volatile uint32_t *mode = (volatile uint32_t *)MTK_WDT_MODE;
	*mode = MTK_WDT_MODE_KEY | (1u << 0) | (1u << 2);

	/* 2. Direct software reset trigger */
	volatile uint32_t *swrst = (volatile uint32_t *)MTK_WDT_SWRST;
	*swrst = MTK_WDT_SWRST_KEY;
	*swrst = MTK_WDT_SWRST_KEY;
	*swrst = MTK_WDT_SWRST_KEY;

	/* 3. Fast 1-second watchdog hardware timeout fallback with PMIC EXRST */
	wdt_arm(1);

	/* 4. Fallback to standard PSCI system reset */
	extern int64_t psci_system_reset(void);
	psci_system_reset();

	for (;;) {
		asm volatile("wfe");
	}
}

/* ---------------- persistent ramoops (console-ramoops) ---------------- */

static void
ramoops_putc(char c)
{
	/* Format: struct persistent_ram_buffer { uint32_t sig; uint32_t start; uint32_t size; uint8_t data[]; } */
	volatile uint32_t *hdr = (volatile uint32_t *)RAMOOPS_BASE;
	if (hdr[0] != 0x43474244u) { /* 'DBGC' */
		hdr[0] = 0x43474244u;
		hdr[1] = 0;
		hdr[2] = 0;
	}
	uint32_t sz = hdr[2];
	if (sz + 1 < (RAMOOPS_SIZE - 16)) {
		volatile uint8_t *data = (volatile uint8_t *)(RAMOOPS_BASE + 12);
		data[sz] = (uint8_t)c;
		hdr[2] = sz + 1;
	}
}

void
xldr_write_panic_record(const char *why)
{
	const char hdr[] = "\n\n*** XLDRPANIC ***\n";
	for (size_t i = 0; hdr[i]; i++) {
		ramoops_putc(hdr[i]);
	}
	if (why) {
		for (size_t i = 0; why[i]; i++) {
			ramoops_putc(why[i]);
		}
	}
	ramoops_putc('\n');

	/* Also write legacy flat record to fallback address for compatibility */
	volatile uint8_t *rp = (volatile uint8_t *)RAMOOPS_BASE_FALLBACK;
	const char flat_hdr[] = "XLDRPANIC\n";
	for (size_t i = 0; i < sizeof(flat_hdr) - 1; i++) {
		rp[i] = flat_hdr[i];
	}
	size_t n = sizeof(flat_hdr) - 1;
	while (why && *why && n < 0x2000) {
		rp[n++] = (uint8_t)*why++;
	}
	rp[n++] = '\n';
}
