/*
 * lxkit_console.cpp — printk/pr_* plumbing into the XNU console (IOLog),
 *                     plus the freestanding formatter the drivers use.
 *
 * DELIVERY: everything below LOG_DEBUG goes through IOLog() (chronology +
 * syslog tag) and level DEBUG messages are level-gated by the ceiling.
 * During very early bring-up (before IOKit timer/iokit full live) the
 * level=0 (EMERG) messages are additionally dropped on the lk framebuffer
 * via vc? Not needed: XNU's PE_init_iokit console handles it (see
 * the display kext README for the vc_progress note).
 *
 * IMPORTANT contextual note: printk must be evidence-safe at interrupt
 * level. It enqueues into a static ring (CONFIG-enforced), prints light.
 */

#include "lxkit_types.h"

#include "libkern/c++/OSMetaClass.h"
#include "IOKit/IOLib.h"

#include "wifi/lx/include/lx/lx_printk.h"
#include "wifi/lx/include/lx/lx_errno.h"
#include "wifi/lx/include/lx/lx_dev.h"
#include "wifi/lx/include/lx/lxkit_api.h"

/* default ceiling: print until lxkit_lcd_devel config says otherwise */
volatile int lx_console_max_level = LX_LOGLEVEL_DEBUG;

static lx_console_sink_t g_sink = NULL;

void
lx_console_install(lx_console_sink_t sink)
{
	g_sink = sink;
}

void
lx_console_write(int level, const char *buf, size_t len)
{
	if (buf == NULL || len == 0)
		return;
	if ((unsigned int)level > (unsigned int)lx_console_max_level)
		return;
	if (g_sink != NULL) {
		g_sink(level, buf, len);
		return;
	}
	/* Fallback: no sink installed (very early).  No IOLog from driver
	 * C-code; emit raw kprintf so bring-up phase M2 also sees output. */
	kprintf("lx[%d]: ", level);
	for (size_t i = 0; i < len; i++)
		kprintf((buf[i] == '%') ? "%%" : "%c", buf[i]);
	kprintf("\n");
}

/* ==========================================================================
 * lx_vsnprintf / lx_snprintf — small, freestanding formatter
 * specifiers: %d %u %x %08x %ld %lu %llu %llx %p %s %c %%
 * ========================================================================== */

static size_t
lx_copy_out(char *dst, size_t cap, size_t pos, const char *src, size_t n)
{
	while (n-- != 0) {
		if (pos + 1 < cap)
			dst[pos++] = *src++;
		else
			return pos;      /* cap reached: stop copying */
	}
	return pos;
}

static size_t
lx_emit_num(char *dst, size_t cap, size_t pos, u64 value,
    unsigned int base, bool uppercase, bool pad, unsigned int width)
{
	char tmp[24];                     /* 64-bit dec max 20; hex 16 */
	const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
	unsigned int i = 0;

	if (value == 0) {
		tmp[i++] = '0';
	} else {
		while (value != 0) {
			tmp[i++] = digits[value % base];
			value /= base;
		}
	}
	while (pad && i < width)
		tmp[i++] = '0';
	if (i >= sizeof(tmp))
		return pos;

	/* reverse copy */
	while (i != 0)
	{
		i--;
		pos = lx_copy_out(dst, cap, pos, &tmp[i], 1);
	}
	return pos;
}

size_t
lx_vsnprintf(char *dst, size_t dst_size, const char *fmt, va_list ap)
{
	size_t pos = 0;

	if (dst == NULL || fmt == NULL || dst_size == 0)
		return 0;

	while (*fmt != '\0') {
		if (*fmt != '%') {
			pos = lx_copy_out(dst, dst_size, pos, fmt, 1);
			fmt++;
			continue;
		}
		fmt++;                    /* past '% */

		if (*fmt == '%') {        /* literal '%' */
			pos = lx_copy_out(dst, dst_size, pos, fmt, 1);
			fmt++;
			continue;
		}

		/* optional '08' width (only kpathname used by drivers) */
		bool pad = false;
		unsigned int width = 0;
		while (*fmt >= '0' && *fmt <= '9') {
			if (*fmt == '0' && width == 0)
				pad = true;
			width = width * 10 + (u32)(*fmt - '0');
			fmt++;
		}

		/* l / ll length modifiers */
		bool is_long = false;
		while (*fmt == 'l') {
			is_long = true;
			fmt++;
		}

		switch (*fmt) {
		case 'd': case 'i': {
			long long v = 0;
			if (is_long)
				v = va_arg(ap, long long);
			else
				v = (long long)va_arg(ap, int);
			u64 mag = (v < 0) ? (u64)(-(v + 1)) + 1 : (u64)v;
			if (v < 0)
				pos = lx_copy_out(dst, dst_size, pos, "-", 1);
			pos = lx_emit_num(dst, dst_size, pos, mag, 10,
			    false, pad, width);
			break;
		}
		case 'u': {
			u64 v;
			if (is_long)
				v = va_arg(ap, unsigned long long);
			else
				v = (u64)va_arg(ap, unsigned int);
			pos = lx_emit_num(dst, dst_size, pos, v, 10, false,
			    pad, width);
			break;
		}
		case 'x': case 'X': {
			u64 v;
			if (is_long)
				v = va_arg(ap, unsigned long long);
			else
				v = (u64)va_arg(ap, unsigned int);
			pos = lx_emit_num(dst, dst_size, pos, v, 16,
			    (*fmt == 'X'), pad, width);
			break;
		}
		case 'p': {
			void *v = va_arg(ap, void *);
			pos = lx_copy_out(dst, dst_size, pos, "0x", 2);
			pos = lx_emit_num(dst, dst_size, pos, (u64)v, 16,
			    false, true, 8);
			break;
		}
		case 's': {
			const char *s = va_arg(ap, const char *);
			size_t slen = s != NULL ? lx_strlen(s) : 6;
			if (s == NULL)
				s = "(null)";
			pos = lx_copy_out(dst, dst_size, pos, s, slen);
			break;
		}
		case 'c': {
			int c = va_arg(ap, int);
			char b = (char)c;
			pos = lx_copy_out(dst, dst_size, pos, &b, 1);
			break;
		}
		default:
			/* unknown: skip both chars (never panics) */
			break;
		}
		fmt++;
	}

	if (pos >= dst_size)
		pos = dst_size - 1;
	dst[pos] = '\0';
	return pos;
}

size_t
lx_snprintf(char *dst, size_t dst_size, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	size_t n = lx_vsnprintf(dst, dst_size, fmt, ap);
	va_end(ap);
	return n;
}

/* ==========================================================================
 * printk family (names mirror Linux; output lands via lx_console_write).
 * C/C++ cannot forward ellipsis; each wrapper opens its own va_list.
 * ========================================================================== */
static int
lx_emit_at(int level, const char *fmt, va_list ap)
{
	char buf[320];                    /* drivers never emit longer than this */

	/* Linux-style "\001<N>" level prefix decode (see KERN_* in
	 * lx_printk.h); mirrors the printk() behaviour of the reference. */
	if (fmt != NULL && (unsigned char)fmt[0] == 0x01 &&
	    fmt[1] >= '0' && fmt[1] <= '7') {
		level = (int)(fmt[1] - '0');
		fmt += 2;
	}

	size_t n = lx_vsnprintf(buf, sizeof(buf), fmt, ap);
	lx_console_write(level, buf, n);
	return (int)n;
}

/* Raw printk: Linux's printk() without a level letter resolves to level 6
 * (KERN_INFO semantics).  "<3>"-style embedding is NOT deciphered; drivers
 * that want levels use the pr_* helpers below. */
int
printk(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_emit_at(LX_LOGLEVEL_INFO, fmt, ap);
	va_end(ap);
	return n;
}

int pr_info(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_emit_at(LX_LOGLEVEL_INFO, fmt, ap);
	va_end(ap);
	return n;
}

int pr_err(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_emit_at(LX_LOGLEVEL_ERR, fmt, ap);
	va_end(ap);
	return n;
}

int pr_warn(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_emit_at(LX_LOGLEVEL_WARNING, fmt, ap);
	va_end(ap);
	return n;
}

int pr_debug(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_emit_at(LX_LOGLEVEL_DEBUG, fmt, ap);
	va_end(ap);
	return n;
}

/* ==========================================================================
 * device-tagged variants: "[name] " prefix; dev arg is void* at the ABI
 * (host-test stubs pass fakes).
 * ========================================================================== */
static int
lx_dev_printk(int level, void *dev, const char *fmt, va_list ap)
{
	struct lx_device *d = (struct lx_device *)dev;
	char buf[352];                    /* tag + payload */
	size_t tl = 0, n;

	if (d != NULL) {
		char tag[48];
		lx_snprintf(tag, sizeof(tag), "[%s] ", d->name);
		tl = lx_strlen(tag);
	}
	n = lx_vsnprintf(buf + tl, sizeof(buf) - tl, fmt, ap);
	lx_console_write(level, buf, tl + n);
	return (int)(tl + n);
}

int dev_info(void *dev, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_dev_printk(LX_LOGLEVEL_INFO, dev, fmt, ap);
	va_end(ap);
	return n;
}

int dev_err(void *dev, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_dev_printk(LX_LOGLEVEL_ERR, dev, fmt, ap);
	va_end(ap);
	return n;
}

int dev_warn(void *dev, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = lx_dev_printk(LX_LOGLEVEL_WARNING, dev, fmt, ap);
	va_end(ap);
	return n;
}
