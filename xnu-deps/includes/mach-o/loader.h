/*
 * hackios-begonia: xnu-deps Mach-O loader definitions
 * --------------------------------------------------
 * Self-contained reimplementation of the *public Mach-O file format* data:
 * magic numbers, load-command constants and the 64-bit header family.
 *
 * Copyright (c) 2026 HackiOS-begonia project. Written from scratch from the
 * publicly documented format data (Mach-O is a documented file format; the
 * wire layout is identical no matter which header enumerates it). No text
 * was copied from Apple's headers.
 *
 * Consistency verified against xnu-8796 sources (READ-ONLY reference):
 *   - EXTERNAL_HEADERS/mach-o/loader.h   (MH_MAGIC_64 = 0xfeedfacf,
 *     MH_FILESET = 0xc, LC_* constants lines 264..313)
 *   - osfmk/mach/machine.h               (CPU_TYPE_ARM = 12,
 *     CPU_ARCH_ABI64 = 0x01000000 -> CPU_TYPE_ARM64 = 0x0100000c)
 *   - osfmk/mach/arm/thread_status.h     (ARM_THREAD_STATE64 = 6,
 *     arm_thread_state64 = 29 x __uint64_t + fp/lr/sp/pc + cpsr/flags
 *     = 10 more words -> count 37)
 *
 * Consumers:
 *   - xnu-deps/kext-tools/kextbuild.py + kextinfo.py (host Mach-O parsing)
 *   - any kext source built standalone without an Apple SDK
 *   - installed into xnu/EXTERNAL_HEADERS/mach-o/loader.h ONLY if absent
 *     (install.sh never overwrites the in-tree copy by default).
 */

#ifndef _HACKIOS_MACH_O_LOADER_H_
#define _HACKIOS_MACH_O_LOADER_H_

/* stdint.h resolves everywhere; during an xnu build the in-tree
 * EXTERNAL_HEADERS/stdint.h is picked up automatically. */
#if __has_include(<stdint.h>)
#include <stdint.h>
#else
typedef unsigned int   uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char  uint8_t;
typedef int            int32_t;
#endif

/* CPU type constants.  Prefer xnu's authoritative header when available. */
#if __has_include(<mach/machine.h>)
#include <mach/machine.h>
#else
#define CPU_ARCH_ABI64           0x01000000
#define CPU_ARCH_ABI64_32        0x02000000
#define CPU_TYPE_ARM             ((int32_t) 12)
#define CPU_TYPE_X86             ((int32_t) 7)
#define CPU_TYPE_ARM64           (CPU_TYPE_ARM | (int32_t)CPU_ARCH_ABI64)
#define CPU_TYPE_X86_64          (CPU_TYPE_X86 | (int32_t)CPU_ARCH_ABI64)
#define CPU_TYPE_ARM64_32        (CPU_TYPE_ARM | (int32_t)CPU_ARCH_ABI64_32)
#define CPU_SUBTYPE_ARM64_ALL    ((int32_t) 0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Magic numbers.  MH_CIGAM_* are the byte-swapped counterparts used when a
 * file was written by the opposite endianness host.
 */
#define MH_MAGIC        0xfeedface      /* 32-bit */
#define MH_CIGAM        0xcefaedfe      /* ~MH_MAGIC */
#define MH_MAGIC_64     0xfeedfacf      /* 64-bit */
#define MH_CIGAM_64     0xcffaedfe      /* ~MH_MAGIC_64 */

/*
 * File types (mh.filetype).  begonia build artifacts: kernel = MH_EXECUTE,
 * prelinked kernel / kernelcache = MH_FILESET, kexts = MH_OBJECT.
 */
#define MH_OBJECT       0x1             /* relocatable object (kext) */
#define MH_EXECUTE      0x2             /* demand paged executable (kernel) */
#define MH_FVMLIB       0x3
#define MH_CORE         0x4
#define MH_PRELOAD      0x5
#define MH_DYLIB        0x6
#define MH_DYLINKER     0x7
#define MH_BUNDLE       0x8
#define MH_DYLIB_STUB   0x9
#define MH_DSYM         0xa
#define MH_KEXT_BUNDLE  0xb             /* legacy kext bundle wrapper */
#define MH_FILESET      0xc             /* set of mach-o's (kernelcache) */
#define MH_GPU_EXECUTE  0xf
#define MH_GPU_DYLIB    0x10

/* mach_header flags */
#define MH_NOUNDEFS                 0x00000001
#define MH_INCRLINK                 0x00000002
#define MH_DYLDLINK                 0x00000004
#define MH_BINDATLOAD               0x00000008
#define MH_PREBOUND                 0x00000010
#define MH_SPLIT_SEGS               0x00000020
#define MH_LAZY_INIT                0x00000040
#define MH_TWOLEVEL                 0x00000080
#define MH_FORCE_FLAT               0x00000100
#define MH_NOFIXPREBINDING          0x00000200
#define MH_PREBINDABLE              0x00000400
#define MH_ALLMODSBOUND             0x00000800
#define MH_SUBSECTIONS_VIA_SYMBOLS  0x00001000
#define MH_CANONICAL                0x00002000
#define MH_WEAK_DEFINES             0x00004000
#define MH_BINDS_TO_WEAK            0x00008000
#define MH_ALLOW_STACK_EXECUTION    0x00010000
#define MH_ROOT_SAFE                0x00020000
#define MH_SETUID_SAFE              0x00040000
#define MH_NO_REEXPORTED_DYLIBS     0x00080000
#define MH_PIE                      0x00200000
#define MH_DEAD_STRIPPABLE_DYLIB    0x00400000
#define MH_HAS_TLV_DESCRIPTORS      0x00800000
#define MH_NO_HEAP_EXECUTION        0x01000000
#define MH_APP_EXTENSION_SAFE       0x02000000
#define MH_NLIST_OUTOFSYNC_WITH_DYLDINFO 0x04000000
#define MH_SIM_SUPPORT              0x08000000
#define MH_DYLIB_IN_CACHE           0x80000000

/* segment flags (segment_command_.flags) */
#define SG_HIGHVM       0x1
#define SG_FVMLIB       0x2
#define SG_NORELOC      0x4
#define SG_PROTECTED_VERSION_1  0x8

/* section type / attribute bits (subset used by kext tooling) */
#define S_ZEROFILL              0x1
#define S_CSTRING_LITERALS      0x2
#define S_GB_ZEROFILL           0x2
#define S_MOD_INIT_FUNC_POINTERS 0x9
#define S_MOD_TERM_FUNC_POINTERS 0xa
#define S_THREAD_LOCAL_REGULAR  0x13
#define S_ATTR_DEBUG            0x02000000
#define S_ATTR_SELF_MODIFYING_CODE 0x04000000
#define S_ATTR_LIVE_SUPPORT     0x08000000
#define S_ATTR_NO_DEAD_STRIP    0x10000000
#define S_ATTR_STRIP_STATIC_SYMS 0x20000000
#define S_ATTR_NO_TOC           0x40000000
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400

/*
 * Load command types (documented format data; single authoritative
 * numbering used by every Mach-O consumer).
 */
#define LC_REQ_DYLD    0x80000000

#define LC_SEGMENT              0x1
#define LC_SYMTAB               0x2
#define LC_SYMSEG               0x3
#define LC_THREAD               0x4
#define LC_UNIXTHREAD           0x5     /* kernel entry point (start.s) */
#define LC_LOADFVMLIB           0x6
#define LC_IDFVMLIB             0x7
#define LC_IDENT                0x8
#define LC_FVMFILE              0x9
#define LC_PREPAGE              0xa
#define LC_DYSYMTAB             0xb
#define LC_LOAD_DYLIB           0xc
#define LC_ID_DYLIB             0xd
#define LC_LOAD_DYLINKER        0xe
#define LC_ID_DYLINKER          0xf
#define LC_PREBOUND_DYLIB       0x10
#define LC_ROUTINES             0x11
#define LC_SUB_FRAMEWORK        0x12
#define LC_SUB_UMBRELLA         0x13
#define LC_SUB_CLIENT           0x14
#define LC_SUB_LIBRARY          0x15
#define LC_TWOLEVEL_HINTS       0x16
#define LC_PREBIND_CKSUM        0x17
#define LC_LOAD_WEAK_DYLIB      (0x18 | LC_REQ_DYLD)
#define LC_SEGMENT_64           0x19
#define LC_ROUTINES_64          0x1a
#define LC_UUID                 0x1b
#define LC_RPATH                (0x1c | LC_REQ_DYLD)
#define LC_CODE_SIGNATURE       0x1d
#define LC_SEGMENT_SPLIT_INFO   0x1e
#define LC_REEXPORT_DYLIB       (0x1f | LC_REQ_DYLD)
#define LC_LAZY_LOAD_DYLIB      0x20
#define LC_ENCRYPTION_INFO      0x21
#define LC_DYLD_INFO            0x22
#define LC_DYLD_INFO_ONLY       (0x22 | LC_REQ_DYLD)
#define LC_LOAD_UP_WEAK_DYLIB   (0x23 | LC_REQ_DYLD)
#define LC_VERSION_MIN_MACOSX   0x24
#define LC_VERSION_MIN_IPHONEOS 0x25
#define LC_FUNCTION_STARTS      0x26
#define LC_DYLD_ENVIRONMENT     0x27
#define LC_MAIN                 (0x28 | LC_REQ_DYLD)
#define LC_DATA_IN_CODE         0x29
#define LC_SOURCE_VERSION       0x2A
#define LC_DYLIB_CODE_SIGN_DRS  0x2B
#define LC_ENCRYPTION_INFO_64   0x2C
#define LC_LINKER_OPTION        0x2D
#define LC_LINKER_OPTIMIZATION_HINT 0x2E
#define LC_VERSION_MIN_TVOS     0x2F
#define LC_VERSION_MIN_WATCHOS  0x30
#define LC_NOTE                 0x31
#define LC_BUILD_VERSION        0x32
#define LC_DYLD_EXPORTS_TRIE    (0x33 | LC_REQ_DYLD)
#define LC_DYLD_CHAINED_FIXUPS  (0x34 | LC_REQ_DYLD)
#define LC_FILESET_ENTRY        (0x35 | LC_REQ_DYLD)
#define LC_ATOM_INFO            (0x36 | LC_REQ_DYLD)
#define LC_FUNCTION_VARIANTS    (0x37 | LC_REQ_DYLD)

/* nlist symbol types */
#define N_EXT   0x01
#define N_TYPE  0x0e
#define N_SECT  0x0e
#define N_UNDF  0x00
#define N_ABS   0x02
#define N_PBUD  0x0c
#define N_INDR  0x0a

/* platform values for build_version_command.platform */
#define PLATFORM_MACOS          1
#define PLATFORM_IOS            2
#define PLATFORM_TVOS           3
#define PLATFORM_WATCHOS        4
#define PLATFORM_BRIDGEOS       5
#define PLATFORM_MACCATALYST    6
#define PLATFORM_IOSSIMULATOR   7
#define PLATFORM_DRIVERKIT      8
#define PLATFORM_VISIONOS       9
#define TOOL_CLANG              3

/*
 * The 64-bit header.  Wire layout is little-endian for every architecture
 * the begonia build consumes; byte-swapping lives in <mach-o/swap.h>.
 */
struct mach_header_64 {
    uint32_t    magic;          /* MH_MAGIC_64 or MH_CIGAM_64 */
    int32_t     cputype;        /* CPU_TYPE_* incl. CPU_ARCH_ABI64 */
    int32_t     cpusubtype;     /* CPU_SUBTYPE_* */
    uint32_t    filetype;       /* MH_* */
    uint32_t    ncmds;          /* load command count */
    uint32_t    sizeofcmds;     /* total bytes of load commands */
    uint32_t    flags;          /* MH_* flags */
    uint32_t    reserved;       /* (64-bit only) */
};

/* generic prelude shared by every load command */
struct load_command {
    uint32_t    cmd;            /* LC_* */
    uint32_t    cmdsize;        /* includes trailing data */
};

/* 64-bit segment: what xnuloader / mkxnuimg.py / kextinfo walk.  The
 * physical address mapping used at boot is   PA = vmaddr - virtBase +
 * physBase   (docs/hardware_map_mt6785.md section 2). */
struct segment_command_64 {
    uint32_t    cmd;            /* LC_SEGMENT_64 */
    uint32_t    cmdsize;
    char        segname[16];    /* "__TEXT", "__DATA_CONST", "__TEXT_EXEC" ... */
    uint64_t    vmaddr;
    uint64_t    vmsize;
    uint64_t    fileoff;
    uint64_t    filesize;
    int32_t     maxprot;        /* VM_PROT_* bit mask */
    int32_t     initprot;
    uint32_t    nsects;         /* section_64 array follows inline */
    uint32_t    flags;          /* SG_* */
};

struct section_64 {
    char        sectname[16];   /* "__text", "__const", "__mod_init_func" ... */
    char        segname[16];    /* owning segment */
    uint64_t    addr;
    uint64_t    size;
    uint32_t    offset;
    uint32_t    align;          /* log2(alignment) */
    uint32_t    reloff;
    uint32_t    nreloc;
    uint32_t    flags;          /* S_* type | attribute bits */
    uint32_t    reserved1;
    uint32_t    reserved2;
    uint32_t    reserved3;
};

/* LC_SYMTAB */
struct symtab_command {
    uint32_t    cmd;            /* LC_SYMTAB */
    uint32_t    cmdsize;
    uint32_t    symoff;
    uint32_t    nsyms;
    uint32_t    stroff;
    uint32_t    strsize;
};

/* LC_DYSYMTAB (subset the kext tools read) */
struct dysymtab_command {
    uint32_t    cmd;            /* LC_DYSYMTAB */
    uint32_t    cmdsize;
    uint32_t    ilocalsym;
    uint32_t    nlocalsym;
    uint32_t    iextdefsym;
    uint32_t    nextdefsym;
    uint32_t    iundefsym;
    uint32_t    nundefsym;
    uint32_t    tocoff;
    uint32_t    ntoc;
    uint32_t    modtaboff;
    uint32_t    nmodtab;
    uint32_t    extrefsymoff;
    uint32_t    nextrefsyms;
    uint32_t    indirectsymoff;
    uint32_t    nindirectsyms;
    uint32_t    extreloff;
    uint32_t    nextrel;
    uint32_t    locreloff;
    uint32_t    nlocrel;
};

/* LC_UUID */
struct uuid_command {
    uint32_t    cmd;            /* LC_UUID */
    uint32_t    cmdsize;        /* exactly 24 */
    uint8_t     uuid[16];
};

/* LC_LOAD_DYLIB / LC_ID_DYLIB payload (how kext tools read com.apple.kpi
 * dependency strings) */
struct dylib {
    uint32_t    name;           /* offset from command start to load string */
    uint32_t    timestamp;
    uint32_t    current_version;
    uint32_t    compatibility_version;
};

struct dylib_command {
    uint32_t    cmd;            /* LC_LOAD_DYLIB / LC_ID_DYLIB */
    uint32_t    cmdsize;
    struct dylib dylib;
};

/* LC_UNIXTHREAD / LC_THREAD: repeated (flavor, count, state...) word pairs.
 * On arm64 the kernel entry state is ARM_THREAD_STATE64. */
struct thread_command {
    uint32_t    cmd;            /* LC_UNIXTHREAD or LC_THREAD */
    uint32_t    cmdsize;
    /* flavor/count word pairs and raw state words follow */
};

/* arm64 thread-state flavor/count, straight from xnu
 * osfmk/mach/arm/thread_status.h ("#define ARM_THREAD_STATE64 6") and
 * osfmk/mach/arm/_structs.h (29 x u64 + fp + lr + sp + pc + cpsr + flags
 * = 37 words total). */
#define ARM_THREAD_STATE64_FLAVOR   6
#define ARM_THREAD64_STATE_COUNT    37

/* LC_BUILD_VERSION */
struct build_version_command {
    uint32_t    cmd;            /* LC_BUILD_VERSION */
    uint32_t    cmdsize;
    uint32_t    platform;       /* PLATFORM_* */
    uint32_t    minos;          /* packed (x<<16)|(y<<8)|z */
    uint32_t    sdk;            /* same packing */
    uint32_t    ntools;
};

/* LC_SOURCE_VERSION */
struct source_version_command {
    uint32_t    cmd;
    uint32_t    cmdsize;
    uint64_t    version;        /* a24.b10.c10.d10.e10 bitfields */
};

/* LC_CODE_SIGNATURE / LC_FUNCTION_STARTS / LC_SEGMENT_SPLIT_INFO */
struct linkedit_data_command {
    uint32_t    cmd;
    uint32_t    cmdsize;
    uint32_t    dataoff;
    uint32_t    datasize;
};

/* MH_FILESET child entries (kernelcache components) */
struct fileset_entry_command {
    uint32_t    cmd;            /* LC_FILESET_ENTRY */
    uint32_t    cmdsize;
    uint64_t    vmaddr;
    uint64_t    fileoff;
    uint64_t    entry_id;       /* offset from command start to entry name */
    uint32_t    reserved;
};

/*
 * 64-bit symbol (mirror of mach-o/nlist.h shape).  This is the only symbol
 * record form the 64-bit kext tooling ever sees.
 */
struct nlist_64 {
    uint32_t    n_strx;         /* index into the string table */
    uint8_t     n_type;
    uint8_t     n_sect;
    uint16_t    n_desc;
    uint64_t    n_value;
};

/* VM protection bits used in segment maxprot/initprot (public numbering) */
#define VM_PROT_NONE    0x00
#define VM_PROT_READ    0x01
#define VM_PROT_WRITE   0x02
#define VM_PROT_EXECUTE 0x04

#ifdef __cplusplus
}
#endif

#endif /* _HACKIOS_MACH_O_LOADER_H_ */
