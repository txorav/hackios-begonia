/*
 * string.c — freestanding bultin shims (memcpy/memset/etc.) so -nostdlib
 * links without libgcc's implicit calls pulling in libc.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *pa = a;
	const uint8_t *pb = b;
	while (n--) {
		if (*pa != *pb) {
			return *pa - *pb;
		}
		pa++; pb++;
	}
	return 0;
}

size_t
strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && (*a == *b)) {
		a++; b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && (*a == *b)) {
		a++; b++; n--;
	}
	if (n == 0) {
		return 0;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

char *
strchr(const char *s, int c)
{
	while (*s) {
		if (*s == (char)c) {
			return (char *)s;
		}
		s++;
	}
	return c ? (char *)0 : (char *)s;
}
