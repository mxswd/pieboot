/* SPDX-License-Identifier: BSD-2-Clause */
#include "lib.h"
#include "uart.h"
#include "led.h"

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if ((((uintptr_t)d | (uintptr_t)s | n) & 7) == 0) {
		uint64_t *d8 = dst;
		const uint64_t *s8 = src;
		for (size_t i = 0; i < n / 8; i++) {
			d8[i] = s8[i];
		}
		return dst;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if (d < s || d >= s + n) {
		return memcpy(dst, src, n);
	}
	while (n--) {
		d[n] = s[n];
	}
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;

	if (c == 0 && (((uintptr_t)d | n) & 7) == 0) {
		uint64_t *d8 = dst;
		for (size_t i = 0; i < n / 8; i++) {
			d8[i] = 0;
		}
		return dst;
	}
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i]) {
			return x[i] - y[i];
		}
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

size_t
strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n]) {
		n++;
	}
	return n;
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (uint8_t)*a - (uint8_t)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (a[i] != b[i] || !a[i]) {
			return (uint8_t)a[i] - (uint8_t)b[i];
		}
	}
	return 0;
}

char *
strchr(const char *s, int c)
{
	for (; *s; s++) {
		if (*s == (char)c) {
			return (char *)s;
		}
	}
	return c ? NULL : (char *)s;
}

size_t
strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = strlen(src);

	if (size) {
		size_t n = MIN(len, size - 1);
		memcpy(dst, src, n);
		dst[n] = 0;
	}
	return len;
}

size_t
strlcat(char *dst, const char *src, size_t size)
{
	size_t dlen = strnlen(dst, size);

	if (dlen == size) {
		return size + strlen(src);
	}
	return dlen + strlcpy(dst + dlen, src, size - dlen);
}

struct outbuf {
	char *buf;
	size_t size;
	size_t len;
};

static void
out_c(struct outbuf *o, char c)
{
	if (o->len + 1 < o->size) {
		o->buf[o->len] = c;
	}
	o->len++;
}

static void
out_num(struct outbuf *o, uint64_t v, unsigned base, int width, char pad, bool upper)
{
	char tmp[24];
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0;

	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	while (n < width) {
		tmp[n++] = pad;
	}
	while (n) {
		out_c(o, tmp[--n]);
	}
}

int
vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct outbuf o = { buf, size, 0 };

	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			out_c(&o, *fmt);
			continue;
		}
		fmt++;
		char pad = ' ';
		int width = 0;
		int lng = 0;
		if (*fmt == '0') {
			pad = '0';
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt++ - '0');
		}
		int precision = -1;             /* "%.*s": at most that many characters */
		if (fmt[0] == '.' && fmt[1] == '*') {
			precision = va_arg(ap, int);
			fmt += 2;
		}
		while (*fmt == 'l') {
			lng++;
			fmt++;
		}
		if (*fmt == 'z') {
			lng = 2;
			fmt++;
		}
		switch (*fmt) {
		case 'd': {
			int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
			if (v < 0) {
				out_c(&o, '-');
				v = -v;
			}
			out_num(&o, (uint64_t)v, 10, width, pad, false);
			break;
		}
		case 'u':
			out_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, width, pad, false);
			break;
		case 'x':
		case 'X':
			out_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, width, pad, *fmt == 'X');
			break;
		case 'p':
			out_c(&o, '0');
			out_c(&o, 'x');
			out_num(&o, (uintptr_t)va_arg(ap, void *), 16, 0, ' ', false);
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			if (!s) {
				s = "(null)";
			}
			int len = 0;
			while (s[len] && (precision < 0 || len < precision)) {
				len++;
			}
			for (int i = len; i < width; i++) {
				out_c(&o, ' ');
			}
			for (int i = 0; i < len; i++) {
				out_c(&o, s[i]);
			}
			break;
		}
		case 'c':
			out_c(&o, (char)va_arg(ap, int));
			break;
		case '%':
			out_c(&o, '%');
			break;
		default:
			out_c(&o, '%');
			out_c(&o, *fmt);
			break;
		}
	}
	if (size) {
		buf[MIN(o.len, size - 1)] = 0;
	}
	return (int)o.len;
}

int
snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

void
printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	uart_puts(buf);
}

void
panic(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	uart_puts("pieboot: fatal: ");
	uart_puts(buf);
	uart_puts("\n");
	led_blink_forever();
}

/* 4MB is plenty for the parsed FDT and the Apple device tree under construction. */
static uint8_t heap[4 << 20] __attribute__((aligned(16)));
static size_t heap_used;

void *
balloc(size_t size)
{
	size = ALIGN_UP(size, 16);
	if (heap_used + size > sizeof(heap)) {
		panic("out of boot heap (%zu bytes requested)", size);
	}
	void *p = &heap[heap_used];
	heap_used += size;
	memset(p, 0, size);
	return p;
}
