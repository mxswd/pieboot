/* SPDX-License-Identifier: BSD-2-Clause */
/* Loading the xnu kernel Mach-O. */
#ifndef PIEBOOT_MACHO_H
#define PIEBOOT_MACHO_H
#include "lib.h"

struct kernel_image {
	const uint8_t *data;
	size_t size;
	uint64_t vbase;          /* lowest vmaddr of a non-empty segment */
	uint64_t vend;           /* end of the highest non-empty segment */
	uint64_t entry;          /* LC_UNIXTHREAD pc (virtual) */
};

/* Validate a 64-bit arm64 MH_EXECUTE kernel and compute its layout. */
void macho_inspect(const void *data, size_t size, struct kernel_image *k);

/* Copy segments to phys_base + (vmaddr - virt_base), zero-filling. */
void macho_load(const struct kernel_image *k, uint64_t virt_base, uint64_t phys_base);
#endif
