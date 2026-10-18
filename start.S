/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * pieboot entry. The image starts with a Linux arm64 Image header, so any
 * loader for arm64 Linux kernels starts it with x0 = FDT physical address,
 * MMU off, at EL2 or EL1.
 */
#include "psci.h"

	.section .text.head, "ax"
	.globl	_start
_start:
	b	entry				// code0
	.long	0				// code1
	.quad	0				// text_offset
	.quad	__image_end - _start		// image_size (includes .bss)
	.quad	0xa				// flags: LE, 4K pages, place anywhere
	.quad	0				// res2
	.quad	0				// res3
	.quad	0				// res4
	.ascii	"ARM\x64"			// magic
	.long	0				// res5

entry:
	mov	x19, x0				// FDT

	adrp	x1, boot_stack_top
	add	x1, x1, :lo12:boot_stack_top
	mov	sp, x1

	// Clear .bss.
	adrp	x1, __bss_start
	add	x1, x1, :lo12:__bss_start
	adrp	x2, __bss_end
	add	x2, x2, :lo12:__bss_end
1:	cmp	x1, x2
	b.hs	2f
	stp	xzr, xzr, [x1], #16
	b	1b
2:
	// Apply R_AARCH64_RELATIVE relocations for the actual load address.
	adr	x0, _start
	bl	pieboot_relocate

	mov	x0, x19
	bl	pieboot_main
3:	wfe
	b	3b

/*
 * void pieboot_enter_kernel(uint64_t entry_pa, uint64_t boot_args_pa)
 *
 * Enter the kernel at EL1 with the MMU and caches off, x0 = boot_args.
 * From EL2: run EL1 in AArch64, give it the physical timer/counter, and
 * stop trapping FP/SIMD and system registers.
 */
	.text
	.globl	pieboot_enter_kernel
pieboot_enter_kernel:
	mov	x19, x0
	mov	x20, x1
	msr	daifset, #0xf
	ic	iallu
	dsb	sy
	isb

	mrs	x2, CurrentEL
	lsr	x2, x2, #2
	cmp	x2, #2
	b.ne	1f

	// EL2 -> EL1. Stay resident at EL2 to serve PSCI calls (psci.c):
	// install the vectors and this CPU's EL2 stack.
	adrp	x2, pieboot_el2_vectors
	add	x2, x2, :lo12:pieboot_el2_vectors
	msr	vbar_el2, x2
	mrs	x2, mpidr_el1
	ubfx	x3, x2, #8, #8			// Aff1 (the Cortex-A76's core number)
	and	x2, x2, #0xff			// Aff0 (the A72's)
	add	x2, x2, x3
	and	x2, x2, #0x7			// CPU 0..7
	add	x2, x2, #1
	adrp	x3, el2_stacks
	add	x3, x3, :lo12:el2_stacks
	add	x3, x3, x2, lsl #12		// 4 KB per CPU, top of the slot
	mov	sp, x3

	mov	x2, #(1 << 31)			// HCR_EL2.RW: EL1 is AArch64 (HVC enabled)
	msr	hcr_el2, x2
	mov	x2, #3				// CNTHCTL_EL2.EL1PCTEN | EL1PCEN
	msr	cnthctl_el2, x2
	msr	cntvoff_el2, xzr
	mov	x2, #0x33ff			// CPTR_EL2: RES1 bits, no FP/SIMD traps
	msr	cptr_el2, x2
	msr	hstr_el2, xzr
	mrs	x2, midr_el1
	msr	vpidr_el2, x2
	mrs	x2, mpidr_el1
	msr	vmpidr_el2, x2
	msr	vttbr_el2, xzr
	ldr	x2, =0x30d00800			// SCTLR_EL1: RES1 bits, MMU/caches off
	msr	sctlr_el1, x2
	mov	x2, #0x3c5			// SPSR: EL1h, DAIF masked
	msr	spsr_el2, x2
	msr	elr_el2, x19
	mov	x0, x20
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	isb
	eret

1:	// Already at EL1: make sure the MMU and caches are off.
	mrs	x2, sctlr_el1
	bic	x2, x2, #(1 << 0)		// M
	bic	x2, x2, #(1 << 2)		// C
	bic	x2, x2, #(1 << 12)		// I
	msr	sctlr_el1, x2
	isb
	mov	x0, x20
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x19

/*
 * Secondary CPUs: the kernel's PSCI CPU_ON points here with context_id =
 * the kernel's reset vector (physical). PSCI may start the core at EL2
 * (TF-A) or at the caller's EL1, so take the same path as the boot CPU.
 * Runs without a stack; the kernel's reset vector ignores x0.
 */
	.globl	pieboot_secondary_entry
pieboot_secondary_entry:
	mov	x1, xzr
	b	pieboot_enter_kernel

/*
 * Spin-table release target for secondary CPUs started through the shim's
 * PSCI CPU_ON (psci.c). Entered at EL2 (or EL1) with the MMU off; the
 * per-CPU slot holds the PSCI entry point and context ID.
 */
	.globl	pieboot_spin_entry
pieboot_spin_entry:
	mrs	x1, mpidr_el1
	and	x1, x1, #0xffffff
	adrp	x2, psci_cpus
	add	x2, x2, :lo12:psci_cpus
	mov	x3, #PSCI_MAX_CPUS
1:	ldr	x4, [x2, #PSCI_CPU_MPIDR]
	cmp	x4, x1
	b.eq	2f
	add	x2, x2, #PSCI_CPU_SIZE
	subs	x3, x3, #1
	b.ne	1b
3:	wfe				// not ours: park
	b	3b
2:	ldr	x5, [x2, #PSCI_CPU_ENTRY]
	ldr	x0, [x2, #PSCI_CPU_CONTEXT]
	br	x5

/*
 * EL2 vectors: only synchronous exceptions from AArch64 EL1 (HVC) are
 * expected. Save the caller-saved registers, let psci.c handle the call
 * (x0-x3 in, x0 out), return.
 */
	.balign	2048
	.globl	pieboot_el2_vectors
pieboot_el2_vectors:
	.rept	8			// current EL with SP0/SPx: not expected
	b	el2_unexpected
	.balign	128
	.endr
	// lower EL, AArch64: synchronous
	b	el2_lower_sync
	.balign	128
	.rept	7			// IRQ/FIQ/SError, and lower EL AArch32
	b	el2_unexpected
	.balign	128
	.endr

el2_lower_sync:
	sub	sp, sp, #(24 * 8)
	stp	x0, x1, [sp, #0x00]
	stp	x2, x3, [sp, #0x10]
	stp	x4, x5, [sp, #0x20]
	stp	x6, x7, [sp, #0x30]
	stp	x8, x9, [sp, #0x40]
	stp	x10, x11, [sp, #0x50]
	stp	x12, x13, [sp, #0x60]
	stp	x14, x15, [sp, #0x70]
	stp	x16, x17, [sp, #0x80]
	stp	x18, x29, [sp, #0x90]
	str	x30, [sp, #0xa0]
	mov	x0, sp
	bl	pieboot_el2_sync
	ldp	x0, x1, [sp, #0x00]
	ldp	x2, x3, [sp, #0x10]
	ldp	x4, x5, [sp, #0x20]
	ldp	x6, x7, [sp, #0x30]
	ldp	x8, x9, [sp, #0x40]
	ldp	x10, x11, [sp, #0x50]
	ldp	x12, x13, [sp, #0x60]
	ldp	x14, x15, [sp, #0x70]
	ldp	x16, x17, [sp, #0x80]
	ldp	x18, x29, [sp, #0x90]
	ldr	x30, [sp, #0xa0]
	add	sp, sp, #(24 * 8)
	eret

el2_unexpected:
	mrs	x0, esr_el2
	mrs	x1, elr_el2
	bl	pieboot_el2_unexpected
4:	wfe
	b	4b

	.bss
	.balign	4096
el2_stacks:
	.space	4096 * (PSCI_MAX_CPUS + 1)

	.balign	16
boot_stack:
	.space	0x10000
boot_stack_top:
