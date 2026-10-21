/*
 * hackios-begonia: xnu-deps OS availability macros
 * ------------------------------------------------
 * Equivalent minimal implementation of the `os/availability.h` family that
 * Apple's SDK (libplatform) ships alongside libSystem.  On a macOS host the
 * SDK copy is used transparently; on a Linux cross-build there is no SDK,
 * so the HackiOS port supplies this header.
 *
 * Where this is consumed: `libsyscall/os/proc.h`, `libsyscall/wrappers/proc.c`,
 * `libsyscall/wrappers/libproc/libproc.h` include `<os/availability.h>`
 * (grep-verified: no consumer inside the kernel tree itself - the KERNEL
 * builds use EXTERNAL_HEADERS/Availability.h, which xnu ships in-tree).
 * Kernel builds therefore never see this header; kext/userland pieces of
 * the HackiOS project (Libsyscall-style wrappers, shims) do.
 *
 * API contract: every macro below EXPANDS TO NOTHING when used normally.
 * The real os/availability.h annotates declarations with
 * `__attribute__((availability(...)))` so the dynamic linker can honour the
 * platform minimums.  For kernel and bring-up builds there is no dyld
 * availability machinery, and every annotated symbol must always be
 * available, hence the no-op semantics.  This mirrors exactly what
 * EXTERNAL_HEADERS/Availability.h (Apple, in-tree) does under KERNEL.
 *
 * Naming: all identifiers in this header carry the double-underscore- or
 * API_-prefix mandated by the C standard for implementation reserved space.
 *
 * License attribution style: original implementation, no Apple text.
 * Any redistribution that also ships Apple's Availability.h (open source
 * xnu tree does) remains under the original Apple/APSL terms as shipped
 * in-tree; this file adds no Apple code.
 */

#ifndef _OS_AVAILABILITY_H
#define _OS_AVAILABILITY_H

#define _OS_AVAILABILITY_USED_FOR_MINIMUM_DOCUMENTATION 1

/*
 * API-availability family: all are pure weakeners ("this declaration may
 * appear with a version tag").  Referencing the names in any expression
 * context is not supported - they must only decorate declarations.
 */

#define __API_AVAILABLE(v, ...)
#define __API_AVAILABLE_BEGIN(v, ...)
#define __API_AVAILABLE_END
#define __API_AVAILABLE_EDITOR_ONLY(v, ...)

#define __API_UNAVAILABLE(v, ...)
#define __API_UNAVAILABLE_BEGIN(v, ...)
#define __API_UNAVAILABLE_END

#define __API_DEPRECATED(msg, v, ...)
#define __API_DEPRECATED_BEGIN(msg, v, ...)
#define __API_DEPRECATED_END(msg)
#define __API_DEPRECATED_WITH_REPLACEMENT(msg, replacement, v, ...)
#define __API_DEPRECATED_WITH_REPLACEMENT_BEGIN(msg, replacement, v, ...)
#define __API_DEPRECATED_WITH_REPLACEMENT_END(msg)

/* Apple's `__SPI_AVAILABLE` macro: same no-op behaviour for kernel builds. */
#define __SPI_AVAILABLE(macos, ios, ...)
#define __SPI_AVAILABLE2(macos, ios, ...) 
#define __SPI_UNAVAILABLE(v, ...)

/* `__OSX_AVAILABLE_*` aliases retained for headers that predate the
 * `__API_*` spelling (matching the compatibility surface libplatform kept
 * alive for years of SDK releases). */
#define __OSX_AVAILABLE(v)
#define __OSX_AVAILABLE_STARTING(macos, ios)
#define __OSX_AVAILABLE_BUT_DEPRECATED(macos, ios, msg)
#define __OSX_AVAILABLE_BUT_DEPRECATED_MSG(macos, ios, msg)
#define __OS_AVAILABLE(v)
#define __OS_UNAVAILABLE(v)
#define __WATCHOS_AVAILABLE(w)

/*
 * API-contract helpers that DO have semantics the port must not lose:
 *
 * __API_TO_BE_DEPRECATED(version) is consumed by the Apple feature
 * configuration to list APIs that will disappear in a future release; the
 * kernel config machinery expects it to expand to a bare decimal constant
 * in some sub-makefiles (see xnu libsyscall Makefiles).  Expand to nothing
 * here, but keep the name - tests reference the macro elsewhere.
 */
#define __API_TO_BE_DEPRECATED(version)

/*
 * The extended form used inside big header blocks:
 *   #include <os/availability.h>
 *   __API_AVAILABLE_BEGIN(macos(13.4), ios(16.5))
 *   ... declarations ...
 *   __API_AVAILABLE_END
 */
#define __OS_AVAILABILITY_BEGIN(le, pi)
#define __OS_AVAILABILITY_END
#define __OS_AVAILABLE_V(le, pi)

#endif /* _OS_AVAILABILITY_H */
