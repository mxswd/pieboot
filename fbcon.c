/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * A text console on the framebuffer (32 or 16 bpp), for boards whose serial console no one
 * is watching: pieboot's messages (and a previous boot's panic log) on HDMI
 * before the kernel's own console takes the screen over. White on black,
 * 8x16 glyphs (doubled on wide screens); the cursor wraps to the top
 * rather than scrolling, since the framebuffer is uncached (the MMU is
 * off) and reading it back is slow.
 */

#include <stdbool.h>

#include "fbcon.h"

extern const uint8_t pieboot_font[95][16];

static volatile uint8_t *fb;
static uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
static uint32_t scale, cols, rows, col, row;

/* White or black; 32 bpp (x8r8g8b8) or 16 bpp (r5g6b5). */
static inline void
pixel(uint32_t x, uint32_t y, bool on)
{
	volatile uint8_t *p = fb + (uint64_t)y * fb_pitch;
	if (fb_bpp == 16) {
		((volatile uint16_t *)p)[x] = on ? 0xffff : 0;
	} else {
		((volatile uint32_t *)p)[x] = on ? 0xffffffff : 0xff000000;
	}
}

static void
clear_row(uint32_t r)
{
	for (uint32_t y = r * 16 * scale; y < (r + 1) * 16 * scale && y < fb_height; y++) {
		for (uint32_t x = 0; x < fb_width; x++) {
			pixel(x, y, false);
		}
	}
}

void
fbcon_init(uint64_t base, uint32_t width, uint32_t height, uint32_t row_bytes, uint32_t bpp)
{
	if (bpp != 16 && bpp != 32) {
		return;
	}
	fb = (volatile uint8_t *)(uintptr_t)base;
	fb_width = width;
	fb_height = height;
	fb_pitch = row_bytes;
	fb_bpp = bpp;
	scale = width >= 1600 ? 2 : 1;
	cols = width / (8 * scale);
	rows = height / (16 * scale);
	col = row = 0;
	for (uint32_t r = 0; r < rows; r++) {
		clear_row(r);
	}
}

static void
draw(char c)
{
	const uint8_t *g = pieboot_font[(c >= 32 && c < 127) ? c - 32 : '?' - 32];
	uint32_t x0 = col * 8 * scale, y0 = row * 16 * scale;
	for (uint32_t y = 0; y < 16 * scale; y++) {
		uint8_t bits = g[y / scale];
		for (uint32_t x = 0; x < 8 * scale; x++) {
			pixel(x0 + x, y0 + y, (bits >> (x / scale)) & 1);
		}
	}
}

static void
newline(void)
{
	col = 0;
	row = (row + 1) % rows;
	clear_row(row);                 /* where the next line goes */
}

void
fbcon_putc(char c)
{
	if (fb == 0 || rows == 0) {
		return;
	}
	if (c == '\n') {
		newline();
	} else if (c == '\r') {
		col = 0;
	} else {
		draw(c);
		if (++col == cols) {
			newline();
		}
	}
}
