/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Where the kernel goes in physical memory (main.c). Plain arithmetic, no
 * hardware: place-test.c runs it on the build host (make test).
 */
#ifndef PIEBOOT_PLACE_H
#define PIEBOOT_PLACE_H

#include <stdbool.h>
#include <stdint.h>

struct place_range {
	uint64_t base, end;             /* [base, end) */
};

/*
 * The lowest address in ram at or above low that is congruent to offset
 * modulo align (a power of two) and from which size bytes fit in ram
 * clear of every one of the n busy ranges; false if there is none.
 */
bool place_lowest(struct place_range ram, uint64_t low, uint64_t align, uint64_t offset,
    uint64_t size, const struct place_range *busy, unsigned n, uint64_t *out);

#endif
