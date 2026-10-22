/*
 * macho.c — kernelcache Mach-O loader for xnuloader.
 *
 * Supports:
 *   - MH_EXECUTE flat kernelcache (all LC_SEGMENT_64 in the container)
 *   - MH_FILESET prelinked kernelcache (iOS-16 style; each LC_FILESET_ENTRY
 *     carries a child Mach-O; we scan the entry region for the child header
 *     magic so we do not depend on exact entry-string padding)
 *
 * Loading rule (docs/hardware_map_mt6785.md §2):
 *   PA(seg) = vmaddr - CLS_BASE + P0        CLS_BASE = min(vmaddr) over all
 *   Entry:  LC_UNIXTHREAD pc of the first child that has one (fileset), or of
 *           the container itself (flat).  PC is converted to a physical
 *           address with the same rule before jumping.
 *
 * xnu start.s is position-independent and boots identity-mapped; it then maps
 * KVA = virtBase + (phys - physBase) itself, so only the phys layout matters.
 */
#include "macho.h"
#include "console.h"
#include <stdint.h>
#include <string.h>

uint64_t g_macho_top;    /* final physical end of the relocated kernel */

struct load_region {
	uint64_t pa;
	uint64_t size;
};
static struct load_region seg_list[64];
static int seg_count;

static uint64_t cls_base;   /* min vmaddr over ALL loaded segments */
static uint64_t cls_top;    /* max (vmaddr + vmsize) over all segments */

static void
record_segment(uint64_t vmaddr, uint64_t vmsize)
{
	if (vmaddr < cls_base) {
		cls_base = vmaddr;
	}
	if (vmaddr + vmsize > cls_top) {
		cls_top = vmaddr + vmsize;
	}
}

static int
macho_load_one(const volatile uint8_t *mh, uint64_t phys_base,
	       uint64_t *entry_out, int depth)
{
	struct mach_header_64 hdr;
	memcpy(&hdr, (const void *)mh, sizeof(hdr));
	if (hdr.magic != MH_MAGIC_64 || hdr.cputype != MH_CPUTYPE_ARM64) {
		return -1;
	}
	if (hdr.filetype != MH_FILETYPE_EXEC &&
	    !(depth > 0 && hdr.filetype == MH_FILETYPE_FILESET)) {
		return -2;
	}

	/* pass 1: bounds (min vmaddr / max vmsize end) */
	const volatile uint8_t *cmd   = mh + sizeof(hdr);
	const volatile uint8_t *c_end = mh + sizeof(hdr) + hdr.sizeofcmds;
	while (cmd + 8 <= c_end) {
		uint32_t c   = cmd[0] | (cmd[1] << 8) | (cmd[2] << 16) | (cmd[3] << 24);
		uint32_t csz = cmd[4] | (cmd[5] << 8) | (cmd[6] << 16) | (cmd[7] << 24);
		if (csz < 8 || cmd + csz > c_end) {
			return -3;
		}
		if (c == LC_SEGMENT_64) {
			struct segment_command_64 seg;
			memcpy(&seg, (const void *)cmd, sizeof(seg));
			if (seg.vmsize != 0) {
				record_segment(seg.vmaddr, seg.vmsize);
			}
		}
		cmd += csz;
	}

	/* pass 2: copy + zero-fill + collect entry */
	uint64_t load_delta = phys_base - cls_base;
	uint64_t entry = 0;
	int entry_found = 0;

	cmd = mh + sizeof(hdr);
	while (cmd + 8 <= c_end) {
		uint32_t c   = cmd[0] | (cmd[1] << 8) | (cmd[2] << 16) | (cmd[3] << 24);
		uint32_t csz = cmd[4] | (cmd[5] << 8) | (cmd[6] << 16) | (cmd[7] << 24);
		if (c == LC_SEGMENT_64) {
			struct segment_command_64 seg;
			memcpy(&seg, (const void *)cmd, sizeof(seg));
			if (seg.vmsize != 0) {
				uint64_t pa = seg.vmaddr + load_delta;
				if (seg_count < 64) {
					seg_list[seg_count].pa = pa;
					seg_list[seg_count].size = seg.vmsize;
					seg_count++;
				}
				if (seg.filesize != 0) {
					memcpy((void *)pa,
					       (const void *)(mh + seg.fileoff), seg.filesize);
				}
				if (seg.vmsize > seg.filesize) {
					memset((void *)(pa + seg.filesize), 0,
					       seg.vmsize - seg.filesize);
				}
			}
		} else if (c == LC_UNIXTHREAD && !entry_found) {
			uint32_t flavor = cmd[8] | (cmd[9] << 8) |
					  (cmd[10] << 16) | (cmd[11] << 24);
			if (flavor == ARM64_THREAD_FLAVOR) {
				const uint64_t *st = (const uint64_t *)(const void *)(cmd + 16);
				entry = st[ARM64_THREAD_REG_PC];
				entry_found = 1;
			}
		} else if (c == LC_FILESET_ENTRY && depth == 0) {
			/* child header: locate MH_MAGIC_64 inside the entry block */
			for (uint32_t off = 16; off + 4 <= csz; off += 4) {
				uint32_t m = cmd[off] | (cmd[off + 1] << 8) |
					     (cmd[off + 2] << 16) | (cmd[off + 3] << 24);
				if (m == MH_MAGIC_64) {
					uint64_t child_entry = 0;
					int rc = macho_load_one(cmd + off, phys_base,
								&child_entry, depth + 1);
					if (rc == 0 && child_entry != 0) {
						entry = child_entry;
						entry_found = 1;
					}
					break;
				}
			}
		}
		cmd += csz;
	}

	if (entry_found) {
		*entry_out = (entry == 0) ? 0 : (entry + load_delta);   /* linked VA -> physical PA */
	}
	return entry_found ? 0 : -4;
}

int
macho_load(uint64_t mh_phys, uint64_t load_base, uint64_t *entry_out)
{
	seg_count = 0;
	cls_base = ~0ull;
	cls_top = 0;
	memset(seg_list, 0, sizeof(seg_list));

	uint64_t entry = 0;
	int rc = macho_load_one((const volatile uint8_t *)mh_phys, load_base, &entry, 0);
	if (rc != 0 || seg_count == 0) {
		*entry_out = 0;
		return rc != 0 ? rc : -5;
	}
	*entry_out = entry;
	g_macho_top = (cls_top - cls_base) + load_base;
	return 0;
}
