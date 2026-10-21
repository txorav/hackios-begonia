/* Freestanding stddef for xnuloader. */
#ifndef _XLDR_STDDEF_
#define _XLDR_STDDEF_

#ifndef _XLDR_SIZE_T_
#define _XLDR_SIZE_T_
typedef unsigned long      size_t;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif /* _XLDR_STDDEF_ */
