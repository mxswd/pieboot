/* SPDX-License-Identifier: BSD-2-Clause */
#include "place.h"

/* The lowest address at or above pa congruent to offset modulo align, or 0 past the top. */
static uint64_t
congruent_at(uint64_t pa, uint64_t align, uint64_t offset)
{
	uint64_t a = (pa & ~(align - 1)) + offset;
	if (a < pa) {
		a += align;
	}
	return a >= pa ? a : 0;
}

/*
 * The lowest fit starts at the first congruent address at or above low or
 * above the end of a busy range: one place lower, either it was below low
 * or a busy range overlapped it, and that one ends just below here.
 */
bool
place_lowest(struct place_range ram, uint64_t low, uint64_t align, uint64_t offset,
    uint64_t size, const struct place_range *busy, unsigned n, uint64_t *out)
{
	bool found = false;
	uint64_t best = 0;

	if (low < ram.base) {
		low = ram.base;
	}
	for (unsigned i = 0; i <= n; i++) {
		uint64_t from = i < n ? busy[i].end : low;
		if (from < low) {
			from = low;
		}
		uint64_t pa = congruent_at(from, align, offset);
		if (pa < from || pa >= ram.end || ram.end - pa < size || (found && pa >= best)) {
			continue;
		}
		bool clear = true;
		for (unsigned j = 0; j < n && clear; j++) {
			clear = busy[j].end <= pa || busy[j].base >= pa + size || busy[j].base >= busy[j].end;
		}
		if (clear) {
			best = pa;
			found = true;
		}
	}
	if (found) {
		*out = best;
	}
	return found;
}
