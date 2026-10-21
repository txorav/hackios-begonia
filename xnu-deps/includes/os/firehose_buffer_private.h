/*
 * hackios-begonia: xnu-deps os/firehose_buffer_private.h
 * -----------------------------------------------------
 * Single header for the libfirehose ("os log"/unified logging) support
 * library that xnu links against.  On a macOS host the SDK copy (from
 * Apple's private SDK, under $(SDKROOT)/usr/local/include/kernel/) is used
 * transparently; on a Linux cross-build the HackiOS port supplies this
 * xnu-deps copy plus the matching link archive (xnu-deps/libfirehose/ ->
 * libfirehose_kernel.a).
 *
 * Consumers INSIDE xnu-8796 (grep-verified, line numbers in the 8796 tree):
 *   - libkern/os/log.c:38            (`__firehose_buffer_tracepoint_reserve` /
 *                                     `__firehose_buffer_tracepoint_flush`,
 *                                     `struct firehose_buffer_range_s`)
 *   - libkern/c++/OSKext.cpp:38      (buffer types only)
 *   - bsd/kern/subr_log.c:77         (`__firehose_kernel_configuration_valid`,
 *                                     `__firehose_buffer_create`,
 *                                     `__firehose_merge_updates`,
 *                                     *_DEFAULT_CHUNK_COUNT/_DEFAULT_IO_PAGES
 *                                     macros, externed counters)
 * The xnu tree confirms these exact prototypes itself at
 *   tests/unit/mocks/mock_3rd_party.c:76-105, whose comment reads
 *   "Linking to libfirehose_kernel is not possible because it is compiled
 *   with -mkernel" - i.e. these five symbols come from the external lib.
 *
 * Parts of the firehose type surface that ARE in-tree
 * (libkern/firehose/{firehose_types_private.h,tracepoint_private.h,
 * chunk_private.h}, libkern/os/firehose.h) are deferred to via __has_include
 * to avoid duplicate typedefs; this header only supplies the missing union.
 *
 * License attribution style: original implementation, no Apple text; the
 * names/prototypes are dictated by the xnu sources they must satisfy.
 */

#ifndef _OS_FIREHOSE_BUFFER_PRIVATE_H
#define _OS_FIREHOSE_BUFFER_PRIVATE_H

#include <stdint.h>
#include <stddef.h>

#ifndef __cplusplus
#include <stdbool.h>
#endif

/* in-tree firehose types available in an xnu build context (they are
 * exported as `firehose/...` from libkern (EXPINC/EXPORT_MI_DIR)). */
#if defined(__has_include)
#if __has_include(<firehose/firehose_types_private.h>) && \
    __has_include(<firehose/tracepoint_private.h>)
#define FIREHOSE_BUFFER_TYPES_IN_TREE 1
#endif
#endif

#ifdef FIREHOSE_BUFFER_TYPES_IN_TREE
#include <firehose/firehose_types_private.h>    /* firehose_push_reply_t,
                                                 * firehose_buffer_map_info_t */
#include <firehose/tracepoint_private.h>        /* firehose_tracepoint_t,
                                                 * firehose_stream_t,
                                                 * firehose_tracepoint_id_u */
#else
/* Standalone tool context: minimal equivalents that keep the prototypes
 * below compile-clean but do NOT attempt to reproduce the full public
 * header layout (kernel side only needs these numbers). */
typedef enum {
	firehose_stream_persist  = 0,
	firehose_stream_special  = 1,
	firehose_stream_memory   = 2,
	firehose_stream_metadata = 3,
	firehose_stream_signpost = 4
} firehose_stream_t;

struct firehose_tracepoint_s { uint8_t opaque[8]; };
typedef struct firehose_tracepoint_s *firehose_tracepoint_t;

typedef struct {
	uint64_t updateNumber;
	uint64_t numberWithTrace;
} firehose_push_reply_t;

typedef uint64_t firehose_tracepoint_id_t;

typedef struct firehose_tracepoint_id_u {
	uint64_t ftid_value;
} firehose_tracepoint_id_u;
#endif /* types in tree / stand-alone */

/* ---------- the buffer driver surface (the part that was missing) -------- */

/*
 * Opaque buffer: an array of FIREHOSE_CHUNK_SIZE chunks (chunk geometry in
 * libkern/firehose/chunk_private.h: 4096-byte pages, header + cursor +
 * payload).  logd maps it via LOGBUFFERMAP (libkern/firehose/ioctl_private.h)
 * and reconstitutes it with knowledge of the driver version.
 */
typedef void *firehose_buffer_t;

/*
 * 16-byte record log.c writes into a tracepoint's public data when the
 * payload has private bytes (libkern/os/log.c lines 942-1004:
 * `sizeof(struct firehose_buffer_range_s)` added to pub_data_size then
 * fields filled as `.fbr_offset` / `.fbr_length`, both narrowed to
 * uint16_t).  If a consumer wants to parse these back, logd's private data
 * region starts at fbr_offset within the enclosing chunk.
 */
struct firehose_buffer_range_s {
	uint16_t fbr_offset;    /* offset of private data, relativ from fc->fc_start */
	uint16_t fbr_length;    /* length of the private data */
};

/* Chunk / buffer configuration variables that subr_log.c defines and that
 * the driver queries; externs must exist either in this header or top of
 * the file (SECURITY_READ_ONLY_LATE definitions in bsd/kern/subr_log.c:135
 * break otherwise). */
extern uint8_t __firehose_buffer_kernel_chunk_count;
extern uint8_t __firehose_num_kernel_io_pages;

#define FIREHOSE_BUFFER_KERNEL_CHUNK_COUNT   (__firehose_buffer_kernel_chunk_count)

/* Default firehose kernel buffer sizing (both overrideable via the
 * firehose_chunk_count / firehose_io_pages boot args handled by
 * bsd/kern/subr_log.c:oslog_init_firehose()); values match Apple's. */
#define FIREHOSE_BUFFER_KERNEL_DEFAULT_CHUNK_COUNT 4
#define FIREHOSE_BUFFER_KERNEL_DEFAULT_IO_PAGES    4

/*
 * The five libfirehose functions the kernel LINK needs; exact signatures
 * proven by xnu-8796 tests/unit/mocks/mock_3rd_party.c:76-105.
 *
 * Return/value contract used by the HackiOS port:
 *   - no logging buffer available  => __firehose_buffer_create returns NULL
 *     and __firehose_buffer_tracepoint_reserve returns NULL; callers in
 *     bsd/kern/subr_log.c / libkern/os/log.c already handle that (they fall
 *     back to the boot persist chunk) - no panics, no null derefs.
 *   - __firehose_kernel_configuration_valid expects count/pages to be sane
 *     small values; returns 0 (false) for anything else and the caller
 *     then uses the two defaults above.
 */
int  __firehose_kernel_configuration_valid(uint8_t chunk_count,
     uint8_t io_pages);
void *__firehose_buffer_create(size_t *size);
bool __firehose_merge_updates(firehose_push_reply_t update);
firehose_tracepoint_t __firehose_buffer_tracepoint_reserve(uint64_t stamp,
     firehose_stream_t stream, uint16_t pub_data_size,
     uint16_t priv_data_size, uint8_t **ft_priv_data);
void __firehose_buffer_tracepoint_flush(firehose_tracepoint_t ft,
     firehose_tracepoint_id_u ftid);

/*
 * Conversely, these four are PROVIDED by the kernel itself (libkern/os/log.c
 * defines __firehose_buffer_push_to_logd, __firehose_allocate,
 * __firehose_critical_region_enter/leave); the libfirehose glue links
 * against them.  Their prototypes live in xnu's own libkern/os/firehose.h -
 * deliberately NOT duplicated here.
 */

#endif /* _OS_FIREHOSE_BUFFER_PRIVATE_H */
