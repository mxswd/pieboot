/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * PSCI provided by pieboot at EL2 for boards whose firmware only offers
 * spin tables (Pi 4 stock armstub, QEMU raspi4b). See psci.c.
 */
#ifndef PIEBOOT_PSCI_H
#define PIEBOOT_PSCI_H

#define PSCI_MAX_CPUS           8

/* struct psci_cpu layout, shared with start.S */
#define PSCI_CPU_MPIDR          0
#define PSCI_CPU_RELEASE        8
#define PSCI_CPU_ENTRY          16
#define PSCI_CPU_CONTEXT        24
#define PSCI_CPU_SIZE           32

#ifndef __ASSEMBLER__
#include "lib.h"
#include "fdt.h"

struct psci_cpu {
	uint64_t mpidr;                 /* Aff2..Aff0 */
	uint64_t release_addr;          /* spin-table cpu-release-addr; 0 = none */
	uint64_t entry;                 /* CPU_ON entry point */
	uint64_t context;               /* CPU_ON context ID (x0 at entry) */
};

extern struct psci_cpu psci_cpus[PSCI_MAX_CPUS];

/*
 * Set up the EL2 PSCI service if the firmware has no PSCI of its own and
 * the CPUs use spin tables. Returns true if the kernel should use it.
 */
bool psci_setup(const struct fdt *fdt, unsigned current_el);
#endif

#endif
