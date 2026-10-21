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
#define FB_BASE_DEFAULT     0x7f000000ul
#define FB_WIDTH_DEFAULT    1080
#define FB_HEIGHT_DEFAULT   2340
#define FB_STRIDE_DEFAULT   (1080 * 4)      /* RGBX8888 */
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

/* panic: XLDRPANIC record into phys 0x47df0000 (pstore) then WFE forever */
void xldr_panic(const char *why) __attribute__((noreturn));   /* see main.c/hal c */
void xldr_write_panic_record(const char *why);
#define RAMOOPS_BASE 0x47df0000ul
#define RAMOOPS_SIZE 0x10000

#endif /* _XLDR_CONSOLE_H_ */
