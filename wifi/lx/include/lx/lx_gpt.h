/*
 * lx_gpt.h — GUID Partition Table (GPT) scanner for the lx shim layer.
 *
 * Header-only, C-only, HOST-UNIT-TESTED (wifi/lx/tests/test_gpt.c compiles
 * this exact code on the build host).  kexts/LxStorage uses it to produce
 * IOMedia-shaped partition nubs from the GPT that systemd-boot/loader/
 * Android-flasher left on the µSD or UFS medium; having it as a pure
 * function of (const void *buf, size_t len) keeps the bring-up flow easy:
 * read 2_leb512 bytes from the medium, hand to lx_gpt_scan(), get nubs.
 *
 * ---------------------------------------------------------------------------
 * SPEC NOTES (clean-room; UEFI 2.10 §5 - the format is public)
 * ---------------------------------------------------------------------------
 * GPT layout on disk (logical blocks of LBASZ bytes):
 *   LBA 0       : protective MBR (not parsed here)
 *   LBA 1       : primary GPT header (92-byte table follows a 12-byte tag)
 *   LBA 2..     : primary partition entries (128 entries × entry size)
 *   LBA -33..-2 : (redundant entries, ignored on scan)
 *   LBA -1      : backup GPT header
 *
 * We scan: signature "EFI PART" (8 bytes), then revision 1.00 (u32
 * rev == 0x00010000), partition entry size 8.  Every entry is tested for
 * "zero data" detection, then a type GUID check — we accept everything
 * but record "bsd-ish/hackios-like" types when the GUID matches known
 * Apple values (APFS: 0x7c3457ef...; HFS+ : 0x48465300...) so consumers
 * can mark boot medias.  Entry parsing is little-endian on disk; the
 * begonia SoC is LE, and the mt6785 bus keeps words in LE.
 */

#ifndef LX_GPT_H
#define LX_GPT_H

#include "lx_types.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ==========================================================================
 * Constants (UEFI-specified)
 * ========================================================================= */
#define LX_GPT_HDR_LBA        1                     /* header is at LBA1    */
#define LX_GPT_SECTOR         512                   /* logical block size   */
#define LX_GPT_SIGNATURE      "EFI PART"            /* 8-byte tag           */
#define LX_GPT_REVISION_1_00  0x00010000u           /* 1.00 encoded         */
#define LX_GPT_MAX_ENTRIES    128                   /* spec plateau         */
#define LX_GPT_MIN_ENTRY_SIZE 128
#define LX_GPT_MAX_ENTRY_SIZE 512                   /* some tuners use larger */

/* partition entry attributes; bit 57..59 contain the "is boot" hints.  We
 * pad with a couple of extra ones for marker scenarios. */
#define LX_GPT_ATTR_BOOT(bit)  (((u64)1u) << (bit))

/* partition type GUIDs of interest for HackiOS bring-up */
static const u8 lx_gpt_guid_apfs[16] = {        /* Apple APFS */
	0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0x11, 0xaa,
	0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac,
};
static const u8 lx_gpt_guid_hfsplus[16] = {     /* Apple HFS+ */
	0x00, 0x53, 0x46, 0x48, 0x00, 0x00, 0x11, 0xaa,
	0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac,
};
static const u8 lx_gpt_guid_apple_ufs[16] = {   /* Apple UFS (unused today, */
	0xaf, 0x88, 0x9d, 0xa1, 0xfc, 0x05, 0x3b, 0x4d, /* kept for research) */
	0xb0, 0x04, 0x3d, 0x7a, 0x53, 0xb0, 0x04, 0x3a,
};
static const u8 lx_gpt_guid_esp[16] = {         /* EFI system partition */
	0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
	0xba, 0x4b, 0x00, 0xa0, 0x48, 0x0d, 0x02, 0x3a,
	/* GUID c12a7328-f81f-11d2-ba4b-00a0480d023a, on-disk byte order */
};

/* ==========================================================================
 * Types
 * ========================================================================= */

/* One parsed partition row (out parameter of lx_gpt_scan() it fills). */
struct lx_gpt_part {
	u8    type_guid[16];
	u8    part_guid[16];
	u64   first_lba;
	u64   last_lba;
	u64   attributes;
	u16   name_utf16le[36];  /* 72 bytes unplanned; use spare names */
	char  name_ascii[36];    /* builtin name for OSDictionary        */
	bool  used;
	u32   index;             /* entry number in the table (0-based)    */
};

/* scan output */
struct lx_gpt_scan {
	/* Header sanity, for the consumer to log. */
	u64      first_usable_lba;  /* first data LBA usable by partitions */
	u64      last_usable_lba;   /* last data LBA usable within the disk */
	u32      entry_size;        /* 128 by default                      */
	u32      num_entries;       /* spec plateau                        */
	u64      part_array_first_lba;
	bool     header_ok;

	struct lx_gpt_part parts[LX_GPT_MAX_ENTRIES];
	u32      used;              /* count of parts[].used==true         */
};

/* ==========================================================================
 * Header offsets (UEFI table)
 * ========================================================================= */
static const size_t lx_gpt_off_sig        = 0;    /* "EFI PART", 8 bytes     */
static const size_t lx_gpt_off_revision   = 8;    /* u32                     */
static const size_t lx_gpt_off_hdr_size   = 12;   /* u32 little endian       */
static const size_t lx_gpt_off_disk_guid  = 56;   /* 16-byte GUID            */
static const size_t lx_gpt_off_first_usable_lba   = 34;  /* u64 */
static const size_t lx_gpt_off_last_usable_lba    = 42;  /* u64 */
static const size_t lx_gpt_off_part_array_lba     = 72;  /* u64 */
static const size_t lx_gpt_off_num_entries        = 80;  /* u32  */
static const size_t lx_gpt_off_entry_size         = 84;  /* u32  */
static const size_t lx_gpt_off_entry_crc32        = 88;  /* u32  */

/* partition entry subfields (X = entry_size relations) */
static const size_t lx_gpt_ent_off_type   = 0;
static const size_t lx_gpt_ent_off_guid   = 16;
static const size_t lx_gpt_ent_off_first  = 32;   /* u64 */
static const size_t lx_gpt_ent_off_last   = 40;   /* u64 */
static const size_t lx_gpt_ent_off_attrs  = 48;   /* u64 */
static const size_t lx_gpt_ent_off_name   = 56;   /* 72 bytes UTF16 LE   */

/* ==========================================================================
 * utf16-le-to-ascii (bounded)
 * ========================================================================= */
static inline void
lx_gpt_utf16_to_ascii(const u16 *src16, size_t n16, char *dst, size_t dstn)
{
	size_t i, j = 0;

	if (dstn == 0)
		return;
	for (i = 0; i < n16 && j + 1 < dstn; i++) {
		u16 c = src16[i];
		if (c == 0)
			break;
		dst[j++] = (c < 128 && c >= 32) ? (char)c : '?';
	}
	dst[j] = '\0';
}

/* Local byte-equal compare — lx_gpt.h is STANDALONE (host tests include it
 * without the lx_kmem.h stack; keep the file dependency-free). */
static inline int lx_gpt_memeq(const void *a, const void *b, size_t n)
{
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	size_t i;

	for (i = 0; i < n; i++)
		if (pa[i] != pb[i])
			return 1;
	return 0;
}

/* ==========================================================================
 * Header + entry table scan.
 *
 * `hdr_buf` points at the FIRST 512 bytes of the header LBA
 *   (i.e. the data read from DiskLBA = 1 * block_size).
 * `entries_buf` points at num_entries * entry_size bytes read from the
 * partition table LBA range.
 *
 * Results are defensive: any malformed input leaves hdr_ok = false and the
 * struct zeroed.  No allocation, no I/O.
 * ========================================================================= */

static inline void lx_gpt_zero_scan(struct lx_gpt_scan *s)
{
	size_t i;
	for (i = 0; i < LX_GPT_MAX_ENTRIES; i++) {
		s->parts[i].used = false;
		s->parts[i].index = (u32)i;
	}
	s->used = 0;
	s->header_ok = false;
	s->first_usable_lba = 0;
	s->last_usable_lba = 0;
	s->entry_size = 0;
	s->num_entries = 0;
	s->part_array_first_lba = 0;
}

static inline int lx_gpt_scan_header(const void *hdr_buf, size_t len,
    struct lx_gpt_scan *s)
{
	lx_gpt_zero_scan(s);
	if (hdr_buf == NULL || len < LX_GPT_SECTOR)
		return -1;             /* too small (a 512 B sector minimum) */

	const u8 *b = (const u8 *)hdr_buf;

	/* 1. signature */
	if (lx_gpt_memeq(b + lx_gpt_off_sig, LX_GPT_SIGNATURE, 8) != 0)
		return -2;             /* not a GPT header */

	/* 2. revision */
	u32 rev = lx_le32(b + lx_gpt_off_revision);
	if (rev != LX_GPT_REVISION_1_00)
		return -3;             /* unknown spec revision */

	/* 3. sizes */
	u32 hdr_size = lx_le32(b + lx_gpt_off_hdr_size);
	if (hdr_size < 92 || hdr_size > LX_GPT_SECTOR)
		return -4;

	u32 n_entries = lx_le32(b + lx_gpt_off_num_entries);
	u32 entry_size = lx_le32(b + lx_gpt_off_entry_size);
	if (entry_size < LX_GPT_MIN_ENTRY_SIZE ||
	    entry_size > LX_GPT_MAX_ENTRY_SIZE || n_entries == 0)
		return -5;
	if (n_entries > LX_GPT_MAX_ENTRIES)
		n_entries = LX_GPT_MAX_ENTRIES;   /* defensive clamp */

	s->entry_size = entry_size;
	s->num_entries = n_entries;
	s->part_array_first_lba = lx_le64(b + lx_gpt_off_part_array_lba);
	/* Disk GUID is informational only in this scanner; the consumer logs
	 * the partition GUIDs it cares about (APFS/HFS+) directly. */
	s->header_ok = true;

	return 0;
}

/* `entries_buf` points at (entry_size * n) bytes of the partition table. */
static inline int
lx_gpt_scan_entries(const void *entries_buf, size_t len,
    struct lx_gpt_scan *s)
{
	u32 i;
	size_t stride;
	const u8 *block = (const u8 *)entries_buf;

	if (s == NULL || !s->header_ok)
		return -1;
	if (entries_buf == NULL)
		return -2;

	stride = (size_t)s->entry_size;
	if (len < (size_t)s->num_entries * stride)
		return -3;             /* consumer gave too few bytes */

	for (i = 0; i < s->num_entries; i++) {
		const u8 *e     = block + (size_t)i * stride;
		struct lx_gpt_part *p = &s->parts[i];

		/* First LBAs ABOVE last → "unused" (spec) */
		p->first_lba = lx_le64(e + lx_gpt_ent_off_first);
		p->last_lba  = lx_le64(e + lx_gpt_ent_off_last);
		if (p->first_lba == 0 || p->last_lba < p->first_lba)
			continue;

		for (size_t k = 0; k < 16; k++)
			p->type_guid[k] = e[lx_gpt_ent_off_type + k];
		for (size_t k = 0; k < 16; k++)
			p->part_guid[k] = e[lx_gpt_ent_off_guid + k];

		p->attributes = lx_le64(e + lx_gpt_ent_off_attrs);

		/* name (72 bytes UTF16 LE = 36 u16) */
		const u16 *nm = (const u16 *)(e + lx_gpt_ent_off_name);
		lx_gpt_utf16_to_ascii(nm, 36, p->name_ascii,
		    sizeof(p->name_ascii));

		p->used = true;
		s->used++;
	}
	return s->used == 0 ? -4 : 0;
}

/* ==========================================================================
 * Convenience wrapper: is a partition of known APFS/HFS+ kind?
 * ========================================================================= */
static inline int lx_gpt_part_is_apfs(const struct lx_gpt_part *p)
{
	return lx_gpt_memeq(p->type_guid, lx_gpt_guid_apfs, 16) == 0;
}

static inline int lx_gpt_part_is_hfsplus(const struct lx_gpt_part *p)
{
	return lx_gpt_memeq(p->type_guid, lx_gpt_guid_hfsplus, 16) == 0;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_GPT_H */
