/*
 * main.c — HackiOS-begonia userland :: ramdisk/static_shell
 *
 * Tiny REPL for the M1..M4 boot-test ramdisk. PID1 target: XNU execs
 * /sbin/launchd (xnu/bsd/kern/kern_exec.c:7344, init_programs[]); we ship
 * this binary at that path on the M4 ramdisk. It is NOT launchd-compatible
 * (no XPC bootstrap handshake) — see ../README.md §3 for the caveats and
 * for the kernel-team interface question this poses.
 *
 * Design constraints (deliberate):
 *   * FREESTANDING, statically linked, no lib System: everything below uses
 *     direct XNU syscalls from syscalls.h (svc #0x80, x16 = BSD number).
 *   * Console I/O ONLY via (/dev/console) open/read/write. There is no
 *     bootstrap port at a bare kernel boot, so no Mach IPC (the
 *     mach-nostalgic demo lives in mach_hello.c and explains itself).
 *   * Commands: help/echo/uname/pid/cat/hexdump/writefile/exec/sleep/exit.
 *
 * Console plumbing facts:
 *   * XNU creates /dev/console relative to IOKit console devices; on the
 *     begonia port, output ultimately lands on UART0 @ 0x11002000 (log hook
 *     lives in the loader/xnu serial stack) and the lk framebuffer console.
 *   * If /dev/console open fails (early boot accidence), we fall back to
 *     file descriptors 1/2 handed to us by XNU's manufactured PID 1
 *     (bsd/kern/bsd_init.c wires these for the initial process).
 *
 * Build: see ./build.sh (aarch64-apple-darwin clang -nostdlib + ld64, with a
 * documented fallback). Sign with `ldid -S` if present; with
 * cs_enforcement_disable=1 (docs/hardware_map_mt6785.md §4) unsigned pages
 * boot fine too — AMFI-stub path is the backstop.
 */

#include "syscalls.h"

#define CONSOLE_PATH  "/dev/console"
#define LINE_BUF        512
#define OUT_BUF         1024

static int con_fd = -1;

static void
console_put(const char *s)
{
	size_t n = str_len(s);
	if (n == 0)
		return;
	sys_write(con_fd, s, n);
}

static void
console_put_dec(unsigned long v)
{
	char tmp[24];
	int i = 0;
	if (v == 0) {
		console_put("0");
		return;
	}
	while (v > 0 && i < (int)sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}
	/* digits are in reverse order — emit them back-to-front */
	for (int j = i - 1; j >= 0; j--) {
		char c[2] = { tmp[j], 0 };
		console_put(c);
	}
}

static void
console_put_hex(unsigned long v)
{
	static const char hexd[] = "0123456789abcdef";
	char buf[19];
	int idx = 0;
	buf[idx++] = '0';
	buf[idx++] = 'x';
	for (int shift = 0; shift < 64; shift += 4) {
		unsigned nib = (v >> (60 - shift)) & 0xFu;
		buf[idx++] = hexd[nib];
	}
	buf[idx] = '\0';
	console_put(buf);
}

static void
cmd_help(void)
{
	console_put(
	    "commands:\n"
	    "  help                this list\n"
	    "  echo [args...]      print args followed by newline\n"
	    "  uname               identity banner (begonia masquerade info)\n"
	    "  pid                 getpid()\n"
	    "  cat FILE            print a file\n"
	    "  hexdump FILE [N]    hex dump the first N (default 256) bytes\n"
	    "  writefile F TEXT    small write test (creates/truncates F)\n"
	    "  exec PATH [args]    execve() another static binary from the image\n"
	    "  sleep SECONDS       busy-spin approx SECONDS (no timer syscall)\n"
	    "  exit                terminate PID 1 (kernel panic halt path)\n");
}

static void
cmd_uname(void)
{
	console_put(
	    "HackiOS-begonia M4 ramdisk shell (iPhone10,3 iOS 16.5 20F66 masquerade)\n"
	    "device: Xiaomi Redmi Note 8 Pro (begonia) · MT6785 (Helio G90T) · ARMv8.2-A\n"
	    "kernel: XNU 8796.121.2 port (16KiB pages, GICv3 @ 0x0c000000)\n");
}

static int
readline(char *buf, int buflen)
{
	size_t got = sys_read(con_fd, buf, (size_t)(buflen - 1));
	if (got <= 0)
		return -1;
	buf[got] = '\0';
	/* strip trailing CR/LF immediately */
	long len = (long)got;
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' ||
	    buf[len - 1] == '\0'))
		buf[--len] = '\0';
	return (int)len;
}

static int
split_tokens(char *line, char **tokens, int maxtok)
{
	int n = 0;
	char *p = line;
	while (n < maxtok) {
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0') break;
		tokens[n++] = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		if (*p) *p++ = '\0';
	}
	return n;
}

static void
cmd_cat(const char *path)
{
	char buf[1024];
	int fd = sys_open(path, O_RDONLY, 0);
	if (fd < 0) {
		console_put("cat: open failed (errno in x0): ");
		console_put(path);
		console_put("\n");
		return;
	}
	for (;;) {
		long got = sys_read(fd, buf, sizeof(buf));
		if (got <= 0)
			break;
		sys_write(con_fd, buf, (size_t)got);
	}
	if (str_len(path) && path[0] != '\n')
		console_put("\n");
	sys_close(fd);
}

static void
cmd_hexdump(const char *path, int limit)
{
	unsigned char buf[1024];
	int fd = sys_open(path, O_RDONLY, 0);
	if (fd < 0) {
		console_put("hexdump: open failed: ");
		console_put(path);
		console_put("\n");
		return;
	}
	unsigned long base = 0;
	static const char hexd[] = "0123456789abcdef";
	while (limit > 0) {
		int chunk = (limit < (int)sizeof(buf)) ? limit : (int)sizeof(buf);
		long got = sys_read(fd, buf, (size_t)chunk);
		if (got <= 0)
			break;
		for (long i = 0; i < got; i += 16) {
			console_put_hex(base + (unsigned long)i);
			console_put("  ");
			for (int j = 0; j < 16; j++) {
				int at = (int)i + j;
				unsigned b = (at < got) ? buf[at] : 0;
				char cell[3];
				cell[0] = hexd[b >> 4];
				cell[1] = hexd[b & 0xF];
				cell[2] = ' ';
				sys_write(con_fd, cell, 3);
				if (j == 7)
					sys_write(con_fd, " ", 1);
			}
			console_put(" |");
			for (int j = 0; j < 16 && (i + j) < got; j++) {
				unsigned c = buf[i + j];
				if (c < 32 || c > 126) c = '.';
				char ch[1];
				ch[0] = (char)c;
				sys_write(con_fd, ch, 1);
			}
			console_put("|\n");
		}
		base += (unsigned long)got;
		limit -= (int)got;
	}
	console_put("\n");
	sys_close(fd);
}

static void
cmd_writefile(const char *path, const char *text)
{
	int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		console_put("writefile: open failed: ");
		console_put(path);
		console_put("\n");
		return;
	}
	size_t n = str_len(text);
	sys_write(fd, text, n);
	sys_close(fd);
	console_put("wrote (bytes): ");
	console_put_dec((unsigned long)n);
	console_put("\n");
}

static void
cmd_exec(char **av, int ac)
{
	if (ac < 2) {
		console_put("exec: PATH required\n");
		return;
	}
	char **eargv = av + 1;   /* argv[0] = program path convention */
	int n = ac - 1;
	if (n < 1) {
		console_put("exec: no path\n");
		return;
	}
	console_put("exec: ");
	console_put(eargv[0]);
	console_put(" (this is a real execve; the REPL ride ends here)\n");
	long rc = syscall3(SYS_execve, (long)eargv[0], (long)eargv, 0);
	console_put("execve returned ");
	console_put_dec((unsigned long)rc);
	console_put(" — binary missing/not Mach-O?\n");
}

static void
repl(void)
{
	char line[LINE_BUF];
	char *toks[16];
	console_put("\nHackiOS-begonia static shell — type 'help'\n");
	for (;;) {
		console_put("hackios> ");
		int len = readline(line, sizeof(line));
		if (len <= 0) {
			console_put("\n");
			continue;
		}
		int ac = split_tokens(line, toks, 16);
		if (ac == 0)
			continue;
		const char *cmd = toks[0];
		if (str_eq(cmd, "help")) {
			cmd_help();
		} else if (str_eq(cmd, "echo")) {
			for (int i = 1; i < ac; i++) {
				console_put(toks[i]);
				console_put(i + 1 < ac ? " " : "\n");
			}
			if (ac == 1) console_put("\n");
		} else if (str_eq(cmd, "uname")) {
			cmd_uname();
		} else if (str_eq(cmd, "pid")) {
			console_put("pid: ");
			console_put_dec((unsigned long)sys_getpid());
			console_put("\n");
		} else if (str_eq(cmd, "cat")) {
			if (ac < 2) console_put("usage: cat FILE\n");
			else cmd_cat(toks[1]);
		} else if (str_eq(cmd, "hexdump")) {
			if (ac < 2) console_put("usage: hexdump FILE [N]\n");
			else cmd_hexdump(toks[1], ac > 2 ? 4096 : 256);
		} else if (str_eq(cmd, "writefile")) {
			if (ac < 3) console_put("usage: writefile F TEXT...\n");
			else cmd_writefile(toks[1], toks[2]);
		} else if (str_eq(cmd, "exec")) {
			cmd_exec(toks, ac);
		} else if (str_eq(cmd, "sleep")) {
			console_put("sleep: single-threaded busy-wait not yet wired "
			    "(needs the ARM Generic Timer; TODO loop on cntv ctl)\n");
		} else if (str_eq(cmd, "exit")) {
			console_put("bye (PID 1 exit => XNU panic path see bsd_init)\n");
			sys_exit(0);
		} else {
			console_put("unknown command: ");
			console_put(cmd);
			console_put("\n");
		}
	}
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	con_fd = sys_open(CONSOLE_PATH, O_RDWR, 0);
	if (con_fd < 0) {
		/* fall back to the manufactured stdin/stdout fds (1/2) */
		con_fd = 1;
	}
	console_put("\r\nHACKIOS-BEGONIA M4 static shell booting\r\n");
	cmd_uname();
	repl();
	sys_exit(0);
	return 0;
}
