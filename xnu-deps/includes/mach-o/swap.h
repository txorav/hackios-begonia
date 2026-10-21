/*
 * hackios-begonia: xnu-deps Mach-O byte-order swap API
 * ---------------------------------------------------
 * Reimplementation of the classic cctools "mach-o/swap.h" surface: in-place
 * byte swapping of the integer fields of each Mach-O structure between
 * big-endian (ppc-era) and little-endian (arm64/x86_64) wire order.
 *
 * Why this exists in xnu-deps: xnu-8796 sources include <mach-o/swap.h> on
 * the HOST (non-KERNEL) build path of
 *   - libkern/kxld/kxld_util.c, kxld_object.c, kxld_kext.c
 *     (kxld host builds: validate_and_swap_macho_64 / unswap_macho_64)
 *   - SETUP/kextsymboltool/kextsymboltool.c (swap_symtab_command,
 *     swap_nlist_64, swap_uuid_command, swap_mach_header_64,
 *     swap_segment_command_64; swap_uuid_command at line 817)
 *   - SETUP/setsegname/setsegname.c
 * Apple's SDK supplies these on macOS; on Linux they must come from this
 * directory (makedefs/MakeInc.def: INCFLAGS_EXTERN means EXTERNAL_HEADERS is
 * on the search path).  The kernel build itself never includes swap.h (the
 * KERNEL paths in kxld use castswab), so this is host-tools glue.
 *
 * Function set = union of the references grep'd out of the files above plus
 * the fat-file helpers kxld_object.c needs.  All code is written from the
 * publicly documented wire format (see loader.h); no Apple source text is
 * reproduced.
 *
 * Compatibility rules (important when both contexts are compiled):
 *   1. If the IN-TREE loader.h (include guard _MACHO_LOADER_H_) is already
 *      part of the translation unit - the kxld/SETUP host-build case - the
 *      canonical 32-bit types exist and are aliased.
 *   2. Otherwise (xnu-deps standalone tool builds against our 64-bit-only
 *      loader.h), freestanding legacy 32-bit types are declared locally.
 *   3. nlist records are always swapped through explicit byte offsets so
 *      both the in-tree struct (with its n_un union) and the xnu-deps
 *      layout work; the legacy 32-bit nlist record is 12 bytes, nlist_64
 *      is 16 bytes on the wire in Fmost-endian form.
 *   4. Do not mix xnu kernel header roots (bsd/libkern) with xnu-deps
 *      includes in a freestanding tool build: the architecture/byte_order.h
 *      include pulls libkern/OSByteOrder.h which wants bsd export headers.
 */

#ifndef _HACKIOS_MACH_O_SWAP_H_
#define _HACKIOS_MACH_O_SWAP_H_

#include <stdint.h>
#include <stddef.h>

/* raw word swappers */
static inline void hackios_swap16(uint16_t *p) { *p = __builtin_bswap16(*p); }
static inline void hackios_swap32(uint32_t *p) { *p = __builtin_bswap32(*p); }
static inline void hackios_swap64(uint64_t *p) { *p = __builtin_bswap64(*p); }

/* convenience cctools-style operators used by host tool code */
#ifndef _HACKIOS_SWAP_CCTOOL_NAMES_DEFINED
#define _HACKIOS_SWAP_CCTOOL_NAMES_DEFINED
#define swap_int(x)   ((int32_t)__builtin_bswap32((uint32_t)(x)))
#define swap_uint(x)  ((uint32_t)__builtin_bswap32((uint32_t)(x)))
#define swap_short(x) ((int16_t)__builtin_bswap16((uint16_t)(x)))
#endif

/* ---- loader / nlist types ----------------------------------------------- */

#if __has_include(<mach-o/loader.h>)
#include <mach-o/loader.h>
#else
#include "loader.h"     /* sibling in this include dir */
#endif

/*
 * Byte-order descriptor: reuse xnu's architecture/byte_order.h enum when it
 * is actually reachable (kxld/SETUP host builds), else local equivalent.
 */
#if defined(__has_include)
#if __has_include(<architecture/byte_order.h>)
#include <architecture/byte_order.h>
#define HACKIOS_SWAP_HAVE_NXBYTEORDER 1
#endif
#endif

#ifndef HACKIOS_SWAP_HAVE_NXBYTEORDER
enum NXByteOrder {
	NXUnknownByteOrder,
	NX_LittleEndian,
	NX_BigEndian
};
#endif

#ifndef HACKIOS_SWAP_HOST_ORDER_DEFINED
#define HACKIOS_SWAP_HOST_ORDER_DEFINED
static inline enum NXByteOrder
hackios_host_byte_order(void)
{
#if defined(__BYTE_ORDER__)
	return (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__) ? NX_BigEndian : NX_LittleEndian;
#else
	const uint16_t probe = 0x0102;
	return (*(const uint8_t *)&probe == 0x01) ? NX_BigEndian : NX_LittleEndian;
#endif
}
#endif /* host order */

static inline int
hackios_swap_needed(enum NXByteOrder file_order)
{
	return hackios_host_byte_order() != file_order;
}

static inline void
hackios_swap64_array(uint64_t *a, size_t count, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		for (size_t i = 0; i < count; i++) {
			hackios_swap64(&a[i]);
		}
	}
}

/*
 * 32-bit structures: aliased to the in-tree/SDK types only when the
 * canonical loader has already been included (see rule 1 above).
 */
#if defined(_MACHO_LOADER_H_)
typedef struct mach_header      hackios_mach_header_32_t;
typedef struct segment_command  hackios_segment_command_32_t;
typedef struct section          hackios_section_32_t;
#else
#ifndef _HACKIOS_MACH_O_SWAP_FREESTANDING_TYPES
#define _HACKIOS_MACH_O_SWAP_FREESTANDING_TYPES
/* legacy 32-bit shapes: publicly documented wire format */
struct hackios_mach_header_32 {
	uint32_t    magic;
	int32_t     cputype;
	int32_t     cpusubtype;
	uint32_t    filetype;
	uint32_t    ncmds;
	uint32_t    sizeofcmds;
	uint32_t    flags;
};
typedef struct hackios_mach_header_32 hackios_mach_header_32_t;
struct hackios_segment_command_32 {
	uint32_t    cmd;
	uint32_t    cmdsize;
	char        segname[16];
	uint32_t    vmaddr;
	uint32_t    vmsize;
	uint32_t    fileoff;
	uint32_t    filesize;
	int32_t     maxprot;
	int32_t     initprot;
	uint32_t    nsects;
	uint32_t    flags;
};
typedef struct hackios_segment_command_32 hackios_segment_command_32_t;
struct hackios_section_32 {
	char        sectname[16];
	char        segname[16];
	uint32_t    addr;
	uint32_t    size;
	uint32_t    offset;
	uint32_t    align;
	uint32_t    reloff;
	uint32_t    nreloc;
	uint32_t    flags;
	uint32_t    reserved1;
	uint32_t    reserved2;
};
typedef struct hackios_section_32 hackios_section_32_t;
struct hackios_fat_header  { uint32_t magic; uint32_t nfat_arch; };
typedef struct hackios_fat_header hackios_fat_header_t;
struct hackios_fat_arch    {
	uint32_t cputype;
	uint32_t cpusubtype;
	uint32_t offset;
	uint32_t size;
	uint32_t align;
};
typedef struct hackios_fat_arch hackios_fat_arch_t;
#endif /* freestanding types */
#endif /* _MACHO_LOADER_H_ */

#ifdef _MACHO_NLIST_H_
typedef struct fat_header hackios_fat_header_t_alias_;
typedef struct fat_arch   hackios_fat_arch_t_alias_;
#else
typedef struct hackios_fat_header hackios_fat_header_t_alias_;
typedef struct hackios_fat_arch   hackios_fat_arch_t_alias_;
#endif

/*
 * Legacy 32-bit nlist record (8 field bytes, 3 pad bytes on 4-byte padding
 * hosts): n_strx(4) n_type(1) n_sect(1) n_desc(2) n_value(4).
 * Both the in-tree <mach-o/nlist.h> "struct nlist" and tool builds match
 * this layout, so swap through explicit byte offsets.
 */
static inline void
hackios_swap_nlist(void *syms_v, uint32_t nsyms, enum NXByteOrder order)
{
	uint8_t *base = (uint8_t *)syms_v;
	for (uint32_t i = 0; i < nsyms; i++) {
		uint8_t *rec = base + 12 * (size_t)i;
		if (hackios_swap_needed(order)) {
			hackios_swap32((uint32_t *)(rec + 0));      /* n_strx */
			hackios_swap16((uint16_t *)(rec + 4));      /* n_desc */
			hackios_swap32((uint32_t *)(rec + 6));      /* n_value */
		}
	}
}

/*
 * 64-bit nlist records: 16 bytes =
 *   n_strx(4) n_type(1) n_sect(1) n_desc(2) n_value(8).
 * The in-tree struct wraps n_strx in an n_un union; the xnu-deps loader.h
 * uses a plain member - identical wire layout and offsets, so byte access.
 */
static inline void
hackios_swap_nlist_64(struct nlist_64 *syms, uint32_t nsyms,
    enum NXByteOrder order)
{
	uint8_t *base = (uint8_t *)syms;
	for (uint32_t i = 0; i < nsyms; i++) {
		uint8_t *rec = base + 16 * (size_t)i;
		if (hackios_swap_needed(order)) {
			hackios_swap32((uint32_t *)(rec + 0));      /* n_strx */
			hackios_swap16((uint16_t *)(rec + 4));      /* n_desc */
			hackios_swap64((uint64_t *)(rec + 8));      /* n_value */
		}
	}
}

/* ---- headers -------------------------------------------------------------- */

static inline void
hackios_swap_mach_header_64(struct mach_header_64 *mh, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&mh->magic);
		hackios_swap32((uint32_t *)&mh->cputype);
		hackios_swap32((uint32_t *)&mh->cpusubtype);
		hackios_swap32(&mh->filetype);
		hackios_swap32(&mh->ncmds);
		hackios_swap32(&mh->sizeofcmds);
		hackios_swap32(&mh->flags);
	}
}

static inline void
hackios_swap_load_command(struct load_command *lc, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&lc->cmd);
		hackios_swap32(&lc->cmdsize);
	}
}

static inline void
hackios_swap_segment_command_64(struct segment_command_64 *sg,
    enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&sg->cmd);
		hackios_swap32(&sg->cmdsize);
		hackios_swap64(&sg->vmaddr);
		hackios_swap64(&sg->vmsize);
		hackios_swap64(&sg->fileoff);
		hackios_swap64(&sg->filesize);
		hackios_swap32((uint32_t *)&sg->maxprot);
		hackios_swap32((uint32_t *)&sg->initprot);
		hackios_swap32(&sg->nsects);
		hackios_swap32(&sg->flags);
	}
}

static inline void
hackios_swap_section_64(struct section_64 *st, uint32_t nsects,
    enum NXByteOrder order)
{
	for (uint32_t i = 0; i < nsects; i++) {
		struct section_64 *s = &st[i];
		if (hackios_swap_needed(order)) {
			hackios_swap64(&s->addr);
			hackios_swap64(&s->size);
			hackios_swap32(&s->offset);
			hackios_swap32(&s->align);
			hackios_swap32(&s->reloff);
			hackios_swap32(&s->nreloc);
			hackios_swap32(&s->flags);
			hackios_swap32(&s->reserved1);
			hackios_swap32(&s->reserved2);
			hackios_swap32(&s->reserved3);
		}
	}
}

static inline void
hackios_swap_uuid_command(struct uuid_command *uc, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&uc->cmd);
		hackios_swap32(&uc->cmdsize);
	}
}

static inline void
hackios_swap_symtab_command(struct symtab_command *st, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&st->cmd);
		hackios_swap32(&st->cmdsize);
		hackios_swap32(&st->symoff);
		hackios_swap32(&st->nsyms);
		hackios_swap32(&st->stroff);
		hackios_swap32(&st->strsize);
	}
}

static inline void
hackios_swap_dysymtab_command(struct dysymtab_command *st, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		uint32_t *f = &st->ilocalsym;
		/* ilocalsym..nlocrel is a contiguous run of 18 uint32 fields */
		for (int i = 0; i < 18; i++) {
			hackios_swap32(&f[i]);
		}
		hackios_swap32(&st->cmd);
		hackios_swap32(&st->cmdsize);
	}
}

static inline void
hackios_swap_dylib_command(struct dylib_command *dl, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&dl->cmd);
		hackios_swap32(&dl->cmdsize);
		hackios_swap32(&dl->dylib.name);
		hackios_swap32(&dl->dylib.timestamp);
		hackios_swap32(&dl->dylib.current_version);
		hackios_swap32(&dl->dylib.compatibility_version);
	}
}

static inline void
hackios_swap_thread_command(struct thread_command *tc, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&tc->cmd);
		hackios_swap32(&tc->cmdsize);
		/* flavor/count/state words are caller-owned; use
		 * hackios_swap64_array / raw hackios_swap32 above. */
	}
}

/* 32-bit walkers (canonical types in xnu context, local types in tool build) */
static inline void
hackios_swap_mach_header_32(hackios_mach_header_32_t *mh, enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&mh->magic);
		hackios_swap32((uint32_t *)&mh->cputype);
		hackios_swap32((uint32_t *)&mh->cpusubtype);
		hackios_swap32(&mh->filetype);
		hackios_swap32(&mh->ncmds);
		hackios_swap32(&mh->sizeofcmds);
		hackios_swap32(&mh->flags);
	}
}

static inline void
hackios_swap_segment_command_32(hackios_segment_command_32_t *sg,
    enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&sg->cmd);
		hackios_swap32(&sg->cmdsize);
		hackios_swap32(&sg->vmaddr);
		hackios_swap32(&sg->vmsize);
		hackios_swap32(&sg->fileoff);
		hackios_swap32(&sg->filesize);
		hackios_swap32((uint32_t *)&sg->maxprot);
		hackios_swap32((uint32_t *)&sg->initprot);
		hackios_swap32(&sg->nsects);
		hackios_swap32(&sg->flags);
	}
}

static inline void
hackios_swap_section_32(hackios_section_32_t *st, uint32_t nsects,
    enum NXByteOrder order)
{
	for (uint32_t i = 0; i < nsects; i++) {
		hackios_section_32_t *s = &st[i];
		if (hackios_swap_needed(order)) {
			hackios_swap32(&s->addr);
			hackios_swap32(&s->size);
			hackios_swap32(&s->offset);
			hackios_swap32(&s->align);
			hackios_swap32(&s->reloff);
			hackios_swap32(&s->nreloc);
			hackios_swap32(&s->flags);
			hackios_swap32(&s->reserved1);
			hackios_swap32(&s->reserved2);
		}
	}
}

/* ---- fat file helpers (kxld_object.c swap_fat_header/swap_fat_arch) ------ */

static inline void
hackios_swap_fat_header(hackios_fat_header_t_alias_ *fh,
    enum NXByteOrder order)
{
	if (hackios_swap_needed(order)) {
		hackios_swap32(&fh->magic);
		hackios_swap32(&fh->nfat_arch);
	}
}

static inline void
hackios_swap_fat_arch(hackios_fat_arch_t_alias_ *arch, uint32_t count,
    enum NXByteOrder order)
{
	for (uint32_t i = 0; i < count; i++) {
		hackios_fat_arch_t_alias_ *f = &arch[i];
		if (hackios_swap_needed(order)) {
			hackios_swap32(&f->cputype);
			hackios_swap32(&f->cpusubtype);
			hackios_swap32(&f->offset);
			hackios_swap32(&f->size);
			hackios_swap32(&f->align);
		}
	}
}

/* ---- generic relocation walker ------------------------------------------- */

/*
 * relocation_info is two packed 32-bit words (address, then the
 * r_symbolnum/r_pcrel/r_length/r_extern/r_type bitfield word).  Word-pointer
 * signature avoids the in-tree/standalone struct-name debate; arm64 files
 * carry no relocations anyway.
 */
static inline void
hackios_swap_relocation_info(uint32_t *relocs_packed, uint32_t nrelocs,
    enum NXByteOrder order)
{
	for (uint32_t i = 0; i < nrelocs; i++) {
		if (hackios_swap_needed(order)) {
			hackios_swap32(&relocs_packed[2 * i]);
			hackios_swap32(&relocs_packed[2 * i + 1]);
		}
	}
}

/* ---- cctools-compatible canonical names ---------------------------------- */

#define swap_mach_header_64          hackios_swap_mach_header_64
#define swap_segment_command_64      hackios_swap_segment_command_64
#define swap_section_64              hackios_swap_section_64
#define swap_symtab_command          hackios_swap_symtab_command
#define swap_dysymtab_command        hackios_swap_dysymtab_command
#define swap_uuid_command            hackios_swap_uuid_command
#define swap_dylib_command           hackios_swap_dylib_command
#define swap_thread_command          hackios_swap_thread_command
#define swap_nlist_64                hackios_swap_nlist_64
#define swap_nlist                   hackios_swap_nlist
#define swap_mach_header             hackios_swap_mach_header_32
#define swap_segment_command         hackios_swap_segment_command_32
#define swap_section                 hackios_swap_section_32
#define swap_fat_header              hackios_swap_fat_header
#define swap_fat_arch                hackios_swap_fat_arch
#define swap_load_command            hackios_swap_load_command
#define swap_relocation_info         hackios_swap_relocation_info

#endif /* _HACKIOS_MACH_O_SWAP_H_ */
