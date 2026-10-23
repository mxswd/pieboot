/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The screen while pieboot runs (a 32 or 16 bpp framebuffer), for boards
 * whose serial console no one is watching. Without -v, a pie in the middle
 * of a black screen (pie.c), with a question mark flashing over it when
 * there is no kernel to boot. With -v, or once another fatal error or a
 * previous boot's panic log turns it to text, a console of pieboot's
 * messages, from the start, before the kernel's own console takes the
 * screen over. White on black, 8x16 glyphs (glyphs and pie doubled on
 * wide screens); the cursor wraps to the top rather than scrolling, since
 * the framebuffer is uncached (the MMU is off) and reading it back is slow.
 */

#include "fbcon.h"

extern const uint8_t pieboot_font[95][16];
extern const uint32_t pie_palette[];
extern const uint16_t pie_plain_box[4], pie_question_box[4];
extern const uint8_t pie_plain[], pie_question[];

static volatile uint8_t *fb;
static uint32_t fb_width, fb_height, fb_pitch, fb_bpp;
static uint32_t scale, cols, rows, col, row;
static bool text;                       /* the console; else the pie */

/* The last 4 KB printed, for the screen to show when it turns to text. */
static char backlog[4096];
static uint64_t backlog_len;

/* 0xRRGGBB; 32 bpp (x8r8g8b8) or 16 bpp (r5g6b5). */
static inline void
pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
	volatile uint8_t *p = fb + (uint64_t)y * fb_pitch;
	if (fb_bpp == 16) {
		((volatile uint16_t *)p)[x] = (uint16_t)(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
	} else {
		((volatile uint32_t *)p)[x] = 0xff000000 | rgb;
	}
}

static void
clear_lines(uint32_t y0, uint32_t y1)
{
	for (uint32_t y = y0; y < y1 && y < fb_height; y++) {
		for (uint32_t x = 0; x < fb_width; x++) {
			pixel(x, y, 0);
		}
	}
}

static void
clear_row(uint32_t r)
{
	clear_lines(r * 16 * scale, (r + 1) * 16 * scale);
}

/*
 * A box of the pie emoji (pie.c: { x, y, width, height } on the emoji),
 * from its runs of { count, palette index }, with the pie in the middle
 * of the screen.
 */
static void
image(const uint16_t box[4], const uint8_t *runs)
{
	if (pie_plain_box[2] * scale > fb_width || pie_plain_box[3] * scale > fb_height) {
		return;
	}
	uint32_t x0 = (fb_width - pie_plain_box[2] * scale) / 2 + (box[0] - pie_plain_box[0]) * scale;
	uint32_t y0 = (fb_height - pie_plain_box[3] * scale) / 2 + (box[1] - pie_plain_box[1]) * scale;
	for (uint32_t x = 0, y = 0; y < box[3]; runs += 2) {
		uint32_t rgb = pie_palette[runs[1]];
		for (uint32_t n = runs[0]; n > 0 && y < box[3]; n--) {
			for (uint32_t dy = 0; dy < scale; dy++) {
				for (uint32_t dx = 0; dx < scale; dx++) {
					pixel(x0 + x * scale + dx, y0 + y * scale + dy, rgb);
				}
			}
			if (++x == box[2]) {
				x = 0;
				y++;
			}
		}
	}
}

void
fbcon_init(uint64_t base, uint32_t width, uint32_t height, uint32_t row_bytes, uint32_t bpp, bool with_text)
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
	text = with_text;
	clear_lines(0, fb_height);
	if (!text) {
		image(pie_plain_box, pie_plain);
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
			pixel(x0 + x, y0 + y, (bits >> (x / scale)) & 1 ? 0xffffff : 0);
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

static void
put(char c)
{
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

void
fbcon_putc(char c)
{
	backlog[backlog_len++ % sizeof(backlog)] = c;
	if (fb == 0 || rows == 0 || !text) {
		return;
	}
	put(c);
}

bool
fbcon_text(void)
{
	if (fb == 0 || rows == 0 || text) {
		return false;
	}
	text = true;
	clear_lines(0, fb_height);
	col = row = 0;
	uint64_t i = backlog_len > sizeof(backlog) ? backlog_len - sizeof(backlog) : 0;
	if (i > 0) {                    /* from the first whole line */
		while (i < backlog_len && backlog[i++ % sizeof(backlog)] != '\n') {
		}
	}
	for (; i < backlog_len; i++) {
		put(backlog[i % sizeof(backlog)]);
	}
	return true;
}

void
fbcon_pie(void)
{
	if (fb == 0) {
		return;
	}
	text = false;
	clear_lines(0, fb_height);
	image(pie_plain_box, pie_plain);
}

bool
fbcon_question(bool on)
{
	if (fb == 0 || text) {
		return false;
	}
	if (on) {
		image(pie_question_box, pie_question);
	} else {
		image(pie_plain_box, pie_plain);
	}
	return true;
}
