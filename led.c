/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The board's activity LED as a progress signal for when no one can watch
 * the serial console and the screen stays dark: on while pieboot runs, off
 * once it enters the kernel, blinking fast if pieboot stops on an error.
 * Only an LED on a Broadcom STB GPIO controller (brcm,brcmstb-gpio: the
 * Pi 5's ACT LED, /leds/led-act) is driven; elsewhere these do nothing.
 */

#include "led.h"
#include "fdt.h"

static volatile uint32_t *gio;          /* the bank's registers: ODEN, DATA, IODIR, ... */
static uint32_t bit;
static bool active_low;

void
led_init(const struct fdt *fdt)
{
	struct fdt_node *n = fdt_path(fdt, "/leds/led-act");
	struct fdt_prop *gpios = n ? fdt_prop(n, "gpios") : NULL;
	if (!gpios || gpios->len < 12) {
		return;
	}
	struct fdt_node *ctl = fdt_find_phandle(fdt->root, (uint32_t)fdt_read_cells(gpios->val, 1));
	uint32_t pin = (uint32_t)fdt_read_cells(gpios->val + 4, 1);
	uint64_t base, size;
	if (!ctl || !fdt_is_compatible(ctl, "brcm,brcmstb-gpio") || !fdt_reg(ctl, 0, &base, &size)) {
		return;
	}
	/* Pins are numbered across banks of the given widths; each bank has 8 registers. */
	uint32_t bank = 0;
	struct fdt_prop *widths = fdt_prop(ctl, "brcm,gpio-bank-widths");
	for (uint32_t i = 0; widths && i + 4 <= widths->len; i += 4, bank++) {
		uint32_t w = (uint32_t)fdt_read_cells(widths->val + i, 1);
		if (pin < w) {
			break;
		}
		pin -= w;
	}
	if (pin >= 32 || (bank + 1) * 32 > size) {
		return;
	}
	gio = (volatile uint32_t *)(uintptr_t)(base + bank * 32);
	bit = 1U << pin;
	active_low = (fdt_read_cells(gpios->val + 8, 1) & 1) != 0;
	gio[2] &= ~bit;                         /* IODIR: output */
}

void
led_set(bool on)
{
	if (!gio) {
		return;
	}
	if (on != active_low) {
		gio[1] |= bit;                  /* DATA */
	} else {
		gio[1] &= ~bit;
	}
}

void
led_blink_forever(void)
{
	for (bool on = true;; on = !on) {
		led_set(on);
		for (volatile uint32_t i = 0; i < 20000000; i++) {
		}
	}
}
