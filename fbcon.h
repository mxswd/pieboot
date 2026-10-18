/* SPDX-License-Identifier: BSD-2-Clause */
/* A text console on a 32 bpp framebuffer (fbcon.c). */
#ifndef PIEBOOT_FBCON_H
#define PIEBOOT_FBCON_H
#include <stdint.h>

void fbcon_init(uint64_t base, uint32_t width, uint32_t height, uint32_t row_bytes, uint32_t bpp);
void fbcon_putc(char c);
#endif
