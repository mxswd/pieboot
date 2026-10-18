/* SPDX-License-Identifier: BSD-2-Clause */
/* The activity LED as a boot progress signal (led.c). */
#ifndef PIEBOOT_LED_H
#define PIEBOOT_LED_H
#include <stdbool.h>
#include "fdt.h"

void led_init(const struct fdt *fdt);
void led_set(bool on);
void led_blink_forever(void) __attribute__((noreturn));
#endif
