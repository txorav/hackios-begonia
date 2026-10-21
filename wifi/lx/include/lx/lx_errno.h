/*
 * lx_errno.h — Linux-style errno vocabulary for the lx shim layer.
 *
 * Included in every ported driver (clean-room rule: names match the Linux
 * vocabulary so mirrored logic reads identically; values are stable POSIX
 * constants, not copied code).
 *
 * Two categories of error codes circulate in the drivers:
 *   1. libc-style negated errnos returned by kmalloc/dma/queue helpers
 *      (-LX_ENOMEM, -LX_EINVAL...) — mirror of include/linux/errno.h use.
 *   2. IOReturn codes at the lxkit boundary (see lxkit_api.h).
 * lx_lx_err_to_iok() converts (1)→(2) and is the ONLY sanctioned direction
 * (the reverse direction is not meaningful — consumers never send Linux
 * errnos into lx drivers).
 */

#ifndef LX_ERRNO_H
#define LX_ERRNO_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* POSIX errno constants (stable, public). */
#define LX_EPERM	1	/* Operation not permitted			 */
#define LX_ENOENT	2	/* No such file or directory		 */
#define LX_EIO		5	/* I/O error					 */
#define LX_ENXIO	6	/* No such device or address		 */
#define LX_EBADF	9	/* Bad file number				 */
#define LX_ENOMEM	12	/* Out of memory				 */
#define LX_EACCES	13	/* Permission denied			 */
#define LX_EBUSY	16	/* Device or resource busy		 */
#define LX_EEXIST	17	/* File exists					 */
#define LX_ENODEV	19	/* No such device				 */
#define LX_EINVAL	22	/* Invalid argument				 */
#define LX_ENOTTY	25	/* Not a typewriter				 */
#define LX_EFBIG	27	/* File too large					 */
#define LX_ENOSPC	28	/* No space left on device		 */
#define LX_ETIMEDOUT	110	/* Connection timed out			 */
#define LX_EAGAIN	11	/* Try again					 */
#define LX_EINTR	4	/* Interrupted system call		 */
#define LX_EOVERFLOW	75	/* Value too large for defined dt */
#define LX_ENOTSUP	95	/* Operation not supported		 */
#define LX_ETOOMANYREFS 109	/* Generic "too many references"   */
#define LX_EDEADLK	35	/* Resource deadlock would occur	 */

/* Free success constant mirroring Linux's 0==OK convention. */
#define LX_OK		0

/*
 * Map a Linux-style failure (negative errno) to an IOReturn.
 * lxkit consumers (kexts/*) must use this at the boundary — see
 * lxkit_api.h, struct lxkit_storage_client::ops->on_request_done().
 */
static inline unsigned int
lx_err_to_iok(int lxerr)
{
	if (lxerr == 0)
		return 0;                      /* kIOReturnSuccess */
	if (lxerr == -LX_ETIMEDOUT)
		return 0x80000001u;            /* kIOReturnTimeout */
	if (lxerr == -LX_ENOMEM)
		return 0x10000006u;            /* kIOReturnNoMemory */
	if (lxerr == -LX_ENODEV)
		return 0x10000008u;            /* kIOReturnNoDevice */
	if (lxerr == -LX_EIO)
		return 0x80000013u;            /* kIOReturnIOError */
	if (lxerr == -LX_EINVAL)
		return 0xe000004cu;            /* kIOReturnBadArgument */
	if (lxerr == -LX_EBUSY)
		return 0x10000009u;            /* kIOReturnBusy */
	if (lxerr == -LX_ENOTSUP)
		return 0xe000004cu + 4;        /* kIOReturnUnsupported region */
	/* Generic fallback — never silently succeed. */
	return 0x80000013u;                /* kIOReturnIOError */
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LX_ERRNO_H */
