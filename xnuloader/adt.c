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
 *   - numeric cells (reg, #a-cells, chip-id, ...) are stored BIG-endian or native LE,
 *   - /chosen/memory-map "reg" entries are DTMemoryMapRange pairs:
 *     { uint64 paddr; uint64 length; } LITTLE-endian (vm_offset_t/size_t),
 *   - strings are NUL-terminated char arrays.
 */
#include "adt.h"
#include "identity.h"
#include "console.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PROP_VAL_MAX 64
#define NVRAM_BANK_SIZE 0x2000u

static uint8_t g_nvram_image[NVRAM_BANK_SIZE];
static uint8_t g_random_seed[64];

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
	int      props[ADT_MAX_NODE_PROPS];
	int      nprops;
	int      children[ADT_MAX_CHILDREN];
	int      nchildren;
};

static struct adt_spec_node g_nodes[ADT_MAX_NODES];
static int                  g_nnodes;
static struct adt_spec_prop g_props[ADT_TOTAL_PROPS];
static int                  g_nprops;

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
	if (n->nprops >= ADT_MAX_NODE_PROPS || g_nprops >= ADT_TOTAL_PROPS || len > PROP_VAL_MAX) {
		xldr_panic("adt: property overflow");
	}
	int pid = g_nprops++;
	n->props[n->nprops++] = pid;
	struct adt_spec_prop *p = &g_props[pid];
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
	if (n->nprops >= ADT_MAX_NODE_PROPS || g_nprops >= ADT_TOTAL_PROPS) {
		xldr_panic("adt: property overflow");
	}
	int pid = g_nprops++;
	n->props[n->nprops++] = pid;
	struct adt_spec_prop *p = &g_props[pid];
	int i = 0;
	for (; name[i] && i < (int)sizeof(p->name) - 1; i++) {
		p->name[i] = name[i];
	}
	p->name[i] = 0;
	p->len = len;
	p->ext_val = v;
	p->ext_len = len;
}

static void
prop_str(struct adt_spec_node *n, const char *name, const char *s)
{
	prop_put(n, name, s, (uint32_t)strlen(s) + 1);
}

static void
prop_str2(struct adt_spec_node *n, const char *name, const char *s1, const char *s2)
{
	char b[PROP_VAL_MAX];
	int l1 = (int)strlen(s1) + 1;
	int l2 = (int)strlen(s2) + 1;
	if (l1 + l2 > (int)sizeof(b)) {
		xldr_panic("adt: prop_str2 overflow");
	}
	memcpy(b, s1, l1);
	memcpy(b + l1, s2, l2);
	prop_put(n, name, b, (uint32_t)(l1 + l2));
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

static void
prop_u32(struct adt_spec_node *n, const char *name, uint32_t v)
{
	prop_put(n, name, &v, sizeof(v));
}

static void
prop_u64(struct adt_spec_node *n, const char *name, uint64_t v)
{
	prop_put(n, name, &v, sizeof(v));
}

static void
prop_padded(struct adt_spec_node *n, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];
	memset(field, 0, sizeof(field));
	int len = 0;
	while (value[len] && len < (int)sizeof(field) - 1) {
		field[len] = value[len];
		len++;
	}
	prop_put(n, name, field, size);
}

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

/* /chosen/memory-map properties: {u64 paddr; u64 length;} little-endian */
static void
prop_memrange(struct adt_spec_node *n, const char *name, uint64_t addr, uint64_t size)
{
	uint8_t b[16];
	for (int i = 0; i < 8; i++) {
		b[i]     = (uint8_t)(addr >> (8 * i));
		b[8 + i] = (uint8_t)(size >> (8 * i));
	}
	prop_put(n, name, b, 16);
}

static void
prop_identity(struct adt_spec_node *n, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(props[i].name, "artwork-display-gamut") == 0) {
			prop_put(n, props[i].name, "sRGB", 5);
			continue;
		}
		prop_ext(n, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* ---------------- NVRAM builder ---------------- */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;
	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	int len = 0;
	while (name[len] && len < 12) {
		h[4 + len] = name[len];
		len++;
	}
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;
	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(image + 20, &generation, 4);
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
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
		sz += prop_size(&g_props[n->props[i]]);
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

	for (int i = 0; i < 4; i++) {
		*w++ = (uint8_t)((uint32_t)n->nprops >> (8 * i));
	}
	for (int i = 0; i < 4; i++) {
		*w++ = (uint8_t)((uint32_t)n->nchildren >> (8 * i));
	}
	for (int i = 0; i < n->nprops; i++) {
		struct adt_spec_prop *p = &g_props[n->props[i]];
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

/* ---------------- template builder ---------------- */

int
adt_build(const struct adt_overrides *o)
{
	g_nnodes = 0;
	g_nprops = 0;

	/* / */
	int root = node_new("/", -1);
	{
		struct adt_spec_node *n = &g_nodes[root];
		prop_str(n, "name", "device-tree");
		prop_identity(n, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
		prop_str(n, "platform-name", "t8015");
		prop_padded(n, "serial-number", "RDW00BEGONIA0", 32);
		prop_padded(n, "mlb-serial-number", "RDW00BEGONIAMLB0", 32);
		prop_padded(n, "model-number", "MQAC2", 32);
		prop_padded(n, "region-info", "LL/A", 32);
		prop_padded(n, "regulatory-model-number", "A1865", 32);
		prop_str2(n, "compatible", "Xiaomi,begonia", "mtk,mt6785");
		prop_str(n, "model", "Redmi Note 8 Pro");
		prop_str(n, "target-type", "MT6785");
		prop_u32(n, "#address-cells", 2);
		prop_u32(n, "#size-cells", 2);
		prop_u32be(n, "revision", 5);
	}

	/* /memory */
	int mem = node_new("memory", root);
	{
		struct adt_spec_node *n = &g_nodes[mem];
		prop_str(n, "name", "memory");
		prop_u32(n, "#address-cells", 2);
		prop_u32(n, "#size-cells", 2);
		uint64_t reg[2] = { o->phys_base, o->phys_size };
		prop_put(n, "reg", reg, sizeof(reg));
	}

	/* /chosen */
	int chosen = node_new("chosen", root);
	{
		struct adt_spec_node *n = &g_nodes[chosen];
		prop_str(n, "name", "chosen");
		prop_ext(n, "bootargs", o->cmdline,
		    o->cmdline ? (uint32_t)strlen(o->cmdline) + 1 : 0);
		for (unsigned i = 0; i < sizeof(g_random_seed); i++) {
#ifdef __aarch64__
			uint64_t cnt;
			asm volatile("mrs %0, cntvct_el0" : "=r"(cnt));
			g_random_seed[i] = (uint8_t)(cnt >> (i & 7)) ^ (uint8_t)(i * 37 + 0x5a);
#else
			g_random_seed[i] = (uint8_t)(i * 37 + 0x5a);
#endif
		}
		prop_ext(n, "random-seed", g_random_seed, sizeof(g_random_seed));
		prop_u64(n, "dram-base", o->phys_base);
		prop_u64(n, "dram-size", o->phys_size);
		prop_u32(n, "debug-enabled", 1);
		prop_u32(n, "display-scale", 3);
		prop_u32(n, "display-rotation", 0);
		prop_str(n, "psci-method", o->is_qemu ? "hvc" : "smc");
		prop_u64(n, "unique-chip-id", 0x000052445730001ULL);
		prop_u32(n, "chip-id", 0x8020u);         /* A11-class for MobileGestalt */
		prop_u32(n, "chip-revision", 1);
		prop_str(n, "board-id", "D22AP");
		prop_str(n, "firmware-version", "iBoot-8422.100.650");
		prop_u32(n, "certificate-production-status", 1);
		prop_u32(n, "certificate-security-mode", 1);
		prop_u32(n, "effective-production-status-ap", 1);
		prop_u32(n, "effective-security-mode-ap", 1);
		prop_u32(n, "secure-boot", 1);
		prop_u32(n, "production-cert", 1);
		prop_u32(n, "development-cert", 0);
		prop_u32(n, "embedded-panic-log-size", 0x80000u);
		prop_u32(n, "ramoops-phys", 0x47df0000u);
		prop_u32(n, "ramoops-size", 0x10000u);

		build_nvram_image(g_nvram_image);
		prop_u32(n, "nvram-bank-size", NVRAM_BANK_SIZE);
		prop_u32(n, "nvram-bank-count", 1);
		prop_u32(n, "nvram-current-bank", 0);
		prop_ext(n, "nvram-proxy-data", g_nvram_image, sizeof(g_nvram_image));
	}

	/* /chosen/memory-map */
	int mmaproot = node_new("memory-map", chosen);
	{
		struct adt_spec_node *n = &g_nodes[mmaproot];
		prop_str(n, "name", "memory-map");
		prop_memrange(n, "DeviceTree", o->mm_dt_addr, o->mm_dt_size ? o->mm_dt_size : 0x10000ul);
		if (o->mm_rd_size) {
			prop_memrange(n, "RAMDisk", o->mm_rd_addr, o->mm_rd_size);
		}
		if (o->fb_size) {
			prop_memrange(n, "FrameBuffer", o->fb_base, o->fb_size);
		}
		prop_memrange(n, "PanicRings", 0x47df0000ull, 0x10000ull);
	}

	/* /defaults */
	int def_node = node_new("defaults", root);
	{
		struct adt_spec_node *n = &g_nodes[def_node];
		prop_str(n, "name", "defaults");
		prop_u32(n, "kern.vm_compressor", 2);
		if (o->is_qemu) {
			prop_u32(n, "serial-device", 0x10u);
		}
	}

	/* /options */
	int opt_node = node_new("options", root);
	{
		struct adt_spec_node *n = &g_nodes[opt_node];
		prop_str(n, "name", "options");
	}

	/* /product */
	int prod = node_new("product", root);
	{
		struct adt_spec_node *n = &g_nodes[prod];
		prop_str(n, "name", "product");
		prop_identity(n, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
		prop_str(n, "product-name", "iPhone X");
		prop_str(n, "product-type", "iPhone10,3");
		prop_str(n, "product-version", "16.5");
		prop_str(n, "board-config", "D22AP");
		prop_str(n, "hardware-model", "D22AP");
		prop_str(n, "serial-number", "FHHAC0HACK00");
		prop_str(n, "MLBSerialNumber", "C0XHACKIOS000");
	}
	int haptics = node_new("haptics", prod);
	{
		struct adt_spec_node *n = &g_nodes[haptics];
		prop_str(n, "name", "haptics");
		prop_u32(n, "closed-loop", 1);
		prop_u32(n, "supports-3rd-party-haptics", 1);
	}

	/* /pram */
	int pram_node = node_new("pram", root);
	{
		struct adt_spec_node *n = &g_nodes[pram_node];
		prop_str(n, "name", "pram");
		uint64_t pram_reg[2] = { 0x47df0000ull, 0x10000ull };
		prop_put(n, "reg", pram_reg, sizeof(pram_reg));
	}

	/* /cpus + cpu0..cpu7 */
	int cpus = node_new("cpus", root);
	{
		struct adt_spec_node *n = &g_nodes[cpus];
		prop_str(n, "name", "cpus");
		prop_u32(n, "#address-cells", 1);
		prop_u32(n, "#size-cells", 0);
	}
	static const uint32_t cpu_mpidr[8] = { 0x000, 0x001, 0x002, 0x003, 0x004, 0x005, 0x100, 0x101 };
	static const uint32_t cpu_freq[8]  = { 2000000000u, 2000000000u, 2000000000u, 2000000000u,
					       2000000000u, 2000000000u, 2050000000u, 2050000000u };
	uint64_t timer_freq = o->is_qemu ? 62500000ull : 13000000ull;
	for (int i = 0; i < 8; i++) {
		char cn[8];
		cn[0] = 'c'; cn[1] = 'p'; cn[2] = 'u'; cn[3] = (char)('0' + i);
		cn[4] = 0;
		int c = node_new(cn, cpus);
		struct adt_spec_node *n = &g_nodes[c];
		prop_str(n, "name", cn);
		prop_str(n, "device_type", "cpu");
		prop_u32(n, "reg", cpu_mpidr[i]);
		prop_u32(n, "cpu-id", (uint32_t)i);
		prop_str(n, "state", (i == 0) ? "running" : "waiting");
		prop_u64(n, "timebase-frequency", timer_freq);
		prop_u64(n, "fixed-frequency", timer_freq);
		prop_u64(n, "clock-frequency", cpu_freq[i]);
		if (i < 6) {
			prop_str(n, "compatible", "arm,cortex-a55");
			prop_u32(n, "small-core", 1);
		} else {
			prop_str(n, "compatible", "arm,cortex-a76");
		}
		prop_u32be(n, "cluster", (i < 6) ? 0u : 1u);
	}

	/* /arm-io + peripherals */
	int armio = node_new("arm-io", root);
	uint64_t io_base = o->is_qemu ? 0x08000000ull : 0x0C000000ull;
	uint64_t io_size = o->is_qemu ? 0x08000000ull : 0x10000000ull;
	{
		struct adt_spec_node *n = &g_nodes[armio];
		prop_str(n, "name", "arm-io");
		prop_str(n, "device_type", o->is_qemu ? "qemu-virt" : "begonia");
		uint64_t ranges[3] = { 0, io_base, io_size };
		prop_put(n, "ranges", ranges, sizeof(ranges));
		prop_u32(n, "#address-cells", 2);
		prop_u32(n, "#size-cells", 2);
	}
	int gic = node_new("gic", armio);
	{
		struct adt_spec_node *n = &g_nodes[gic];
		prop_str(n, "name", "gic");
		prop_str(n, "device_type", "interrupt-controller");
		prop_str(n, "compatible", "arm,gic-v3");
		uint64_t gic_dist = o->is_qemu ? 0x08000000ull : 0x0c000000ull;
		uint64_t gic_redist = o->is_qemu ? 0x080a0000ull : 0x0c040000ull;
		uint64_t gic_reg[4] = { gic_dist - io_base, 0x10000ull, gic_redist - io_base, 0x100000ull };
		prop_put(n, "reg", gic_reg, sizeof(gic_reg));
		prop_str(n, "interrupt-type", "gicv3");
	}
	int uartn = node_new("uart0", armio);
	{
		struct adt_spec_node *n = &g_nodes[uartn];
		prop_str(n, "name", "uart0");
		prop_str(n, "device_type", "uart");
		prop_str(n, "compatible", o->is_qemu ? "arm,pl011" : "mediatek,mt6785-uart");
		uint64_t uart_addr = o->is_qemu ? 0x09000000ull : 0x11002000ull;
		uint64_t uart_reg[2] = { uart_addr - io_base, 0x1000ull };
		prop_put(n, "reg", uart_reg, sizeof(uart_reg));
		if (o->is_qemu) {
			prop_u32(n, "AAPL,phandle", 0x10u);
			prop_u32(n, "boot-console", 1);
		}
		prop_u32(n, "clock-frequency", 26000000u);
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
