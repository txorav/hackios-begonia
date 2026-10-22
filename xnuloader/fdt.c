/*
 * fdt.c — minimal Flattened Device Tree parser for xnuloader.
 * Single full-scan pass caches node records (path + struct-block offsets);
 * property fetch re-walks a single node's token range.
 * Big-endian wire format; mediation helpers in be32/be64.
 */
#include "fdt.h"
#include "console.h"
#include <stdint.h>
#include <stddef.h>

static const volatile uint8_t *fdt;
static uint32_t fdt_struct_off;
static uint32_t fdt_struct_size;
static uint32_t fdt_strings_off;

static inline uint32_t
be32(const volatile uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t
be64(const volatile uint8_t *p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

#define FDT_MAX_TRACKED_NODES 96

struct fdt_node_rec {
	char     path[160];
	uint32_t begin_off;    /* offset of FDT_BEGIN_NODE token */
	uint32_t props_off;    /* offset of first token after the name field */
};
static struct fdt_node_rec fdt_nodes[FDT_MAX_TRACKED_NODES];
static int                  fdt_node_count;

/* path stack during scan */
struct path_stack {
	char path[192];
	int  len;
};

static void
ps_push(struct path_stack *ps, const volatile uint8_t *name)
{
	int n = 0;
	while (name[n] != 0 && n < 120) { n++; }
	int rem = (int)sizeof(ps->path) - 1 - ps->len;
	if (n > rem) { return; }
	if (ps->len > 0) { ps->path[ps->len++] = '/'; }
	for (int i = 0; i < n; i++) { ps->path[ps->len + i] = (char)name[i]; }
	ps->len += n;
	ps->path[ps->len] = 0;
}

static void
ps_pop(struct path_stack *ps)
{
	while (ps->len > 0 && ps->path[ps->len - 1] != '/') { ps->len--; }
	if (ps->len > 0 && ps->path[ps->len - 1] == '/') { ps->len--; }
	ps->path[ps->len] = 0;
}

static int
path_prefix_match(const char *path, const char *match)
{
	int i = 0;
	while (match[i]) {
		if (path[i] != match[i]) { return 0; }
		i++;
	}
	/* exact, or prefix followed by unit-address '@' or a child '/' */
	return path[i] == 0 || path[i] == '@' || path[i] == '/';
}

int
fdt_init(uint64_t base)
{
	fdt = (const volatile uint8_t *)base;
	fdt_node_count = 0;
	if (base == 0 || fdt == 0) {
		return FDT_E_BADMAG;
	}
	if (be32(fdt) != FDT_MAGIC) {
		return FDT_E_BADMAG;
	}
	uint32_t total = be32(fdt + 4);
	if (total == 0 || total > 0x400000) {
		return FDT_E_TRUNC;
	}
	fdt_struct_off  = be32(fdt + 8);
	fdt_strings_off = be32(fdt + 12);
	fdt_struct_size = be32(fdt + 36);
	if (fdt_struct_off >= total || fdt_struct_off == 0) {
		return FDT_E_FMT;
	}
	if (fdt_struct_size == 0 || fdt_struct_off + fdt_struct_size > total) {
		fdt_struct_size = total - fdt_struct_off;
	}
	if (fdt_strings_off >= total) {
		return FDT_E_FMT;
	}

	/* single pass: track every node (bounded count), build the cache */
	struct path_stack ps;
	ps.len = 0;
	ps.path[0] = 0;

	uint32_t off = 0;
	const uint32_t end = fdt_struct_size;
	while (off < end) {
		uint32_t tok = be32(fdt + fdt_struct_off + off);
		off += 4;
		switch (tok) {
		case FDT_NOP:
			break;
		case FDT_BEGIN_NODE: {
			uint32_t begin_off  = off - 4;
			const volatile uint8_t *namep = fdt + fdt_struct_off + off;
			uint32_t nlen = 0;
			while (namep[nlen] != 0 && nlen < 120) { nlen++; }
			if (nlen > 0) {
				ps_push(&ps, namep);
			}
			off += (nlen + 4) & ~3u;
			if (fdt_node_count < FDT_MAX_TRACKED_NODES) {
				struct fdt_node_rec *r = &fdt_nodes[fdt_node_count];
				int i = 0;
				for (; ps.path[i]; i++) { r->path[i] = ps.path[i]; }
				r->path[i] = 0;
				r->begin_off = begin_off;
				r->props_off = off;
				fdt_node_count++;
			}
			break;
		}
		case FDT_END_NODE:
			ps_pop(&ps);
			break;
		case FDT_PROP: {
			uint32_t len = be32(fdt + fdt_struct_off + off);
			off += 8 + len;                      /* len + nameoff + value */
			off = (off + 3) & ~3u;
			break;
		}
		case FDT_END:
			off = end;
			break;
		default:
			return FDT_E_FMT;
		}
	}
	return FDT_E_OK;
}

static int
node_find_exact(const char *path, int *node)
{
	for (int k = 0; k < fdt_node_count; k++) {
		if (fdt_node_eq(fdt_nodes[k].path, path)) {
			*node = k;
			return FDT_E_OK;
		}
	}
	return FDT_E_NOTFOUND;
}

int
fdt_node_eq(const char *a, const char *b)
{
	while (*a && *b && *a == *b) { a++; b++; }
	return *a == 0 && *b == 0;
}

int
fdt_find(const char *path, int *node)
{
	const char *p = path;
	while (*p == '/') { p++; }
	return node_find_exact(p, node);
}

int
fdt_find_prefix(const char *prefix, int start_from, int *node)
{
	for (int k = (start_from <= 0 ? 0 : start_from + 1); k < fdt_node_count; k++) {
		if (path_prefix_match(fdt_nodes[k].path, prefix)) {
			*node = k;
			return FDT_E_OK;
		}
	}
	return FDT_E_NOTFOUND;
}

int
fdt_get_prop(int node, const char *name, const uint8_t **val, int *len)
{
	if (node < 0 || node >= fdt_node_count) {
		return FDT_E_NOTFOUND;
	}
	uint32_t off = fdt_nodes[node].props_off;
	while (off + 4 <= fdt_struct_size) {
		uint32_t tok = be32(fdt + fdt_struct_off + off);
		off += 4;
		if (tok == FDT_PROP) {
			uint32_t plen     = be32(fdt + fdt_struct_off + off);
			uint32_t name_off = be32(fdt + fdt_struct_off + off + 4);
			const volatile uint8_t *s = fdt + fdt_strings_off + name_off;
			int eq = 1;
			for (int i = 0; name[i]; i++) {
				if (name[i] != (char)s[i]) { eq = 0; break; }
			}
			if (eq) {
				*val = (const uint8_t *)(fdt + fdt_struct_off + off + 8);
				*len = (int)plen;
				return FDT_E_OK;
			}
			off += 8;
			off += (plen + 3) & ~3u;
		} else if (tok == FDT_END_NODE || tok == FDT_END) {
			return FDT_E_NOTFOUND;
		} else if (tok == FDT_NOP) {
			continue;
		} else {
			return FDT_E_FMT;
		}
	}
	return FDT_E_NOTFOUND;
}

int
fdt_prop_cells_u32(int node, const char *name, uint32_t *out, int max)
{
	const uint8_t *v;
	int l;
	if (fdt_get_prop(node, name, &v, &l) != FDT_E_OK) {
		return -1;
	}
	int cells = l / 4;
	if (cells > max) { cells = max; }
	for (int i = 0; i < cells; i++) {
		const volatile uint8_t *c = v + i * 4;
		out[i] = ((uint32_t)c[0] << 24) | ((uint32_t)c[1] << 16) |
			 ((uint32_t)c[2] << 8) | (uint32_t)c[3];
	}
	return cells;
}

int
fdt_prop_u64(int node, const char *name, uint64_t *out)
{
	uint32_t c[4];
	if (fdt_prop_cells_u32(node, name, c, 4) < 2) {
		return -1;
	}
	*out = ((uint64_t)c[0] << 32) | c[1];
	return 0;
}

/*
 * DRAM size: sum of "reg" sizes over every memory node
 * (uses the node's #address-cells / #size-cells, defaults 2/2).
 */
uint64_t
fdt_mem_size(void)
{
	uint64_t total = 0;
	int k = -1;
	while (fdt_find_prefix("memory", k, &k) == FDT_E_OK) {
		uint32_t ac = 2, sc = 2;
		fdt_prop_cells_u32(k, "#address-cells", &ac, 1);
		fdt_prop_cells_u32(k, "#size-cells", &sc, 1);
		if (ac == 0) { ac = 2; }
		if (sc == 0) { sc = 2; }
		const uint8_t *v; int l;
		if (fdt_get_prop(k, "reg", &v, &l) != FDT_E_OK) {
			continue;
		}
		int step = (int)(ac + sc) * 4;
		int off = 0;
		while (off + step <= l) {
			uint64_t sz = 0;
			for (uint32_t i = 0; i < sc; i++) {
				sz = (sz << 32) | (uint64_t)be32(v + off + (int)(ac * 4) + i * 4);
			}
			total += sz;
			off += step;
		}
		if (total == 0 && fdt_node_count > 0) {
			continue;
		}
	}
	return total;
}

int
fdt_reserves(uint64_t out[][2], int max)
{
	uint32_t rsv_off = be32(fdt + 16);
	int n = 0;
	const volatile uint8_t *p = fdt + rsv_off;
	while (n < max) {
		uint64_t base = be64(p);
		uint64_t size = be64(p + 8);
		if (base == 0 && size == 0) {
			break;
		}
		out[n][0] = base;
		out[n][1] = size;
		n++;
		p += 16;
	}
	return n;
}

uint32_t
fdt_totalsize(void)
{
	return be32(fdt + 4);
}
