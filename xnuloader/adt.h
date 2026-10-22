/*
 * adt.h — Apple DeviceTree (ADT) blob writer + begonia template.
 *
 * Format (native-endian, matches xnu/pexpert/gen/device_tree.c SecureDT):
 *   DeviceTreeNode       = { u32 nProperties; u32 nChildren; }
 *   DeviceTreeNodeProperty = { char name[32]; u32 length; value bytes; pad-to-4 }
 *  Children and properties are stored INLINE (recursively) after the header:
 *    [node header][ props... ][ child nodes... ]
 *  The first property of every node is "name" (NUL-terminated string).
 *  XNU's SecureDTFindEntry looks up by property (e.g. name=="chosen").
 *
 *  /chosen/memory-map entries: reg = DTMemoryMapRange[] = pairs of
 *  { uint64 paddr; uint64 length; } (little-endian, 16 bytes per region).
 */
#ifndef _XLDR_ADT_H_
#define _XLDR_ADT_H_

#include <stdint.h>

#define ADT_MAX_NODES      48
#define ADT_MAX_PROPS      16
#define ADT_MAX_CHILDREN   12

typedef void *adt_node_t;    /* opaque builder handle */

/* builder helpers */
void adt_finalize_size(void);
int  adt_size(void);
void adt_serialize(uint8_t *dst, int *len_out);   /* called once */

/* template fill: overrides come from the FDT / payload manifest */
struct adt_overrides {
	uint64_t phys_base;
	uint64_t phys_size;         /* memSize */
	uint64_t fb_base;
	uint64_t fb_size;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t fb_stride;
	uint32_t fb_depth;
	const char *cmdline;        /* full bootargs string */
	const uint8_t *randseed;    /* 16 bytes */
	/* chosen memory-map (DeviceTree buffer, RAMDisk, all phys) */
	uint64_t mm_dt_addr, mm_dt_size;
	uint64_t mm_rd_addr, mm_rd_size;
};

int adt_build(const struct adt_overrides *o);   /* builds template in RAM */

#endif /* _XLDR_ADT_H_ */
