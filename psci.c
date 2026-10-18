/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * PSCI 0.2 subset served by pieboot at EL2 (HVC conduit), for boards whose
 * firmware parks secondary CPUs on spin tables instead of providing PSCI
 * (the Pi 4's stock armstub, QEMU raspi4b; later the Pi 3).
 *
 *   PSCI_VERSION       0.2
 *   CPU_ON             spin-table release of the target CPU at
 *                      pieboot_spin_entry, which jumps to the requested
 *                      entry point with x0 = context ID
 *   SYSTEM_RESET       BCM2835 PM watchdog, when the FDT has one
 *   SYSTEM_OFF         parks the calling CPU
 *
 * Everything else returns NOT_SUPPORTED.
 */

#include "psci.h"
#include "uart.h"

#define PSCI_VERSION            0x84000000u
#define PSCI_CPU_ON_32          0x84000003u
#define PSCI_CPU_ON_64          0xc4000003u
#define PSCI_SYSTEM_OFF         0x84000008u
#define PSCI_SYSTEM_RESET       0x84000009u

#define PSCI_SUCCESS            0
#define PSCI_NOT_SUPPORTED      (-1)
#define PSCI_INVALID_PARAMETERS (-2)

#define ESR_EC(esr)             (((esr) >> 26) & 0x3f)
#define ESR_EC_HVC64            0x16

/* BCM2835 power management / watchdog (Linux bcm2835_wdt.c) */
#define PM_RSTC                 0x1c
#define PM_WDOG                 0x24
#define PM_PASSWORD             0x5a000000u
#define PM_RSTC_WRCFG_MASK      0x00000030u
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020u

void pieboot_spin_entry(void);

struct psci_cpu psci_cpus[PSCI_MAX_CPUS];
static uint64_t pm_base;

struct el2_frame {
	uint64_t x[19];         /* x0-x18 */
	uint64_t x29, x30;
};

static inline uint64_t
read_esr_el2(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, esr_el2" : "=r"(v));
	return v;
}

static inline void
write32(uint64_t addr, uint32_t v)
{
	*(volatile uint32_t *)(uintptr_t)addr = v;
}

static inline uint32_t
read32(uint64_t addr)
{
	return *(volatile uint32_t *)(uintptr_t)addr;
}

static int64_t
psci_cpu_on(uint64_t mpidr, uint64_t entry, uint64_t context)
{
	for (unsigned i = 0; i < PSCI_MAX_CPUS; i++) {
		struct psci_cpu *c = &psci_cpus[i];
		if (!c->release_addr || c->mpidr != (mpidr & 0xffffff)) {
			continue;
		}
		c->entry = entry;
		c->context = context;
		/*
		 * EL2 runs with the MMU (and so the data cache) off, as does the
		 * spinning CPU: plain stores reach memory. Order the slot before
		 * the release, then wake the CPU.
		 */
		__asm__ volatile ("dsb sy" ::: "memory");
		*(volatile uint64_t *)(uintptr_t)c->release_addr = (uint64_t)(uintptr_t)pieboot_spin_entry;
		__asm__ volatile ("dsb sy; sev" ::: "memory");
		return PSCI_SUCCESS;
	}
	return PSCI_INVALID_PARAMETERS;
}

__attribute__((noreturn)) static void
psci_system_reset(void)
{
	if (pm_base) {
		write32(pm_base + PM_WDOG, PM_PASSWORD | 10);
		uint32_t rstc = read32(pm_base + PM_RSTC) & ~PM_RSTC_WRCFG_MASK;
		write32(pm_base + PM_RSTC, PM_PASSWORD | rstc | PM_RSTC_WRCFG_FULL_RESET);
	}
	for (;;) {
		__asm__ volatile ("wfi");
	}
}

void pieboot_el2_sync(struct el2_frame *f);

void
pieboot_el2_sync(struct el2_frame *f)
{
	uint64_t esr = read_esr_el2();

	if (ESR_EC(esr) != ESR_EC_HVC64) {
		printf("pieboot: unexpected EL2 trap, ESR 0x%lx\n", esr);
		f->x[0] = (uint64_t)PSCI_NOT_SUPPORTED;
		return;
	}

	int64_t ret;
	switch ((uint32_t)f->x[0]) {
	case PSCI_VERSION:
		ret = 2;                /* 0.2 */
		break;
	case PSCI_CPU_ON_32:
	case PSCI_CPU_ON_64:
		ret = psci_cpu_on(f->x[1], f->x[2], f->x[3]);
		break;
	case PSCI_SYSTEM_RESET:
		psci_system_reset();
		break;
	case PSCI_SYSTEM_OFF:
		for (;;) {
			__asm__ volatile ("wfi");
		}
	default:
		ret = PSCI_NOT_SUPPORTED;
		break;
	}
	f->x[0] = (uint64_t)ret;
}

void pieboot_el2_unexpected(uint64_t esr, uint64_t elr);

void
pieboot_el2_unexpected(uint64_t esr, uint64_t elr)
{
	printf("pieboot: unexpected exception at EL2, ESR 0x%lx ELR 0x%lx\n", esr, elr);
}

bool
psci_setup(const struct fdt *fdt, unsigned current_el)
{
	if (fdt_find_compatible(fdt->root, "arm,psci-1.0") ||
	    fdt_find_compatible(fdt->root, "arm,psci-0.2") ||
	    fdt_find_compatible(fdt->root, "arm,psci")) {
		return false;           /* the firmware's own PSCI */
	}
	if (current_el != 2) {
		return false;           /* HVC needs a resident EL2 */
	}

	struct fdt_node *cpus = fdt_path(fdt, "/cpus");
	unsigned n = 0;
	for (struct fdt_node *c = cpus ? cpus->child : NULL; c && n < PSCI_MAX_CPUS; c = c->sibling) {
		const char *type = fdt_prop_str(c, "device_type");
		const char *method = fdt_prop_str(c, "enable-method");
		struct fdt_prop *reg = fdt_prop(c, "reg");
		struct fdt_prop *rel = fdt_prop(c, "cpu-release-addr");
		if (!type || strcmp(type, "cpu") || !reg) {
			continue;
		}
		psci_cpus[n].mpidr = fdt_read_cells(reg->val, reg->len >= 8 ? 2 : 1) & 0xffffff;
		if (method && !strcmp(method, "spin-table") && rel) {
			psci_cpus[n].release_addr = fdt_read_cells(rel->val, rel->len / 4);
		}
		n++;
	}

	struct fdt_node *pm = fdt_find_compatible(fdt->root, "brcm,bcm2835-pm-wdt");
	if (!pm) {
		pm = fdt_find_compatible(fdt->root, "brcm,bcm2835-pm");
	}
	uint64_t size;
	if (pm && !fdt_reg(pm, 0, &pm_base, &size)) {
		pm_base = 0;
	}

	printf("pieboot: PSCI over HVC for %u spin-table CPU(s)%s\n", n,
	    pm_base ? ", reset via PM watchdog" : "");
	return true;
}
