/* SPDX-License-Identifier: BSD-2-Clause */
#include "macho.h"

#define MH_MAGIC_64              0xfeedfacf
#define CPU_TYPE_ARM64           0x0100000c
#define MH_EXECUTE               0x2
#define LC_SEGMENT_64            0x19
#define LC_UNIXTHREAD            0x5
#define ARM_THREAD_STATE64       6

struct mach_header_64 {
	uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};

struct load_command {
	uint32_t cmd, cmdsize;
};

struct segment_command_64 {
	uint32_t cmd, cmdsize;
	char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

#define FOREACH_LC(k, lc) \
	for (const struct load_command *lc = (const void *)((k)->data + sizeof(struct mach_header_64)), \
	    *lc##_end = (const void *)((const uint8_t *)lc + ((const struct mach_header_64 *)(k)->data)->sizeofcmds); \
	    lc < lc##_end; lc = (const void *)((const uint8_t *)lc + lc->cmdsize))

void
macho_inspect(const void *data, size_t size, struct kernel_image *k)
{
	const struct mach_header_64 *mh = data;

	if (size < sizeof(*mh) || mh->magic != MH_MAGIC_64) {
		panic_nokernel("kernel is not a 64-bit Mach-O");
	}
	if (mh->cputype != CPU_TYPE_ARM64 || mh->filetype != MH_EXECUTE) {
		panic_nokernel("kernel is not an arm64 MH_EXECUTE (cputype 0x%x filetype %u)", mh->cputype, mh->filetype);
	}
	if (sizeof(*mh) + mh->sizeofcmds > size) {
		panic_nokernel("truncated Mach-O load commands");
	}
	k->data = data;
	k->size = size;
	k->vbase = UINT64_MAX;
	k->vend = 0;
	k->entry = 0;

	FOREACH_LC(k, lc) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *seg = (const void *)lc;
			if (!seg->vmsize) {
				continue;
			}
			if (seg->fileoff + seg->filesize > size) {
				panic_nokernel("a segment extends past the end of the kernel file");
			}
			k->vbase = MIN(k->vbase, seg->vmaddr);
			k->vend = MAX(k->vend, seg->vmaddr + seg->vmsize);
		} else if (lc->cmd == LC_UNIXTHREAD) {
			const uint32_t *t = (const uint32_t *)(lc + 1);
			if (t[0] == ARM_THREAD_STATE64) {
				/* x0-x28, fp, lr, sp, then pc */
				memcpy(&k->entry, (const uint8_t *)(t + 2) + 32 * 8, sizeof(k->entry));
			}
		}
	}
	if (k->vbase == UINT64_MAX || !k->entry) {
		panic_nokernel("kernel has no segments or no LC_UNIXTHREAD entry point");
	}
}

void
macho_load(const struct kernel_image *k, uint64_t virt_base, uint64_t phys_base)
{
	FOREACH_LC(k, lc) {
		if (lc->cmd != LC_SEGMENT_64) {
			continue;
		}
		const struct segment_command_64 *seg = (const void *)lc;
		if (!seg->vmsize) {
			continue;
		}
		uint8_t *dst = (uint8_t *)(uintptr_t)(phys_base + (seg->vmaddr - virt_base));
		uint64_t n = MIN(seg->filesize, seg->vmsize);
		char name[17];
		memcpy(dst, k->data + seg->fileoff, n);
		memset(dst + n, 0, seg->vmsize - n);
		memcpy(name, seg->segname, 16);
		name[16] = 0;
		printf("  %s: va 0x%lx pa 0x%lx size 0x%lx\n", name,
		    seg->vmaddr, (uint64_t)(uintptr_t)dst, seg->vmsize);
	}
}
