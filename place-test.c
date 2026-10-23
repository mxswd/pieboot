/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * place_lowest() on the build host (make test), in the layouts the
 * firmware leaves: each case says where the kernel must go.
 */
#include <stdio.h>
#include <stdlib.h>

#include "place.h"

#define MB              0x100000ULL
#define BLOCK           (32 * MB)       /* KERNEL_L2_BLOCK */
#define OFFSET          (16 * MB)       /* virtBase 0xfffffe0007000000 modulo the block */
#define NONE            UINT64_MAX

static int failed;

static void
check(const char *name, struct place_range ram, uint64_t low, uint64_t size,
    const struct place_range *busy, unsigned n, uint64_t want)
{
	uint64_t got = NONE;
	if (!place_lowest(ram, low, BLOCK, OFFSET, size, busy, n, &got)) {
		got = NONE;
	}
	bool ok = got == want;
	/* Whatever it says, it must be a fit. */
	if (got != NONE) {
		ok = ok && got >= low && got >= ram.base && got + size <= ram.end && got % BLOCK == OFFSET;
		for (unsigned i = 0; i < n; i++) {
			ok = ok && (busy[i].end <= got || busy[i].base >= got + size);
		}
	}
	printf("%s: %s: 0x%llx", ok ? "ok" : "FAILED", name, (unsigned long long)got);
	if (!ok) {
		printf(" (want 0x%llx)", (unsigned long long)want);
		failed = 1;
	}
	printf("\n");
}

int
main(void)
{
	/*
	 * The 4 GB Pi 4: pieboot at 0x200000, the FDT and bundle just below
	 * 0x2F000000. main.c raises low to 32 MB (xnu's L2 block arithmetic).
	 */
	struct place_range pi4 = { 0, 0x3b400000 };
	uint64_t pieboot_end = 0x200000 + 0x470000;
	uint64_t low = BLOCK;
	struct place_range pi4_busy[] = {
		{ 0x2eff0000, 0x2f000000 },     /* FDT */
		{ 0x2c600000, 0x2efe0000 },     /* bundle */
	};
	check("pi4: below the FDT and the bundle", pi4, low, 22 * MB, pi4_busy, 2, 48 * MB);
	check("pi4: without the 32 MB floor", pi4, pieboot_end, 22 * MB, pi4_busy, 2, 16 * MB);
	check("pi4: an 80 MB RAM disk", pi4, low, 104 * MB, pi4_busy, 2, 48 * MB);
	check("pi4: a RAM disk too big for below", pi4, low, 760 * MB, pi4_busy, 2, NONE);
	check("pi4: just fits below", pi4, low, 0x2c600000 - 48 * MB, pi4_busy, 2, 48 * MB);

	/* QEMU raspi4b: the bundle at 128 MB, the FDT right after it. */
	struct place_range qemu = { 0, 0x3c000000 };
	struct place_range qemu_busy[] = {
		{ 0x09290000, 0x092b06ce },     /* FDT */
		{ 0x08000000, 0x09290000 },     /* bundle */
	};
	check("qemu: below the bundle", qemu, low, 20 * MB, qemu_busy, 2, 48 * MB);
	check("qemu: a 140 MB footprint goes above the FDT", qemu, low, 140 * MB, qemu_busy, 2,
	    0x0b000000);
	check("qemu: just fits below", qemu, low, 0x08000000 - 48 * MB, qemu_busy, 2, 48 * MB);
	check("qemu: just too big for below", qemu, low, 0x08000000 - 48 * MB + 1, qemu_busy, 2,
	    0x0b000000);
	/* QEMU virt: RAM at 1 GB, pieboot at 2 MB in it, the bundle at 128 MB in it. */
	struct place_range virt = { 0x40000000, 0xc0000000 };
	struct place_range virt_busy[] = {
		{ 0x49290000, 0x492b06ce },
		{ 0x48000000, 0x49290000 },
	};
	check("virt: below the bundle", virt, 0x40200000 + 0x470000, 20 * MB, virt_busy, 2, 0x41000000);

	/* A framebuffer in RAM, with room below it for the kernel's first allocations. */
	struct place_range fb_busy[] = {
		{ 0x2eff0000, 0x2f000000 },
		{ 0x2c600000, 0x2efe0000 },
		{ 0x02000000 - 126 * MB, 0x02000000 + 8 * MB },   /* clamped at 0 by main.c */
	};
	fb_busy[2].base = 0;
	check("framebuffer low in RAM: the kernel goes above it", pi4, low, 22 * MB, fb_busy, 3,
	    0x03000000);
	fb_busy[2].base = 0x3e000000 - 512 * MB;
	fb_busy[2].end = 0x3e000000 + 8 * MB;
	check("framebuffer high in RAM: no change", pi4, low, 22 * MB, fb_busy, 3, 48 * MB);

	/* A /memreserve/ entry main.c keeps below the kernel raises low. */
	check("a memreserve at 0x5000000-0x5100000", pi4, 0x5100000, 22 * MB, pi4_busy, 2, 0x07000000);
	/* Nothing busy; RAM that does not start at 0; low below RAM. */
	check("nothing busy", (struct place_range){ 0x80000000, 0x90000000 }, 0, 22 * MB, NULL, 0,
	    0x81000000);
	check("RAM too small", (struct place_range){ 0, 0x2000000 }, pieboot_end, 22 * MB, NULL, 0, NONE);
	check("at the very end of RAM", (struct place_range){ 0, 0x3000000 }, pieboot_end, 32 * MB, NULL, 0,
	    0x1000000);
	/* A busy range ending past the top of the address space never wraps. */
	struct place_range top_busy[] = { { 0x10000000, UINT64_MAX } };
	check("busy to the top", (struct place_range){ 0, UINT64_MAX }, pieboot_end, 22 * MB, top_busy, 1,
	    16 * MB);
	check("busy to the top, too big below", (struct place_range){ 0, UINT64_MAX }, pieboot_end,
	    0x10000000, top_busy, 1, NONE);

	printf("place-test: %s\n", failed ? "FAIL" : "PASS");
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
