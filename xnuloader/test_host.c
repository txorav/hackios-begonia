/*
 * test_host.c — host-compiled unit tests for the xnuloader pure logic:
 *   1. fdt.c: build a synthetic FDT, verify find/props/mem_size
 *   2. adt.c: verify ADT blob layout matches the SecureDT reader contract
 *      (xnu/pexpert/gen/device_tree.c): inline properties, NATIVE-endian
 *      u32 header/length fields, first prop "name",
 *      /chosen/memory-map reg entries as 16-byte LE {paddr, length} pairs.
 *
 * Build & run (system headers — do NOT add -Iinclude; the freestanding
 * include dir would shadow glibc headers and break stdlib):
 *   gcc -std=gnu11 -O1 -o /tmp/xldr_test test_host.c \
 *       adt.c fdt.c string.c && /tmp/xldr_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include "fdt.h"
#include "adt.h"
#include "console.h"

void xldr_panic(const char *why) { fprintf(stderr, "PANIC %s\n", why); exit(1); }
void uart_putc(char c) { (void)c; }
void uart_puts(const char *s) { (void)s; }
void uart_puthex(uint64_t v) { (void)v; }
void fb_init(uint64_t b, uint32_t w, uint32_t h, uint32_t s) { (void)b;(void)w;(void)h;(void)s; }
void fb_text(const char *x, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)x;(void)a;(void)b;(void)c;(void)d; }

/* ---------------- synthetic FDT ---------------- */

static uint8_t fdtbuf[4096];

static void
put32be(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void
build_fdt(void)
{
	memset(fdtbuf, 0, sizeof(fdtbuf));

	const uint32_t struct_off = 0x40;
	const uint32_t str_off    = 0x300;   /* well past the struct content */

	/* ---- string table ---- */
	const char *strings[] = { "reg", "#address-cells", "#size-cells", "name" };
	uint32_t off_str[4];
	uint32_t so = 0;
	for (int i = 0; i < 4; i++) {
		off_str[i] = so;
		memcpy(fdtbuf + str_off + so, strings[i], strlen(strings[i]) + 1);
		so += (uint32_t)strlen(strings[i]) + 1;
	}

	/* ---- structure block ---- */
	uint8_t  *sp = fdtbuf + struct_off;
	uint32_t *w  = (uint32_t *)(void *)sp;

#define PUTW(x)   do { put32be((uint8_t *)w, (uint32_t)(x)); w++; } while (0)
#define PUTVAL(b, nbytes) do { memcpy(w, (b), (nbytes)); \
		w = (uint32_t *)(((uintptr_t)w + ((nbytes) + 3u)) & ~(uintptr_t)3u); \
	} while (0)

	/* root: name prop = "device-tree" */
	PUTW(FDT_BEGIN_NODE);
	PUTW(0);                                       /* root name: empty, padded */
	PUTW(FDT_PROP); PUTW(12); PUTW(off_str[3]);    /* name = "device-tree" */
	PUTVAL("device-tree", 12);

	/* /soc */
	PUTW(FDT_BEGIN_NODE);
	PUTVAL("soc", 4);
	PUTW(FDT_PROP); PUTW(0); PUTW(off_str[3]);

	/* /soc/uart0 — reg = <0x11002000 0x100000> */
	PUTW(FDT_BEGIN_NODE);
	PUTVAL("uart0\0\0", 8);
	PUTW(FDT_PROP); PUTW(0); PUTW(off_str[3]);
	PUTW(FDT_PROP); PUTW(8); PUTW(off_str[0]);
	{
		uint8_t reg[8] = { 0x11, 0x00, 0x20, 0x00, 0x00, 0x10, 0x00, 0x00 };
		PUTVAL(reg, 8);
	}
	PUTW(FDT_END_NODE);     /* uart0 */
	PUTW(FDT_END_NODE);     /* soc   */

	/* /memory@40000000 — reg = <0 0x40000000 0 0xC0000000> */
	PUTW(FDT_BEGIN_NODE);
	PUTVAL("memory@40000000\0", 16);
	PUTW(FDT_PROP); PUTW(0); PUTW(off_str[3]);
	PUTW(FDT_PROP); PUTW(4); PUTW(off_str[1]);     /* #address-cells = 2 */
	{
		uint8_t cell[4] = { 0, 0, 0, 2 };
		PUTVAL(cell, 4);
	}
	PUTW(FDT_PROP); PUTW(4); PUTW(off_str[2]);     /* #size-cells = 2 */
	{
		uint8_t cell[4] = { 0, 0, 0, 2 };
		PUTVAL(cell, 4);
	}
	PUTW(FDT_PROP); PUTW(16); PUTW(off_str[0]);    /* reg */
	{
		uint8_t reg[16];
		put32be(reg + 0, 0); put32be(reg + 4, 0x40000000);
		put32be(reg + 8, 0); put32be(reg + 12, 0xC0000000);
		PUTVAL(reg, 16);
	}
	PUTW(FDT_END_NODE);     /* memory */
	PUTW(FDT_END_NODE);     /* root   */
	PUTW(FDT_END);          /* structure block end */

#undef PUTW
#undef PUTVAL

	/* ---- header (dt spec v17 layout) ---- */
	uint32_t struct_size = (uint32_t)((uintptr_t)w - (uintptr_t)sp);
	uint32_t total = (str_off + so + 15u) & ~15u;
	put32be(fdtbuf + 0,  FDT_MAGIC);
	put32be(fdtbuf + 4,  total);
	put32be(fdtbuf + 8,  0x28);                    /* rsvmap: single zero entry */
	put32be(fdtbuf + 12, struct_off);
	put32be(fdtbuf + 16, str_off);
	put32be(fdtbuf + 20, 17);                      /* last_comp_version */
	put32be(fdtbuf + 24, 0);                       /* boot cpuid */
	put32be(fdtbuf + 28, so);                      /* size_dt_strings */
	put32be(fdtbuf + 32, struct_size);             /* size_dt_struct */
}

/* ---------------- ADT checks ---------------- */

static void
check_adt_layout(uint8_t *dt, int len)
{
	assert(len > 0);
	/* root header: 2 native (LE) u32: nProperties, nChildren */
	uint32_t nprops = dt[0] | (dt[1] << 8) | (dt[2] << 16) | (dt[3] << 24);
	assert(nprops >= 1);
	/* walk: expect first prop name == "name" */
	const uint8_t *q = dt + 8;
	for (uint32_t i = 0; i < nprops; i++) {
		uint32_t plen = q[32] | (q[33] << 8) | (q[34] << 16) | (q[35] << 24);
		if (i == 0) {
			assert(strcmp((const char *)q, "name") == 0);
		}
		q += 32 + 4 + ((plen + 3) & ~3u);
	}
	(void)len;
}

int
main(void)
{
	/* ---- FDT ---- */
	build_fdt();
	int rc = fdt_init((uint64_t)(uintptr_t)fdtbuf);
	assert(rc == FDT_E_OK);

	int n;
	assert(fdt_find("chosen", &n) == FDT_E_NOTFOUND);
	assert(fdt_find("/soc/uart0", &n) == FDT_E_OK);

	const uint8_t *val; int vl;
	assert(fdt_get_prop(n, "reg", &val, &vl) == FDT_E_OK);
	assert(vl == 8);
	assert(val[0] == 0x11 && val[1] == 0x00 && val[2] == 0x20);
	uint32_t cells[4];
	assert(fdt_prop_cells_u32(n, "reg", cells, 4) == 2);
	assert(cells[0] == 0x11002000u && cells[1] == 0x100000u);

	uint64_t mem = fdt_mem_size();
	assert(mem == 0xC0000000ull);
	printf("fdt: mem_size=%.1f GiB, uart reg ok\n", (double)mem / 1e9);

	/* ---- ADT ---- */
	struct adt_overrides ov;
	memset(&ov, 0, sizeof(ov));
	ov.phys_base = 0x40000000;
	ov.phys_size = 0xC0000000;
	ov.fb_base = 0x7f000000;
	ov.fb_size = 2340 * 4320;
	ov.fb_width = 1080;
	ov.fb_height = 2340;
	ov.fb_stride = 4320;
	ov.fb_depth = 32;
	ov.cmdline = "serial=3 -v";
	ov.mm_dt_addr = 0x40240000;
	ov.mm_dt_size = 0x10000;
	ov.mm_rd_addr = 0x40280000;
	ov.mm_rd_size = 12345;
	assert(adt_build(&ov) == 0);

	int sz = adt_size();
	static uint8_t blob[65536];
	adt_serialize(blob, &sz);
	assert(sz > 0 && sz < 65536);
	check_adt_layout(blob, sz);
	printf("adt: serialized %d bytes; first-prop-name ok\n", sz);

	/* verify /chosen/memory-map/RAMDisk reg value is a 16-byte LE pair:
	 * { paddr = 0x40280000, length = 12345 } */
	const uint8_t want[16] = {
		0x00, 0x00, 0x28, 0x40, 0x00, 0x00, 0x00, 0x00,
		0x39, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	int found = 0;
	for (int i = 0; i + 16 <= sz; i++) {
		if (memcmp(blob + i, want, 16) == 0) {
			found = 1;
			break;
		}
	}
	assert(found);   /* memory-map RAMDisk reg landform check */

	printf("ALL xnuloader host tests passed\n");
	return 0;
}
