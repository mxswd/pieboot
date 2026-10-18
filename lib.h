/* SPDX-License-Identifier: BSD-2-Clause */
/* Freestanding support for pieboot. */
#ifndef PIEBOOT_LIB_H
#define PIEBOOT_LIB_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define MIN(a, b)        ((a) < (b) ? (a) : (b))
#define MAX(a, b)        ((a) > (b) ? (a) : (b))

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Bump allocator for boot-time data structures (never freed). */
void *balloc(size_t size);

static inline uint32_t be32(const void *p)
{
	const uint8_t *b = p;
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static inline uint64_t be64(const void *p)
{
	return ((uint64_t)be32(p) << 32) | be32((const uint8_t *)p + 4);
}

#endif /* PIEBOOT_LIB_H */
