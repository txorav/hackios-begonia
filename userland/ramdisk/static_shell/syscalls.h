/*
 * syscalls.h — HackiOS-begonia userland :: ramdisk/static_shell
 *
 * Freestanding Darwin/ARM64 syscall wrappers for the M4 test ramdisk shell.
 * ABI facts (verified against the in-tree XNU 8796.121.2 sources, NOT lore):
 *
 *   * syscall instruction..........  svc #0x80
 *   * BSD syscall number register..  x16
 *     (numbering: xnu/bsd/kern/syscalls.master —
 *      1 exit, 3 read, 4 write, 5 open, 6 close, 20 getpid,
 *      54 ioctl, 59 execve, 95 fsync)
 *   * Mach trap numbering..........  NEGATIVE; mach_msg_trap = -31
 *     (xnu/osfmk/mach/syscall_sw.h:108: kernel_trap(mach_msg_trap,-31,7))
 *   * return convention............ x0; error → CF (carry flag) set,
 *     number in x0 (BSD errno semantics like x86 Darwin code)
 *
 * On a bare kernel boot (M4: launchd not yet present, no launchd bootstrap
 * port, no /dev/console driver stack beyond the console TTY) we deliberately
 * do ONLY BSD console/file I/O. Mach IPC needs ports and a bootstrap — see
 * mach_hello.c for why that is out of scope here.
 */

#ifndef HACKIOS_SYSCALLS_H
#define HACKIOS_SYSCALLS_H

typedef unsigned long uintptr_t;
typedef unsigned long size_t;
typedef long         ssize_t;
typedef long         intptr_t;

#define NULL ((void *)0)

/* BSD syscall numbers (xnu/bsd/kern/syscalls.master, generation of 8796) */
#define SYS_exit        1
#define SYS_read        3
#define SYS_write       4
#define SYS_open        5
#define SYS_close       6
#define SYS_getpid      20
#define SYS_ioctl       54
#define SYS_execve      59
#define SYS_fsync       95

/* open(2) flags (xnu/bsd/sys/fcntl.h) */
#define O_RDONLY          0x0000
#define O_WRONLY          0x0001
#define O_RDWR            0x0002
#define O_CREAT           0x0200
#define O_TRUNC           0x0400

/* Our freestanding errno subset (values match bsd/sys/errno.h) */
#define ENOENT            2
#define EBADF             9
#define EINTR             4

extern int main(int argc, char **argv);

static inline long
syscall0(long n)
{
	register long x8 __asm__("x16") = n;
	register long x0 __asm__("x0") = 0;
	__asm__ volatile("svc #0x80"
	    : "+r"(x0)
	    : "r"(x8)
	    : "cc", "memory");
	return x0;
}

static inline long
syscall1(long n, long a)
{
	register long x8 __asm__("x16") = n;
	register long x0 __asm__("x0") = a;
	__asm__ volatile("svc #0x80"
	    : "+r"(x0)
	    : "r"(x8)
	    : "cc", "memory");
	return x0;
}

static inline long
syscall2(long n, long a, long b)
{
	register long x8 __asm__("x16") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	__asm__ volatile("svc #0x80"
	    : "+r"(x0)
	    : "r"(x1), "r"(x8)
	    : "cc", "memory");
	return x0;
}

static inline long
syscall3(long n, long a, long b, long c)
{
	register long x8 __asm__("x16") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc #0x80"
	    : "+r"(x0)
	    : "r"(x1), "r"(x2), "r"(x8)
	    : "cc", "memory");
	return x0;
}

/* --- minimal C std subset ------------------------------------------------ */

static void sys_exit(int code) { syscall1(SYS_exit, code); for (;;) {} }

static ssize_t sys_read(int fd, void *buf, size_t n)
{ return syscall3(SYS_read, fd, (long)buf, (long)n); }

static ssize_t sys_write(int fd, const void *buf, size_t n)
{ return syscall3(SYS_write, fd, (long)buf, (long)n); }

static int sys_open(const char *path, int flags, int mode)
{ return syscall3(SYS_open, (long)path, flags, mode); }

static int sys_close(int fd) { return syscall1(SYS_close, fd); }

static int sys_getpid(void) { return (int)syscall0(SYS_getpid); }

static void memzero(void *p, size_t n)
{
	unsigned char *d = p;
	while (n--) *d++ = 0;
}

static void memcopy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--) *d++ = *s++;
}

static size_t str_len(const char *s)
{
	size_t n = 0;
	while (s[n] != '\0') n++;
	return n;
}

static int str_eq(const char *a, const char *b)
{
	while (*a == *b) {
		if (*a == '\0') return 1;
		a++; b++;
	}
	return 0;
}

static unsigned long hex_value(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return 0xFFFFFFFFUL;
}

#endif /* HACKIOS_SYSCALLS_H */
