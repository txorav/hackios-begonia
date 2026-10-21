/*
 * adt.c — Apple DeviceTree (ADT) blob writer + begonia template builder.
 *
 * Blob format (matches xnu/pexpert/gen/device_tree.c SecureDT reader):
 *   DeviceTreeNode         { u32 nProperties; u32 nChildren; }
 *   DeviceTreeNodeProperty { char name[32]; u32 length; value; pad-to-4 }
 * Layout is recursive-inline: node header, its properties, then each child.
 * The FIRST property of every node is "name" (NUL-terminated string).
 *
 * Value encoding:
 *   - numeric cells (reg, #a-cells, chip-id, ...) are stored BIG-endian
 *     (OpenFirmware tradition; XNU consumers decode BE 32-bit cells),
 *   - /chosen/memory-map "reg" entries are DTMemoryMapRange pairs:
 *     { uint64 paddr; uint64 length; } LITTLE-endian (vm_offset_t/size_t),
 *   - strings are NUL-terminated char arrays.
 */
#include "adt.h"
#include "console.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PROP_VAL_MAX 256

struct adt_spec_prop {
	char     name[32];
	uint32_t len;
	uint8_t  val[PROP_VAL_MAX];
	const void *ext_val;     /* non-NULL: serialization reads here instead */
	uint32_t ext_len;
};

struct adt_spec_node {
	char     name[32];
	uint32_t size;           /* serialized size */
	struct adt_spec_prop props[ADT_MAX_PROPS];
	int      nprops;
	int      children[ADT_MAX_CHILDREN];
	int      nchildren;
};

static struct adt_spec_node g_nodes[ADT_MAX_NODES];
static int                  g_nnodes;

/* ---------------- primitives ---------------- */

static int
node_new(const char *name, int parent)
{
	if (g_nnodes >= ADT_MAX_NODES) {
		xldr_panic("adt: node pool exhausted");
	}
	int id = g_nnodes++;
	struct adt_spec_node *n = &g_nodes[id];
	int i = 0;
	n->nprops = 0;
	n->nchildren = 0;
	n->size = 0;
	for (; name[i] && i < (int)sizeof(n->name) - 1; i++) {
		n->name[i] = name[i];
	}
	n->name[i] = 0;
	if (parent >= 0) {
		struct adt_spec_node *p = &g_nodes[parent];
		if (p->nchildren >= ADT_MAX_CHILDREN) {
			xldr_panic("adt: children overflow");
		}
		p->children[p->nchildren++] = id;
	}
	return id;
}

static void
prop_put(struct adt_spec_node *n, const char *name, const void *v, uint32_t len)
{
	if (n->nprops >= ADT_MAX_PROPS || len > PROP_VAL_MAX) {
		xldr_panic("adt: property overflow");
	}
	struct adt_spec_prop *p = &n->props[n->nprops++];
	int i = 0;
	for (; name[i] && i < (int)sizeof(p->name) - 1; i++) {
		p->name[i] = name[i];
	}
	p->name[i] = 0;
	p->len = len;
	p->ext_val = 0;
	p->ext_len = 0;
	memset(p->val, 0, sizeof(p->val));
	if (v && len) {
		memcpy(p->val, v, len);
	}
}

static void
prop_ext(struct adt_spec_node *n, const char *name, const void *v, uint32_t len)
{
	if (n->nprops >= ADT_MAX_PROPS) {
		xldr_panic("adt: property overflow");
	}
	struct adt_spec_prop *p = &n->props[n->nprops++];
	int i = 0;
	for (; name[i] && i < (int)sizeof(p->name) - 1; i++) {
		p->name[i] = name[i];
	}
	p->name[i] = 0;
	p->len = 0;
	p->ext_val = v;
	p->ext_len = len;
}

static void
prop_str(struct adt_spec_node *n, const char *name, const char *s)
{
	prop_put(n, name, s, (uint32_t)strlen(s) + 1);
}

static void
prop_str2(struct adt_spec_node *n, const char *name, const char *a, const char *b)
{
	uint8_t buf[128];
	uint32_t la = (uint32_t)strlen(a) + 1;
	uint32_t lb = (uint32_t)strlen(b) + 1;
	if (la + lb > sizeof(buf)) {
		xldr_panic("adt: str2 overflow");
	}
	memcpy(buf, a, la);
	memcpy(buf + la, b, lb);
	prop_put(n, name, buf, la + lb);
}

static void
prop_u32be(struct adt_spec_node *n, const char *name, uint32_t v)
{
	uint8_t b[4];
	b[0] = (uint8_t)(v >> 24);
	b[1] = (uint8_t)(v >> 16);
	b[2] = (uint8_t)(v >> 8);
	b[3] = (uint8_t)v;
	prop_put(n, name, b, 4);
}

/* reg = <addr size> — one address cell + one size cell, big-endian */
static void
prop_reg(struct adt_spec_node *n, uint32_t addr, uint32_t size)
{
	uint8_t b[8];
	b[0] = (uint8_t)(addr >> 24);
	b[1] = (uint8_t)(addr >> 16);
	b[2] = (uint8_t)(addr >> 8);
	b[3] = (uint8_t)addr;
	b[4] = (uint8_t)(size >> 24);
	b[5] = (uint8_t)(size >> 16);
	b[6] = (uint8_t)(size >> 8);
	b[7] = (uint8_t)size;
	prop_put(n, "reg", b, 8);
}

/* /chosen/memory-map "reg": {u64 paddr; u64 length;} little-endian */
static void
prop_memrange(struct adt_spec_node *n, uint64_t addr, uint64_t size)
{
	uint8_t b[16];
	for (int i = 0; i < 8; i++) {
		b[i]     = (uint8_t)(addr >> (8 * i));
		b[8 + i] = (uint8_t)(size >> (8 * i));
	}
	prop_put(n, "reg", b, 16);
}

/* ---------------- measure + serialize ---------------- */

static void serialize_node(int id, uint8_t **out);

static uint32_t
prop_size(const struct adt_spec_prop *p)
{
	uint32_t len = p->ext_val ? p->ext_len : p->len;
	return 32 + 4 + ((len + 3) & ~3u);
}

static uint32_t
measure_node(int id)
{
	struct adt_spec_node *n = &g_nodes[id];
	uint32_t sz = 8;
	for (int i = 0; i < n->nprops; i++) {
		sz += prop_size(&n->props[i]);
	}
	for (int i = 0; i < n->nchildren; i++) {
		sz += measure_node(n->children[i]);
	}
	n->size = sz;
	return sz;
}

int
adt_size(void)
{
	return (int)measure_node(0);
}

void
adt_serialize(uint8_t *dst, int *len_out)
{
	uint8_t *o = dst;

	serialize_node(0, &o);
	*len_out = (int)(measure_node(0));
}

static void
serialize_node(int id, uint8_t **out)
{
	struct adt_spec_node *n = &g_nodes[id];
	uint8_t *o = *out;
	uint8_t *w = o;

	/* node header u32s are NATIVE-endian (xnu/pexpert/gen/device_tree.c
	 * SecureDT reader does plain C struct loads) — LE on ARM64. */
	for (int i = 0; i < 4; i++) {
		*w++ = (uint8_t)((uint32_t)n->nprops >> (8 * i));
	}
	for (int i = 0; i < 4; i++) {
		*w++ = (uint8_t)((uint32_t)n->nchildren >> (8 * i));
	}
	for (int i = 0; i < n->nprops; i++) {
		struct adt_spec_prop *p = &n->props[i];
		memset(w, 0, 32);
		memcpy(w, p->name, strlen(p->name));
		w += 32;
		uint32_t len  = p->ext_val ? p->ext_len : p->len;
		const void *v = p->ext_val ? p->ext_val : (const void *)p->val;
		w[0] = (uint8_t)len;
		w[1] = (uint8_t)(len >> 8);
		w[2] = (uint8_t)(len >> 16);
		w[3] = (uint8_t)(len >> 24);
		w += 4;
		uint32_t padded = (len + 3) & ~3u;
		memset(w, 0, padded);
		if (len) {
			memcpy(w, v, len);
		}
		w += padded;
	}
	for (int i = 0; i < n->nchildren; i++) {
		serialize_node(n->children[i], &w);
	}
	*out = w;
}

/* ---------------- the begonia template ---------------- */

/*
 * /chosen/memory-map "DeviceTree" length: reserve a fixed 64 KiB window for
 * the serialized ADT (matches iBoot sizing; the blob itself is far smaller).
 */
#define ADT_MM_DT_SIZE  0x10000ul

int
adt_build(const struct adt_overrides *o)
{
	g_nnodes = 0;

	/* / */
	int root = node_new("/", -1);
	{
		struct adt_spec_node *n = &g_nodes[root];
		/* xnu's PE_init_platform looks the root up by name=="device-tree" */
		prop_str(n, "name", "device-tree");
		prop_str2(n, "compatible", "Xiaomi,begonia", "mtk,mt6785");
		prop_str(n, "model", "Redmi Note 8 Pro");
		prop_str(n, "target-type", "MT6785");
		prop_u32be(n, "#address-cells", 1);
		prop_u32be(n, "#size-cells", 1);
		prop_u32be(n, "revision", 5);
	}

	/* /memory */
	int mem = node_new("memory", root);
	{
		struct adt_spec_node *n = &g_nodes[mem];
		prop_str(n, "name", "memory");
		prop_u32be(n, "#address-cells", 2);
		prop_u32be(n, "#size-cells", 2);
		uint8_t b[16];
		uint32_t base_hi = 0;
		uint32_t base_lo = (uint32_t)o->phys_base;
		uint32_t size_hi = (uint32_t)(o->phys_size >> 32);
		uint32_t size_lo = (uint32_t)o->phys_size;
		b[0] = (uint8_t)(base_hi >> 24);
		b[1] = (uint8_t)(base_hi >> 16);
		b[2] = (uint8_t)(base_hi >> 8);
		b[3] = (uint8_t)base_hi;
		b[4] = (uint8_t)(base_lo >> 24);
		b[5] = (uint8_t)(base_lo >> 16);
		b[6] = (uint8_t)(base_lo >> 8);
		b[7] = (uint8_t)base_lo;
		b[8] = (uint8_t)(size_hi >> 24);
		b[9] = (uint8_t)(size_hi >> 16);
		b[10] = (uint8_t)(size_hi >> 8);
		b[11] = (uint8_t)size_hi;
		b[12] = (uint8_t)(size_lo >> 24);
		b[13] = (uint8_t)(size_lo >> 16);
		b[14] = (uint8_t)(size_lo >> 8);
		b[15] = (uint8_t)size_lo;
		prop_put(n, "reg", b, 16);
	}

	/* /product */
	int prod = node_new("product", root);
	{
		struct adt_spec_node *n = &g_nodes[prod];
		prop_str(n, "name", "product");
		prop_str(n, "product-name", "iPhone10,3");
		prop_str(n, "product-type", "iPhone10,3");
		prop_str(n, "product-version", "16.5");
		prop_str(n, "board-config", "D22AP");
		prop_str(n, "hardware-model", "D22AP");
		prop_str(n, "serial-number", "FHHAC0HACK00");
		prop_str(n, "MLBSerialNumber", "C0XHACKIOS000");
	}

	/* /chosen */
	int chosen = node_new("chosen", root);
	int chosen_id = chosen;
	{
		struct adt_spec_node *n = &g_nodes[chosen];
		prop_str(n, "name", "chosen");
		prop_ext(n, "bootargs", o->cmdline,
		    o->cmdline ? (uint32_t)strlen(o->cmdline) + 1 : 0);
		if (o->randseed) {
			prop_put(n, "randseed", o->randseed, 16);
		}
		prop_u32be(n, "chip-id", 0x8020u);         /* A11-class for MobileGestalt */
		prop_u32be(n, "chip-revision", 1);
		prop_str(n, "board-id", "D22AP");
		prop_u32be(n, "ramoops-phys", 0x47df0000u);
		prop_u32be(n, "ramoops-size", 0x10000u);
	}

	/* /chosen/memory-map */
	int mmaproot = node_new("memory-map", chosen_id);
	{
		prop_str(&g_nodes[mmaproot], "name", "memory-map");
	}
	int mm_dt = node_new("DeviceTree", mmaproot);
	{
		struct adt_spec_node *n = &g_nodes[mm_dt];
		prop_str(n, "name", "DeviceTree");
		prop_memrange(n, o->mm_dt_addr, ADT_MM_DT_SIZE);
	}
	int mm_rd = node_new("RAMDisk", mmaproot);
	{
		struct adt_spec_node *n = &g_nodes[mm_rd];
		prop_str(n, "name", "RAMDisk");
		prop_memrange(n, o->mm_rd_addr, o->mm_rd_size);
	}
	int mm_fb = node_new("FrameBuffer", mmaproot);
	{
		struct adt_spec_node *n = &g_nodes[mm_fb];
		prop_str(n, "name", "FrameBuffer");
		prop_memrange(n, o->fb_base, o->fb_size);
	}
	int mm_panic = node_new("PanicRings", mmaproot);
	{
		struct adt_spec_node *n = &g_nodes[mm_panic];
		prop_str(n, "name", "PanicRings");
		prop_memrange(n, 0x47df0000ull, 0x10000ull);
	}

	/* /cpus + cpu0..cpu7 */
	int cpus = node_new("cpus", root);
	{
		struct adt_spec_node *n = &g_nodes[cpus];
		prop_str(n, "name", "cpus");
		prop_u32be(n, "#address-cells", 1);
		prop_u32be(n, "#size-cells", 0);
	}
	static const uint32_t cpu_mpidr[8] = { 0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x100, 0x101 };
	static const uint32_t cpu_freq[8]  = { 2000000000u, 2000000000u, 2000000000u, 2000000000u,
					       2000000000u, 2000000000u, 2050000000u, 2050000000u };
	for (int i = 0; i < 8; i++) {
		char cn[8];
		cn[0] = 'c'; cn[1] = 'p'; cn[2] = 'u'; cn[3] = (char)('0' + i);
		cn[4] = 0;
		int c = node_new(cn, cpus);
		struct adt_spec_node *n = &g_nodes[c];
		prop_str(n, "name", cn);
		prop_str(n, "device-type", "cpu");
		prop_u32be(n, "reg", cpu_mpidr[i]);
		if (i < 6) {
			prop_str(n, "compatible", "arm,cortex-a55");
		} else {
			prop_str(n, "compatible", "arm,cortex-a76");
		}
		prop_u32be(n, "clock-frequency", cpu_freq[i]);
		prop_u32be(n, "cpu-id", (uint32_t)i);
		prop_u32be(n, "cluster", (i < 6) ? 0u : 1u);
		prop_str(n, "state", (i == 0) ? "running" : "on");
	}

	/* /arm-io + peripherals */
	int armio = node_new("arm-io", root);
	{
		struct adt_spec_node *n = &g_nodes[armio];
		prop_str(n, "name", "arm-io");
		prop_u32be(n, "#address-cells", 1);
		prop_u32be(n, "#size-cells", 1);
	}
	int gic = node_new("gic", armio);
	{
		struct adt_spec_node *n = &g_nodes[gic];
		prop_str(n, "name", "gic");
		prop_reg(n, 0x0c000000u, 0x40000u);
		prop_reg(n, 0x0c040000u, 0xC0000u);        /* redistributor */
		prop_str(n, "interrupt-type", "gicv3");
	}
	int uartn = node_new("uart0", armio);
	{
		struct adt_spec_node *n = &g_nodes[uartn];
		prop_str(n, "name", "uart0");
		prop_reg(n, 0x11002000u, 0x1000u);
		prop_u32be(n, "clock-frequency", 26000000u);
		prop_u32be(n, "interrupt", 0x73u);         /* SPI 115 (0x73) from begonia DTS */
	}
	int msdc = node_new("msdc1", armio);
	{
		struct adt_spec_node *n = &g_nodes[msdc];
		prop_str(n, "name", "msdc1");
		prop_reg(n, 0x11240000u, 0x10000u);
		prop_u32be(n, "clock-frequency", 200000000u);
		prop_u32be(n, "interrupt", 0x66u);         /* SPI 102 (0x66) from begonia DTS */
	}
	int ufsn = node_new("ufs0", armio);
	{
		struct adt_spec_node *n = &g_nodes[ufsn];
		prop_str(n, "name", "ufs0");
		prop_reg(n, 0x11270000u, 0x100000u);
		prop_u32be(n, "interrupt", 0x68u);         /* SPI 104 (0x68) from begonia DTS */
	}
	int i2c0 = node_new("i2c0", armio);
	{
		struct adt_spec_node *n = &g_nodes[i2c0];
		prop_str(n, "name", "i2c0");
		prop_reg(n, 0x11007000u, 0x1000u);
	}
	int ts = node_new("touchscreen", armio);
	{
		struct adt_spec_node *n = &g_nodes[ts];
		prop_str(n, "name", "touchscreen");
		prop_u32be(n, "i2c-address", 0x38);
		prop_str(n, "compatible", "focaltech,fts");
		prop_u32be(n, "interrupt", 0);
	}
	int fbnode = node_new("disp-fb", armio);
	{
		struct adt_spec_node *n = &g_nodes[fbnode];
		prop_str(n, "name", "disp-fb");
		prop_reg(n, (uint32_t)o->fb_base, (uint32_t)o->fb_size);
		prop_u32be(n, "width", o->fb_width);
		prop_u32be(n, "height", o->fb_height);
		prop_u32be(n, "stride", o->fb_stride);
		prop_u32be(n, "depth", o->fb_depth);
		prop_str(n, "format", "RGBX8888");
	}

	measure_node(0);   /* set sizes for the first measure */
	return 0;
}

void
adt_finalize_size(void)
{
	measure_node(0);
}
