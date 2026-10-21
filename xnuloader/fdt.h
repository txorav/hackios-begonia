/*
 * fdt.h — minimal Flattened Device Tree parser for the stage-2 loader.
 * lk passes the MediaTek FDT address in x0 when it boots boot.img.
 * Only what the ADT builder needs: node lookup by path, property fetch,
 * DRAM region discovery, reserve-map enumeration.
 */
#ifndef _XLDR_FDT_H_
#define _XLDR_FDT_H_

#include <stdint.h>

#define FDT_MAGIC        0xD00DFEED      /* stored big-endian in the blob */
#define FDT_BEGIN_NODE   0x1
#define FDT_END_NODE     0x2
#define FDT_PROP         0x3
#define FDT_NOP          0x4
#define FDT_END          0x9

int   fdt_init(uint64_t base);                        /* 0 = ok */
int   fdt_find(const char *path, int *node);          /* -NOTFOUND if missing */
int   fdt_find_prefix(const char *prefix, int start_from, int *node);
int   fdt_node_eq(const char *a, const char *b);
int   fdt_get_prop(int node, const char *name, const uint8_t **val, int *len);
int   fdt_prop_cells_u32(int node, const char *name, uint32_t *out, int max);
int   fdt_prop_u64(int node, const char *name, uint64_t *out);
/* sum of "reg" lengths of every node whose path starts with `prefix` */
uint64_t fdt_mem_size(void);
/* enumerate /memreserve entries; returns count written to out[] */
int   fdt_reserves(uint64_t out[][2], int max);

extern int fdt_explain_err;   /* debugging aid */

#define FDT_E_OK        0
#define FDT_E_NOTFOUND  1
#define FDT_E_BADMAG    2
#define FDT_E_TRUNC     3
#define FDT_E_FMT       4

#endif /* _XLDR_FDT_H_ */
