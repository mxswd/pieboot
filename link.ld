/* SPDX-License-Identifier: BSD-2-Clause */
/* pieboot is linked at 0 as a static PIE and relocates itself (start.S). */
ENTRY(_start)

SECTIONS
{
	. = 0;
	__image_start = .;
	.text : {
		KEEP(*(.text.head))
		*(.text .text.*)
	}
	.rodata : { *(.rodata .rodata.*) }
	.data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) }
	.rela.dyn : {
		__rela_start = .;
		*(.rela .rela.*)
		__rela_end = .;
	}
	.data : { *(.data .data.*) }
	.got : { *(.got .got.*) }
	. = ALIGN(4096);
	__image_file_end = .;
	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss .bss.* COMMON)
		. = ALIGN(16);
		__bss_end = .;
	}
	__image_end = .;

	/DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame) }
}
