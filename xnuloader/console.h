/*
 * console.h — early console: MediaTek UART0 (0x11002000, 16550-style, 32-bit
 * regs) + the lk-initialized framebuffer (test pattern + tiny 3x5 text).
 */
#ifndef _XLDR_CONSOLE_H_
#define _XLDR_CONSOLE_H_

#include <stdint.h>

#ifdef XLDR_UART_PL011
#define UART0_BASE_DEFAULT  0x09000000ul   /* QEMU virt PL011 */
#else
#define UART0_BASE_DEFAULT  0x11002000ul   /* MediaTek UART0 (lk-configured) */
#endif
#define FB_BASE_DEFAULT     0x7e605000ul   /* Begonia LK linear framebuffer (atag,videolfb) */
#define FB_WIDTH_DEFAULT    1080
#define FB_HEIGHT_DEFAULT   2340
#define FB_STRIDE_DEFAULT   4352           /* 1088 words * 4 = 4352 bytes */
#define FB_DEPTH_DEFAULT    32

void uart_init(uint64_t base);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(uint64_t v);

/* framebuffer console; stride in bytes; depth 32bpp RGBX */
void fb_init(uint64_t base, uint32_t width, uint32_t height, uint32_t stride);
void fb_clear(uint32_t rgb);
void fb_text(const char *s, uint32_t x, uint32_t y, uint32_t fg, uint32_t scale);
void fb_draw_test_pattern(void);   /* M1 visual proof: gradient + border + text */

/* watchdog: MediaTek MT6785 TOPRGU (0x10007000) */
#define MTK_WDT_BASE        0x10007000ul
#define MTK_WDT_MODE        (MTK_WDT_BASE + 0x0000)
#define MTK_WDT_RESTART     (MTK_WDT_BASE + 0x0008)
#define MTK_WDT_MODE_KEY    0x22000000u
#define MTK_WDT_RESTART_KEY 0x1971u

void wdt_disable(void);
void wdt_kick(void);

/* panic: XLDRPANIC record into persistent ramoops (0x4d05f000 on begonia) */
void xldr_panic(const char *why) __attribute__((noreturn));   /* see main.c/hal c */
void xldr_write_panic_record(const char *why);

/* Live begonia console-ramoops is at 0x4d05f000 (256 KiB); legacy carveout was 0x47df0000 */
#define RAMOOPS_BASE_FALLBACK 0x47df0000ul
#define RAMOOPS_BASE          0x4d05f000ul
#define RAMOOPS_SIZE          0x40000ul

#endif /* _XLDR_CONSOLE_H_ */
