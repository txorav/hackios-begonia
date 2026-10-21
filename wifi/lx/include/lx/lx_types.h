/*
 * lx_types.h — base integer/boolean types and compiler traits for the
 *               HackiOS-begonia Linux-emulation shim layer ("lx").
 *
 * Project   : HackiOS-begonia (XNU 8796.121.2 / iOS 16.5 on Xiaomi Redmi
 *             Note 8 Pro, MediaTek Helio G90T / MT6785, ARMv8.2-A).
 * Layer     : wifi/lx  (Linux-driver hosting shim: lxkit bridge + drivers)
 * Consumers : kexts/LxStorage, kexts/FocalTouchIOHID (and future wifi kext,
 *             naming placeholder wifi/lx — hosts ALL Linux peripheral shims).
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE EXISTS
 * ---------------------------------------------------------------------------
 * Upstream Linux drivers are written against `u8..u64`, `s8..s64`, `bool`,
 * `__le32` and a handful of compiler primitives.  The lx layer re-hosts those
 * drivers' *clean-room reimplementations* (see wifi/lx/README.md, "What code
 * is copied vs. rewritten") inside XNU.  To keep the ported peripheral driver
 * code (drivers/mt6785 sources) shaped like its Linux reference while remaining a
 * clean-room artifact, we define the primitive type vocabulary here ONCE.
 *
 * This header is freestanding: it must compile both
 *   (a) inside the XNU kernel (arm64, clang, kext build, C11/C++17), and
 *   (b) on the build host (x86_64/aarch64, gcc/clang, host unit tests —
 *       see the sources in wifi/lx/tests).
 *
 * ---------------------------------------------------------------------------
 * SCAN ORDER & OWNERSHIP
 * ---------------------------------------------------------------------------
 * Canonical hardware contract: docs/hardware_map_mt6785.md (§1 SoC identity).
 * This file belongs to the shims engineer; it must not be modified by other
 * subteams.  Any contract change goes to docs/hardware_map_mt6785.md first.
 *
 * ---------------------------------------------------------------------------
 * LICENSE NOTE
 * ---------------------------------------------------------------------------
 * Type *names* follow the Linux vocabulary for readability of the mirrored
 * drivers; no Linux source content is copied.  Everything here is XNU/APSL
 * project code (see project README + Apple license headers).
 */

#ifndef LX_TYPES_H
#define LX_TYPES_H

#include <stddef.h>     /* freestanding header: NULL, size_t, uintptr_t */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* --------------------------------------------------------------------------
 * Build flavour switches.
 *
 * LX_TARGET_KERNEL  : compiling as part of a kernelcache kext (XNU host).
 * LX_HOST_TEST      : compiling as part of wifi/lx/tests (host cc, no VM).
 * LX_I386_NOOPT...  : none — only these two matter today.
 *
 * The kext Makefiles define LX_TARGET_KERNEL explicitly (see the Makefile
 * of each consumer kext); the test harness defines LX_HOST_TEST.  Exactly
 * should be set.  If both are set, LX_HOST_TEST wins (fail fast below).
 *
 * Why not auto-detect via XNU_KERNEL_PRIVATE?  That macro is private and may
 * not be defined for kext compilations; an explicit switch is cheaper and
 * unambiguous.
 * -------------------------------------------------------------------------- */
#if defined(LX_HOST_TEST) && defined(LX_TARGET_KERNEL)
# error "lx: set exactly one of LX_HOST_TEST / LX_TARGET_KERNEL"
#endif
#if !defined(LX_HOST_TEST) && !defined(LX_TARGET_KERNEL)
# error "lx: one of LX_HOST_TEST / LX_TARGET_KERNEL must be defined"
#endif

/* --------------------------------------------------------------------------
 * Fixed-width integer vocabulary (Linux-shaped).
 *
 * `unsigned long long` is used for u64 rather than `unsigned long`, matching
 * Linux (u64 is 64-bit on *every* platform Linux supports) so that mirrored
 * driver code and its printf formats remain unambiguous.
 * -------------------------------------------------------------------------- */
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

typedef signed char         s8;
typedef short               s16;
typedef int                 s32;
typedef long long           s64;

typedef unsigned char       u_int8;    /* BSD-ish aliases, used by a few   */
typedef unsigned short      u_int16;   /* mirrored snippets; keep for      */
typedef unsigned int        u_int32;   /* diffing against reference code   */
typedef unsigned long long  u_int64;

/* typedefs for pointer-provenance hygiene (not used by all drivers). */
typedef u8    __u8, _u8;
typedef u16   __u16, _u16;
typedef u32   __u32, _u32;
typedef u64   __u64, _u64;
typedef s8    __s8;
typedef s16   __s16;
typedef s32   __s32;
typedef s64   __s64;
typedef u64   bool_u64;                /* compile-time placeholder; unused */

/*
 * bool / true / false:
 *  - C: use _Bool (compiler-real boolean; overflows-free semantics).
 *  - C++: native bool already exists — do nothing.
 * Guard with __bool_true_false_are_defined so including <stdbool.h> later
 * (host tests do) stays legal.
 */
#if defined(__cplusplus)
   /* nothing */
#else
# if !defined(__bool_true_false_are_defined)
typedef _Bool  bool;
#  define bool  _Bool
#  define true  1
#  define false 0
#  define __bool_true_false_are_defined 1
# endif
#endif

/* --------------------------------------------------------------------------
 * Byte-order annotations (__le16/__le32/__le64) — documentation-only in the
 * lx world: every peripheral we host (MSDC1 @0x11240000, UFSHCI @0x11270000,
 * I2C0 @0x11007000) is little-endian and ARM64/MikroTik-style register access
 * is performed through lx_iospace.h accessors.  We keep the annotations so
 * mirrored code reads naturally.
 * -------------------------------------------------------------------------- */
typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;

/* --------------------------------------------------------------------------
 * Constants useful to mirrored drivers.
 * -------------------------------------------------------------------------- */
#define LX_U8_MAX     0xFFU
#define LX_U16_MAX    0xFFFFU
#define LX_U32_MAX    0xFFFFFFFFUL
#define LX_U64_MAX    0xFFFFFFFFFFFFFFFFULL
#define LX_S32_MAX    0x7FFFFFFF
#define LX_S32_MIN    (-0x7FFFFFFF - 1)

#define LX_PAGE_SHIFT 12UL    /* 4 KiB: we address *devices*, never XNU user pages */
#define LX_PAGE_SIZE  (1UL << LX_PAGE_SHIFT)
#define LX_PAGE_MASK  (~(LX_PAGE_SIZE - 1))

#define LX_KB(x)  ((x) * 1024ULL)
#define LX_MB(x)  ((x) * 1024ULL * 1024ULL)

/* --------------------------------------------------------------------------
 * Locked-width format helpers.
 *
 * XNU console (IOLog/kprintf) doesn't grok %llu on all paths, but does accept
 * %llu on the in-tree printf (osfmk/kern/printf.c supports %llu).  Host tests
 * use glibc printf.  Both accept the fixed-width C99 inttypes spellings we
 * define below; drivers should use them to keep output identical.
 * -------------------------------------------------------------------------- */
#define LX_FMT_U32  "u"
#define LX_FMT_S32  "d"
#define LX_FMT_U64  "llu"
#define LX_FMT_S64  "lld"
#define LX_FMT_HEX8  "02x"
#define LX_FMT_HEX32 "x"
#define LX_FMT_HEX64 "llx"

/* --------------------------------------------------------------------------
 * min/max/round helpers used liberally by shifted drivers.
 * -------------------------------------------------------------------------- */
#define LX_MIN(a, b)      ((a) < (b) ? (a) : (b))
#define LX_MAX(a, b)      ((a) > (b) ? (a) : (b))
#define LX_ROUND_UP(x, n)  (((x) + (n) - 1) / (n) * (n))
#define LX_ROUND_DOWN(x, n) ((x) & ~((n) - 1))
#define LX_DIV_ROUND_UP(x, n) (((x) + (n) - 1) / (n))
#define LX_ARRAY_SIZE(a)  (sizeof(a) / sizeof((a)[0]))

/* --------------------------------------------------------------------------
 * Barrier instructions.
 *
 * In a kernelcache kext on arm64 the correct data-synchronisation barrier
 * for MMIO write-before-read ordering around device registers is either
 *   dmb ish    : ensure visibility order across inner-shareable domain, or
 *   dsb sy     : full order to any observer (deeper, slightly slower).
 *
 * Linux `mb()`/`rmb()`/`wmb()` collapse to `dmb` variants on ARM.  For
 * MT6785 peripherals the strictest sequence (dsb sy) around SDC/DMA
 * doorbells is cheap enough at the transfer sizes we care about and removes
 * a whole class of bring-up races; we use dmb ish everywhere except DMA
 * completion (dsb sy), which mirrors Linux's ordering-relevant savings.
 *
 * On host tests barriers compile away (single-threaded parsers).
 * -------------------------------------------------------------------------- */
#if defined(LX_TARGET_KERNEL)
# if defined(__aarch64__)
#  define lx_barrier_dmb()      __asm__ volatile ("dmb ish" ::: "memory")
#  define lx_barrier_dsb()      __asm__ volatile ("dsb sy"   ::: "memory")
# else
#  error "lx: kernel build is arm64-only in this project"
# endif
#else
# define lx_barrier_dmb()  __asm__ volatile ("" ::: "memory")
# define lx_barrier_dsb()  __asm__ volatile ("" ::: "memory")
#endif /* LX_TARGET_KERNEL */

/* Linux-to-lx alias sugar: mirrored code references mb()/rmb()/wmb(). */
#define mb()  lx_barrier_dsb()
#define rmb() lx_barrier_dmb()
#define wmb() lx_barrier_dmb()

/* --------------------------------------------------------------------------
 * Attribute sugaring (unused, WARN_ON-once debug narrative).
 * -------------------------------------------------------------------------- */
#ifndef __LX_UNUSED
# define __LX_UNUSED __attribute__((unused))
#endif

/* Compile-time assert usable anywhere (no external headers). */
#ifndef LX_STATIC_ASSERT
# define LX_STATIC_ASSERT(cond, msg) \
	typedef char lx_static_assert_##msg[(cond) ? 1 : -1]
#endif

/* NLS stringification helper (mirrors Linux __stringify). */
#define LX_STRINGIFY_(x) #x
#define LX_STRINGIFY(x)  LX_STRINGIFY_(x)

/* --------------------------------------------------------------------------
 * Packed little-endian scalar loaders for wire-format parsing (touch reports,
 * GPT, SCSI payloads).  They work on any unaligned buffer and must stay
 * freestanding (registered driver clean-room rule).
 * -------------------------------------------------------------------------- */
static inline u16 lx_le16(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u16)(b[0] | (b[1] << 8));
}

static inline u32 lx_le32(const void *p)
{
	const u8 *b = (const u8 *)p;
	return (u32)((u32)b[0] | ((u32)b[1] << 8) |
	    ((u32)b[2] << 16) | ((u32)b[3] << 24));
}

static inline u64 lx_le64(const void *p)
{
	return (u64)lx_le32(p) | ((u64)lx_le32((const u8 *)p + 4) << 32);
}

static inline void lx_le_put16(void *p, u16 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v & 0xFF);
	b[1] = (u8)(v >> 8);
}

static inline void lx_le_put32(void *p, u32 v)
{
	u8 *b = (u8 *)p;
	b[0] = (u8)(v & 0xFF);
	b[1] = (u8)((v >> 8) & 0xFF);
	b[2] = (u8)((v >> 16) & 0xFF);
	b[3] = (u8)((v >> 24) & 0xFF);
}

/* Size sanity (protects against a future ABI-neutral retyping accident). */
LX_STATIC_ASSERT(sizeof(u16) == 2, u16_is_2_bytes);
LX_STATIC_ASSERT(sizeof(u32) == 4, u32_is_4_bytes);
LX_STATIC_ASSERT(sizeof(u64) == 8, u64_is_8_bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* LX_TYPES_H */
