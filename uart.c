/* SPDX-License-Identifier: BSD-2-Clause */
#include "uart.h"
#include "fbcon.h"

/* PL011 registers. The firmware/QEMU has usually set up the baud rate. */
#define UARTDR   0x00
#define UARTFR   0x18
#define UARTFR_TXFF (1u << 5)
#define UARTFR_BUSY (1u << 3)
#define UARTIBRD 0x24
#define UARTFBRD 0x28
#define UARTLCR_H 0x2c
#define UARTLCR_H_WLEN8 (3u << 5)
#define UARTLCR_H_FEN   (1u << 4)
#define UARTCR   0x30
#define UARTCR_UARTEN   (1u << 0)
#define UARTCR_TXE      (1u << 8)
#define UARTCR_RXE      (1u << 9)

static volatile uint8_t *uart_base;
static bool uart_dead;                  /* its FIFO stopped draining: given up */

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(uart_base + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(uart_base + off) = v;
}

void
uart_set_base(uint64_t pa)
{
	uart_base = (volatile uint8_t *)(uintptr_t)pa;
}

/*
 * Set 8N1 at baud from the UART clock, unless the firmware already enabled
 * the UART with a baud rate (the Pi 5's debug UART may be left alone).
 */
void
uart_setup(uint32_t clock_hz, uint32_t baud)
{
	if (!uart_base || !clock_hz || !baud ||
	    ((rd(UARTCR) & UARTCR_UARTEN) && rd(UARTIBRD) != 0)) {
		return;
	}
	/* Divisor = clock / (16 baud), in 1/64ths, rounded. */
	uint32_t div64 = (uint32_t)((((uint64_t)clock_hz * 4) + baud / 2) / baud);
	wr(UARTCR, 0);
	for (uint32_t spins = 0; (rd(UARTFR) & UARTFR_BUSY) && spins < 5000000; spins++) {
	}
	wr(UARTIBRD, div64 >> 6);
	wr(UARTFBRD, div64 & 63);
	wr(UARTLCR_H, UARTLCR_H_WLEN8 | UARTLCR_H_FEN);
	wr(UARTCR, UARTCR_UARTEN | UARTCR_TXE | UARTCR_RXE);
}

/*
 * A character to the UART and to the framebuffer console. A UART whose
 * FIFO does not drain (no clock, say) is given up on after about a
 * second rather than hanging the boot.
 */
void
uart_putc(char c)
{
	fbcon_putc(c);
	if (!uart_base || uart_dead) {
		return;
	}
	if (c == '\n') {
		uart_putc_raw('\r');
	}
	uart_putc_raw(c);
}

void
uart_putc_raw(char c)
{
	if (!uart_base || uart_dead) {
		return;
	}
	for (uint32_t spins = 0; *(volatile uint32_t *)(uart_base + UARTFR) & UARTFR_TXFF; spins++) {
		if (spins > 5000000) {
			uart_dead = true;
			return;
		}
	}
	*(volatile uint32_t *)(uart_base + UARTDR) = (uint8_t)c;
}

bool
uart_working(void)
{
	return uart_base && !uart_dead;
}

void
uart_puts(const char *s)
{
	while (*s) {
		uart_putc(*s++);
	}
}
