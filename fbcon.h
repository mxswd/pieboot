/* SPDX-License-Identifier: BSD-2-Clause */
/* The screen while pieboot runs: a pie, or a text console (fbcon.c). */
#ifndef PIEBOOT_FBCON_H
#define PIEBOOT_FBCON_H
#include <stdbool.h>
#include <stdint.h>

void fbcon_init(uint64_t base, uint32_t width, uint32_t height, uint32_t row_bytes, uint32_t bpp, bool text);
void fbcon_putc(char c);
bool fbcon_text(void);                  /* the pie to text, with what was printed; false if not the pie */
void fbcon_pie(void);                   /* back to the pie */
bool fbcon_question(bool on);           /* the question mark over the pie, or not; false if no pie */
#endif
