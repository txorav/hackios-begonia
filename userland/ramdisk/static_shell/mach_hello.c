/*
 * mach_hello.c — HackiOS-begonia userland :: ramdisk/static_shell
 *
 * "WHY NOT MACH?" documentation-as-code. This binary demonstrates the
 * Mach IPC situation on a bare XNU boot and doubles as a future template;
 * it is NOT linked into the M4 ramdisk shell by default.
 *
 * The trap ABI (all numbers verified in-tree, xnu-8796.121.2):
 *   - BSD syscall:  svc #0x80 with the number in x16  (e.g. write = 4)
 *   - Mach traps:   svc #0x80 with a NEGATIVE number in x16
 *                   — mach_msg_trap is  -31
 *                     xnu/osfmk/mach/syscall_sw.h:108
 *                       kernel_trap(mach_msg_trap, -31, 7)
 *
 * The catch that makes this a "can't ship it yet" demo:
 *   A mach_msg() needs a valid MACH port NAME to send/receive on. Ports are
 *   granted by the kernel at process creation only for well-known services
 *   (task_self/host_self etc.); process-to-process message routing on real
 *   launchd-based iOS goes through the **bootstrap port**, which launchd
 *   publishes. At a bare XNU boot (M4), launchd/Average is not running —
 *   XNU manufactured PID 1 with plain fds and no ports, and there is no
 *   bootstrap service to ask.
 *
 * So: this program (a) prints the explanation, (b) performs ONE literal
 * mach_msg send attempt against a null port so operators can see exactly
 * what the kernel does with it (expected: MACH_SEND_INVALID_DEST),
 * (c) seeds the TODO for the M5+ era: once kernel-port's launchd handover
 * works (plan §Phase 5 M5), call bootstrap_check_in against the published
 * port and this same trap call becomes a live client.
 *
 * Build (only if you want the experiment on-device):
 *   clang —target aarch64-apple-darwin -nostdlib -static
 *     mach_hello.c -o mach_hello  (+ ldid -S if available)
 *   deploy inside the ramdisk (see ../README.md §3) and run via the REPL:
 *     > exec /bin/mach_hello
 */

#include <stdint.h>

typedef unsigned long mach_vm_size_t;
typedef long mach_msg_return_t;

/* minimal mach_msg shapes we need (decoded from osfmk/mach/message.h on
 * purpose — do NOT include Apple SDK headers; freestanding only) */
struct mach_msg_header_t {
	uint32_t bits;          /* msgh_size | msgh_descriptor_count<<9 ... */
	uint32_t size;
	uint32_t remote_port;
	uint32_t reply_port;
	uint32_t voucher;       /* iOS 13+: msgh_voucher_port */
	uint32_t id;
};

struct mach_msg_send_body {
	struct mach_msg_header_t hdr;
	uint32_t trailer_info[4];   /* minimal trailer placeholder */
};

#define MACH_SEND_MSG          0x00000001
#define MACH_SEND_INVALID_DEST 0x10000003
#define MACH_MSG_TYPE_COPY_SEND 19
#define MACH_PORT_NULL         0

static inline long
mach_trap(long number, long a, long b, long c, long d, long e, long f, long g)
{
	register long x8 __asm__("x16") = number;   /* negative = mach trap */
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	register long x6 __asm__("x6") = g;
	__asm__ volatile("svc #0x80"
	    : "+r"(x0)
	    : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x6), "r"(x8)
	    : "cc", "memory");
	return x0;
}

/* tiny BSD syscall for console feedback (write = 4 per syscalls.master) */
static long
sys_write(long fd, const void *buf, unsigned long n)
{
	register long x8 __asm__("x16") = 4;
	register long x0 __asm__("x0") = fd;
	register long x1 __asm__("x1") = (long)buf;
	register long x2 __asm__("x2") = (long)n;
	__asm__ volatile("svc #0x80" : "+r"(x0)
	    : "r"(x1), "r"(x2), "r"(x8) : "cc", "memory");
	return x0;
}

static void
puts_(const char *s)
{
	unsigned long n = 0;
	while (s[n]) n++;
	if (n) sys_write(1, s, n);
}

static void
puthex(unsigned long v)
{
	static const char hx[] = "0123456789abcdef";
	char buf[19];
	int idx = 0;
	buf[idx++] = '0';
	buf[idx++] = 'x';
	for (int shift = 0; shift < 64; shift += 4)
		buf[idx++] = hx[(v >> (60 - shift)) & 0xFu];
	buf[idx] = 0;
	sys_write(1, buf, (unsigned long)idx);
}

int
mach_demo(void)
{
	struct mach_msg_send_body body;

	/* (a) explain */
	puts_("\nMACH HELLO — the IPC situation on a bare XNU boot:\n");
	puts_("  mach_msg_trap = -31 (svc #0x80, x16)   [xnu/osfmk/mach/syscall_sw.h:108]\n");
	puts_("  this process was manufactured by XNU WITHOUT any port grants:\n");
	puts_("  no task_self, no host_self, NO BOOTSTRAP (launchd not running yet)\n");
	puts_("  attempting ONE send anyway, dest = MACH_PORT_NULL...\n");

	/* (b) literal send attempt */
	body.hdr.bits = MACH_SEND_MSG;
	body.hdr.size = (uint32_t)sizeof(body);
	body.hdr.remote_port = MACH_PORT_NULL;
	body.hdr.reply_port = MACH_PORT_NULL;
	body.hdr.voucher = MACH_PORT_NULL;
	body.hdr.id = 0x4841434B;    /* "HACK" */
	mach_msg_return_t rc = mach_trap(-31, (long)&body, body.hdr.size,
	    0, MACH_SEND_MSG, 0, MACH_PORT_NULL, 0);
	puts_("  mach_msg returned ");
	puthex((unsigned long)rc);
	if (rc == MACH_SEND_INVALID_DEST) {
		puts_ (" == MACH_SEND_INVALID_DEST — exactly as expected\n");
	}
	else {
		puts_("\n");
	}
	return (int)rc;
}

int
main(void)
{
	(void)mach_demo();
	puts_("\nTODO (M5+ era): once kernel-port launchd publishes the bootstrap\n"
	    "port, run task_self/bootstrap_check_in from here and the SAME trap\n"
	    "call becomes a live send — see userland/IDEAS_OR_TODO.md Phase 5.4.\n");
	return 0;
}
