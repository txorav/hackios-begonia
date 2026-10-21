/*
 * lx_printk.h — print/log plumbing for the lx shim layer.
 *
 * ---------------------------------------------------------------------------
 * ROLE IN THE lx STACK
 * ---------------------------------------------------------------------------
 * Linux drivers use pr_info()/dev_info()/printk().  The shim keeps those
 * *names* so the mirrored (clean-room) driver code in drivers/mt6785 is
 * diffable against its Linux reference, but the implementation is routed
 * through lxkit:
 *
 *      printk/pr_info -> lx_console_write(level, buf, len)
 *                             |
 *                             v
 *      lxkit_console (lxkit_console.cpp, compiled into the owning kext)
 *                             |
 *                             +-- IOLog()/kprintf() (XNU, level-tagged)
 *                             +-- optional early bypass: direct MMIO to
 *                                 UART0 @ 0x11002000 (bring-up only; above
 *                                 is preferred once kprintf is live).
 *
 * The console sink is installed by lxkit_init_console() (glue, kernel side)
 * or lx_host_console_install() (host tests).  Until installed, printk output
 * is dropped (drivers must never spin waiting for console).
 *
 * ---------------------------------------------------------------------------
 * LEVELS
 * ---------------------------------------------------------------------------
 * We mirror the Linux 8-level scheme (KERN_... prefix + pr_* wrappers) with
 * numeric levels 0..7, 7 = most verbose.  The kernel sink filters levels
 * above (>) the boot-configured ceiling.
 */

#ifndef LX_PRINTK_H
#define LX_PRINTK_H

#include <stdarg.h>
#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Console sink function pointer — installed by lxkit (kernel) or tests. */
typedef void (*lx_console_sink_t)(int level, const char *buf, size_t len);

/* Linux kernel log levels, 0..7. */
#define LX_LOGLEVEL_EMERG   0   /* system unusable                   */
#define LX_LOGLEVEL_ALERT   1   /* action must be taken immediately  */
#define LX_LOGLEVEL_CRIT    2   /* critical conditions               */
#define LX_LOGLEVEL_ERR     3   /* error conditions                  */
#define LX_LOGLEVEL_WARNING 4   /* warning conditions                */
#define LX_LOGLEVEL_NOTICE  5   /* normal but significant            */
#define LX_LOGLEVEL_INFO    6   /* informational                     */
#define LX_LOGLEVEL_DEBUG   7   /* debug-level messages              */

/* KERN_* prefixes (mirrored for diff-friendly call sites).  The lx console
 * decodes the "\001<N>" prefix exactly like Linux printk does and routes
 * the message to that level, stripping the byte pair. */
#define KERN_SOH      "\001"
#define KERN_EMERG    KERN_SOH "0"
#define KERN_ALERT    KERN_SOH "1"
#define KERN_CRIT     KERN_SOH "2"
#define KERN_ERR      KERN_SOH "3"
#define KERN_WARNING  KERN_SOH "4"
#define KERN_NOTICE   KERN_SOH "5"
#define KERN_INFO     KERN_SOH "6"
#define KERN_DEBUG    KERN_SOH "7"

/* Default ceiling: show ERR and above until bring-up turns the faucet on. */
extern volatile int lx_console_max_level;

/* Sink registration.  Idempotent install of the (only) sink per image. */
void lx_console_install(lx_console_sink_t sink);
void lx_console_write(int level, const char *buf, size_t len);

/* Raw strlen (avoid pulling libc into freestanding drivers). */
static inline size_t lx_strlen(const char *s)
{
	size_t n = 0;
	while (s[n] != '\0')
		n++;
	return n;
}

/* Small static-buffer vsnprintf substitute:
 *   lx_snprintf(dst, dst_size, "%s/%d %08lx", ...)
 * We implement ONLY the specifiers the drivers use:
 *   %d %u %x %08x %ld %lu %llu %llx %p %s %c %%
 * (Fixed stack of conversions keeps the code auditable; panic/abort on
 * malformed input is unacceptable — silently stop formatting instead.)
 */
size_t lx_vsnprintf(char *dst, size_t dst_size, const char *fmt, va_list ap);
size_t lx_snprintf(char *dst, size_t dst_size, const char *fmt, ...);

/* --------------------------------------------------------------------- */
/* printk + pr_* family (names preserved from Linux; behaviour identical) */
/* --------------------------------------------------------------------- */

int  printk(const char *fmt, ...);
int  pr_info(const char *fmt, ...);
int  pr_err(const char *fmt, ...);
int  pr_warn(const char *fmt, ...);
int  pr_debug(const char *fmt, ...);

/* netdev-style, device-tagged helpers: "[msdc1@ villagers]" style prefix.
 * `dev` param is generic void* so host tests can pass a fake device. */
int dev_info(void *dev, const char *fmt, ...);
int dev_err(void *dev, const char *fmt, ...);
int dev_warn(void *dev, const char *fmt, ...);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_PRINTK_H */
