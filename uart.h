/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef PIEBOOT_UART_H
#define PIEBOOT_UART_H
#include <stdbool.h>
#include <stdint.h>

/* Early PL011 console; output is dropped until uart_set_base(). */
void uart_set_base(uint64_t pa);
void uart_setup(uint32_t clock_hz, uint32_t baud);
void uart_putc(char c);                 /* also on the framebuffer console */
void uart_putc_raw(char c);             /* the UART alone, no newline translation */
bool uart_working(void);
void uart_puts(const char *s);
#endif
