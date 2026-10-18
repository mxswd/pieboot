/* SPDX-License-Identifier: BSD-2-Clause */
/* boot_args as defined by xnu's pexpert/pexpert/arm64/boot.h (kBootArgsRevision2). */
#ifndef PIEBOOT_BOOT_ARGS_H
#define PIEBOOT_BOOT_ARGS_H
#include <stdint.h>

#define BOOT_LINE_LENGTH    1024
#define kBootArgsRevision2  2
#define kBootArgsVersion2   2

struct Boot_Video {
	uint64_t v_baseAddr;
	uint64_t v_display;
	uint64_t v_rowBytes;
	uint64_t v_width;
	uint64_t v_height;
	uint64_t v_depth;
};

struct boot_args {
	uint16_t Revision;
	uint16_t Version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct Boot_Video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

_Static_assert(__builtin_offsetof(struct boot_args, deviceTreeP) == 96, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, bootFlags) == 1136, "boot_args layout");
#endif
