/* Freestanding stdint for xnuloader (no libc on board). */
#ifndef _XLDR_STDINT_
#define _XLDR_STDINT_

typedef unsigned char      uint8_t;
typedef signed   char      int8_t;
typedef unsigned short     uint16_t;
typedef signed   short     int16_t;
typedef unsigned int       uint32_t;
typedef signed   int       int32_t;
typedef unsigned long long uint64_t;
typedef signed   long long int64_t;
typedef unsigned long      uintptr_t;
typedef long               intptr_t;

#ifndef _XLDR_SIZE_T_
#define _XLDR_SIZE_T_
typedef unsigned long      size_t;
#endif

#define UINT32_MAX 0xFFFFFFFFu
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFull
#define INT64_MIN  (-0x7FFFFFFFFFFFFFFFll - 1)

#endif /* _XLDR_STDINT_ */
