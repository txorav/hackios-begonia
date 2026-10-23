/*
 * macho.h — minimal Mach-O 64 definitions for the kernelcache loader
 * (public format data, re-implemented; mirrors xnu/makedefs/loader2 docs).
 * LC_FILESET_ENTRY children: prelinked kernelcache (iOS 16 style).
 */
#ifndef _XLDR_MACHO_H_
#define _XLDR_MACHO_H_

#include <stdint.h>

#define MH_MAGIC_64         0xFEEDFACF
#define MH_CPUTYPE_ARM64    0x0100000C
#define MH_FILETYPE_EXEC    0x2
#define MH_FILETYPE_FILESET 0x0C

#define LC_SEGMENT_64       0x19
#define LC_UUID             0x24
#define LC_UNIXTHREAD       0x5
#define LC_FILESET_ENTRY    0x33

struct mach_header_64 {
	uint32_t magic;
	uint32_t cputype;
	uint32_t cpusubtype;
	uint32_t filetype;
	uint32_t ncmds;
	uint32_t sizeofcmds;
	uint32_t flags;
	uint32_t reserved;
};

struct segment_command_64 {
	uint32_t cmd;
	uint32_t cmdsize;
	char     segname[16];
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t fileoff;
	uint64_t filesize;
	uint32_t maxprot;
	uint32_t initprot;
	uint32_t nsects;
	uint32_t flags;
};

struct fileset_entry_command {
	uint32_t cmd;
	uint32_t cmdsize;
	uint64_t entry_id;       /* followed by NUL string then child header */
	uint64_t reserved;
};

struct thread_command {     /* LC_UNIXTHREAD (arm64): ARM_THREAD_STATE64 */
	uint32_t cmd;
	uint32_t cmdsize;
	uint32_t flavor;         /* 6 */
	uint32_t count;          /* 68 = 272/4 */
	/* followed by state: x0..x28(29), fp(29), lr(30), sp(31), pc(32), cpsr */
};

#define ARM64_THREAD_FLAVOR   6
#define ARM64_THREAD_REG_PC   32

#define KERNEL_VIRT_BASE      0xFFFFFFF004000000ULL
#define KERNEL_LINK_OFFSET    0x03000000ULL

/* load a kernelcache: mh at mh_phys; segments land relative to load_base. */
int      macho_load(uint64_t mh_phys, uint64_t load_base, uint64_t *entry_out);
extern uint64_t g_macho_top;   /* physical end of the relocated kernel image */

#endif /* _XLDR_MACHO_H_ */
