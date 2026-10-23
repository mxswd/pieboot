/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * pieboot: start xnu from arm64 Linux-style boot firmware.
 *
 * Input:  x0 = FDT. The xnu kernel (and optionally a RAM disk and extra
 *         boot-args) arrives as the initrd: either a bare Mach-O kernel or
 *         a pieboot bundle (see mkbundle.py), which may hold one kernel per
 *         SoC; select_kernel() picks one by the device tree and the CPU.
 * Output: the kernel entered at EL1, MMU off, x0 = boot_args, with an Apple
 *         device tree describing the machine.
 *
 * Physical layout, as low in the first RAM range as it fits above this
 * image (place_kernel()):
 *
 *   physBase                     <- virtBase (kernel VA rounded down to 2MB)
 *   kernel segments              at physBase + (vmaddr - virtBase)
 *   Apple device tree
 *   RAM disk (optional)
 *   boot_args
 *   topOfKernelData; the kernel owns everything up to the end of RAM,
 *   the firmware's FDT and initrd included once it runs.
 */

#include "adt.h"
#include "boot_args.h"
#include "fdt.h"
#include "macho.h"
#include "uart.h"
#include "psci.h"
#include "mbox.h"
#include "fbcon.h"
#include "led.h"
#include "place.h"

#define SZ_16K           0x4000ULL
#define SZ_2M            0x200000ULL
/*
 * The kernel's bootstrap page tables (start.s) map it with L2 blocks, so
 * physBase and virtBase must agree modulo the L2 block size: 2 MB with 4K
 * pages, 32 MB with 16K pages. 32 MB serves both kernels.
 */
#define KERNEL_L2_BLOCK  0x2000000ULL
#define SOC_DEFAULT "arm64"

extern uint8_t __image_start[] __attribute__((visibility("hidden")));
extern uint8_t __image_end[] __attribute__((visibility("hidden")));

void pieboot_enter_kernel(uint64_t entry_pa, uint64_t boot_args_pa) __attribute__((noreturn));
void pieboot_secondary_entry(void);

/* --- self relocation ------------------------------------------------------- */

struct elf64_rela {
	uint64_t r_offset;
	uint64_t r_info;
	int64_t r_addend;
};

#define R_AARCH64_RELATIVE 1027

extern struct elf64_rela __rela_start[] __attribute__((visibility("hidden")));
extern struct elf64_rela __rela_end[] __attribute__((visibility("hidden")));

void pieboot_relocate(uint64_t base);

void
pieboot_relocate(uint64_t base)
{
	for (struct elf64_rela *r = __rela_start; r < __rela_end; r++) {
		if ((uint32_t)r->r_info == R_AARCH64_RELATIVE) {
			*(uint64_t *)(base + r->r_offset) = base + (uint64_t)r->r_addend;
		}
	}
}

/* --- bundle ------------------------------------------------------------------ */

#define BUNDLE_MAGIC "PIEBUNDL"

struct bundle_entry {
	char name[16];
	uint64_t offset;
	uint64_t size;
};

struct bundle_header {
	char magic[8];
	uint32_t version;
	uint32_t count;
	struct bundle_entry entries[];
};

#define MAX_KERNELS     8

struct payload {
	const uint8_t *kernel;
	size_t kernel_size;
	const uint8_t *ramdisk;
	size_t ramdisk_size;
	const char *bootargs;
	size_t bootargs_size;
	/* A version 2 bundle: its kernels and their table (mkbundle.py). */
	struct {
		char name[16];
		const uint8_t *image;
		size_t size;
	} kernels[MAX_KERNELS];
	unsigned nkernels;
	const char *table;
	size_t table_size;
};

static void
open_payload(const uint8_t *p, size_t size, struct payload *out)
{
	memset(out, 0, sizeof(*out));
	if (size >= 4 && *(const uint32_t *)p == 0xfeedfacf) {
		out->kernel = p;
		out->kernel_size = size;
		return;
	}
	const struct bundle_header *h = (const void *)p;
	if (size < sizeof(*h) || memcmp(h->magic, BUNDLE_MAGIC, 8) || h->version < 1 || h->version > 2) {
		panic_nokernel("initrd is neither a Mach-O kernel nor a pieboot bundle");
	}
	for (uint32_t i = 0; i < h->count; i++) {
		const struct bundle_entry *e = &h->entries[i];
		if (e->offset + e->size > size) {
			panic_nokernel("bundle entry %u out of range", i);
		}
		if (!strncmp(e->name, "kernel", sizeof(e->name))) {
			out->kernel = p + e->offset;
			out->kernel_size = e->size;
		} else if (!strncmp(e->name, "kernel.", 7) && out->nkernels < MAX_KERNELS) {
			memcpy(out->kernels[out->nkernels].name, e->name + 7, sizeof(e->name) - 7);
			out->kernels[out->nkernels].image = p + e->offset;
			out->kernels[out->nkernels].size = e->size;
			out->nkernels++;
		} else if (!strncmp(e->name, "kernels", sizeof(e->name))) {
			out->table = (const char *)p + e->offset;
			out->table_size = e->size;
		} else if (!strncmp(e->name, "ramdisk", sizeof(e->name))) {
			out->ramdisk = p + e->offset;
			out->ramdisk_size = e->size;
		} else if (!strncmp(e->name, "bootargs", sizeof(e->name))) {
			out->bootargs = (const char *)p + e->offset;
			out->bootargs_size = e->size;
		}
	}
	if (!out->kernel && !out->nkernels) {
		panic_nokernel("bundle has no kernel");
	}
}

/* --- machine description ----------------------------------------------------- */

#define MAX_RAM_RANGES  8

struct machine {
	struct fdt fdt;
	uint64_t ram_base, ram_size;    /* span of all RAM ranges */
	struct range { uint64_t base, end; } ram[MAX_RAM_RANGES];
	unsigned nram;                  /* sorted, merged /memory ranges */
	uint64_t initrd_start, initrd_end;
	uint64_t timebase;
	uint64_t boot_mpidr;
	struct fdt_node *uart;
	struct fdt_node *gic;           /* GICv2 distributor, if any */
	bool shim_psci;                 /* PSCI served by pieboot at EL2 (psci.c) */
	uint64_t io_base, io_end;
	uint32_t next_phandle;
	uint32_t uart_phandle;
	uint64_t panic_pa, panic_size;  /* the panic region (/pram), if any */
	uint64_t pool_pa, pool_size;    /* the display pool (carve_display_pool), if any */
};

/*
 * A /defaults pmap-io-ranges entry (xnu's pmap_io_range_t): physical memory
 * the kernel does not manage, and how it maps it (VM_WIMG_*, xnu's
 * osfmk/arm/memory_types.h; without an entry, as a device's).
 */
struct pmap_io_range {
	uint64_t addr;
	uint64_t len;
	uint32_t wimg;
	uint32_t signature;
};
#define VM_WIMG_WCOMB           0x6     /* write-combined: normal memory, not cached */

/* The panic region at the top of RAM: iBoot's kMemoryRegion_Panic. */
#define PANIC_REGION_SIZE       (512 * 1024)
/* How long a previous boot's panic log stays on the screen. */
#define PANIC_PAUSE_SECONDS     15U

/*
 * The start of the header the kernel writes at the start of the panic
 * region (struct embedded_panic_header in xnu's osfmk/kern/debug.h, which
 * is packed; its flags are 64 bits).
 */
#define EMBEDDED_PANIC_MAGIC    0x46554E4B      /* FUNK */
struct panic_header {
	uint32_t magic;
	uint32_t crc;
	uint32_t version;
	uint64_t flags;
	uint32_t panic_log_offset;
	uint32_t panic_log_len;
} __attribute__((packed));

/*
 * Clean and invalidate the data cache lines of [start, end) to the point of
 * coherency. pieboot runs with the MMU off (its stores bypass the caches),
 * and xnu turns the caches on without invalidating them first (iBoot
 * leaves them clean): lines the firmware left for this memory, stale or
 * dirty, must not outlive pieboot's writes there.
 */
static void
dcache_clean_invalidate(uint64_t start, uint64_t end)
{
	uint64_t ctr;
	__asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
	uint64_t line = 4ULL << ((ctr >> 16) & 0xf);    /* DminLine, in words */
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint64_t a = start & ~(line - 1); a < end; a += line) {
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Print the panic log a previous boot left in the panic region (as text on the screen). */
static void
show_previous_panic(const struct machine *m)
{
	const struct panic_header *h = (const void *)(uintptr_t)m->panic_pa;
	if (h->magic != EMBEDDED_PANIC_MAGIC || h->panic_log_len == 0 ||
	    h->panic_log_offset >= m->panic_size ||
	    h->panic_log_len > m->panic_size - h->panic_log_offset) {
		return;
	}
	bool pie = fbcon_text();
	printf("pieboot: the previous boot panicked; its panic log:\n");
	const char *log = (const char *)h + h->panic_log_offset;
	for (uint32_t i = 0; i < h->panic_log_len && log[i]; i++) {
		uart_putc(log[i]);
	}
	printf("\npieboot: end of the previous panic log\n");
	/*
	 * Left in place, unlike iBoot, which extracts it: the kernel keeps it
	 * for user space (xnu's kern.previous_panic) and then clears the region.
	 */
	/* Time to read it (or photograph the screen) before booting again. */
	printf("pieboot: booting again in %u seconds\n", PANIC_PAUSE_SECONDS);
	uint64_t until = read_cntpct() + read_cntfrq() * PANIC_PAUSE_SECONDS;
	while (read_cntpct() < until) {
	}
	if (pie) {
		fbcon_pie();
	}
}

static inline uint64_t
read_sysreg_mpidr(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(v));
	return v;
}

static inline unsigned
current_el(void)
{
	uint64_t v;
	__asm__ volatile ("mrs %0, CurrentEL" : "=r"(v));
	return (unsigned)(v >> 2) & 3;
}

/*
 * A framebuffer the firmware set up and described in the device tree
 * (simple-framebuffer, 32 or 16 bits per pixel), for the kernel's console.
 */
static bool
dt_framebuffer(const struct fdt *fdt, struct Boot_Video *video)
{
	struct fdt_node *n = fdt_find_compatible(fdt->root, "simple-framebuffer");
	const char *status = n ? fdt_prop_str(n, "status") : NULL;
	const char *format = n ? fdt_prop_str(n, "format") : NULL;
	struct fdt_prop *reg = n ? fdt_prop(n, "reg") : NULL;
	uint32_t width, height, stride;
	if (!n || (status && strcmp(status, "okay") && strcmp(status, "ok")) || !reg || !format ||
	    (strcmp(format, "a8r8g8b8") && strcmp(format, "x8r8g8b8") && strcmp(format, "r5g6b5")) ||
	    !fdt_prop_u32(n, "width", &width) || !fdt_prop_u32(n, "height", &height) ||
	    !fdt_prop_u32(n, "stride", &stride)) {
		return false;
	}
	uint64_t pa, size;
	if (!fdt_reg(n, 0, &pa, &size)) {
		/* Under /chosen, which has no ranges: the address as it is. */
		unsigned ac = fdt_address_cells(n->parent);
		if (reg->len < 4 * ac) {
			return false;
		}
		pa = fdt_read_cells(reg->val, ac);
	}
	video->v_baseAddr = pa;
	video->v_rowBytes = stride;
	video->v_width = width;
	video->v_height = height;
	video->v_depth = strcmp(format, "r5g6b5") ? 32 : 16;
	return true;
}

/* --- kernel selection -------------------------------------------------------- */

/* Whether the CPU has a feature a kernel needs (mkbundle.py's names). */
static bool
cpu_has(const char *f, size_t len)
{
	uint64_t v;
	if (len == 3 && !strncmp(f, "16k", 3)) {
		__asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(v));
		unsigned tgran16 = (unsigned)(v >> 20) & 0xf;       /* 1, or 2 with 52-bit addresses */
		return tgran16 == 1 || tgran16 == 2;
	}
	if (len == 3 && !strncmp(f, "lse", 3)) {
		__asm__ volatile ("mrs %0, id_aa64isar0_el1" : "=r"(v));
		return ((v >> 20) & 0xf) >= 2;
	}
	if (len == 3 && !strncmp(f, "pan", 3)) {
		__asm__ volatile ("mrs %0, id_aa64mmfr1_el1" : "=r"(v));
		return ((v >> 20) & 0xf) >= 1;
	}
	return false;
}

/* Whether the root node's compatible list has the string s[0..len). */
static bool
root_compatible(const struct fdt *fdt, const char *s, size_t len)
{
	struct fdt_prop *c = fdt_prop(fdt->root, "compatible");
	for (size_t i = 0; c && i < c->len;) {
		const char *e = (const char *)c->val + i;
		size_t n = strlen(e);
		if (n == len && !strncmp(e, s, len)) {
			return true;
		}
		i += n + 1;
	}
	return false;
}

/* The value of "key=" in boot-args, copied (up to a space) into out. */
static bool
bootarg(const char *args, size_t size, const char *key, char *out, size_t outlen)
{
	size_t klen = strlen(key);
	for (size_t i = 0; args && i + klen < size && args[i]; i++) {
		if ((i > 0 && args[i - 1] != ' ') || strncmp(args + i, key, klen)) {
			continue;
		}
		size_t n = 0;
		for (const char *p = args + i + klen; *p && *p != ' ' && n + 1 < outlen; p++) {
			out[n++] = *p;
		}
		out[n] = '\0';
		return true;
	}
	return false;
}

/* Whether boot-args have the flag (say "-v") as a word of its own. */
static bool
bootflag(const char *args, size_t size, const char *flag)
{
	size_t len = strlen(flag);
	for (size_t i = 0; args && i + len <= size && args[i]; i++) {
		if ((i == 0 || args[i - 1] == ' ') && !strncmp(args + i, flag, len) &&
		    (i + len == size || args[i + len] == ' ' || args[i + len] == '\0')) {
			return true;
		}
	}
	return false;
}

/*
 * A version 2 bundle's kernel for this machine: the one the boot-arg
 * pieboot-kernel=NAME names, else the first in the table with a root
 * compatible of this device tree and every CPU feature it needs, else the
 * table's default.
 */
static void
select_kernel(const struct fdt *fdt, struct payload *pl, const char *fdt_args)
{
	if (!pl->nkernels) {
		return;
	}
	char want[16] = "", dflt[16] = "", chosen[16] = "";
	const char *why = NULL;
	if (bootarg(fdt_args, fdt_args ? strlen(fdt_args) + 1 : 0, "pieboot-kernel=", want, sizeof(want)) ||
	    bootarg(pl->bootargs, pl->bootargs_size, "pieboot-kernel=", want, sizeof(want))) {
		strlcpy(chosen, want, sizeof(chosen));
		why = "the boot-arg pieboot-kernel";
	}
	/* Table lines: NAME FEATURES COMPAT..., then "default NAME". */
	const char *t = pl->table, *end = pl->table ? pl->table + pl->table_size : NULL;
	while (t && t < end && *t) {
		const char *eol = t;
		while (eol < end && *eol && *eol != '\n') {
			eol++;
		}
		const char *tok[16];
		size_t toklen[16];
		unsigned ntok = 0;
		for (const char *p = t; p < eol && ntok < 16;) {
			while (p < eol && *p == ' ') {
				p++;
			}
			const char *q = p;
			while (q < eol && *q != ' ') {
				q++;
			}
			if (q > p) {
				tok[ntok] = p;
				toklen[ntok++] = (size_t)(q - p);
			}
			p = q;
		}
		t = eol < end && *eol ? eol + 1 : end;
		if (ntok == 2 && toklen[0] == 7 && !strncmp(tok[0], "default", 7)) {
			size_t n = toklen[1] < sizeof(dflt) - 1 ? toklen[1] : sizeof(dflt) - 1;
			memcpy(dflt, tok[1], n);
			dflt[n] = '\0';
			continue;
		}
		if (ntok < 3 || chosen[0]) {
			continue;
		}
		const char *compat = NULL;
		size_t compat_len = 0;
		for (unsigned i = 2; i < ntok && !compat; i++) {
			if (root_compatible(fdt, tok[i], toklen[i])) {
				compat = tok[i];
				compat_len = toklen[i];
			}
		}
		if (!compat) {
			continue;
		}
		bool ok = true;
		if (!(toklen[1] == 1 && tok[1][0] == '-')) {
			for (const char *f = tok[1]; f < tok[1] + toklen[1];) {
				const char *fe = f;
				while (fe < tok[1] + toklen[1] && *fe != ',') {
					fe++;
				}
				if (!cpu_has(f, (size_t)(fe - f))) {
					printf("pieboot: kernel %.*s fits %.*s, but the CPU lacks %.*s\n", (int)toklen[0], tok[0],
					    (int)compat_len, compat, (int)(fe - f), f);
					ok = false;
				}
				f = fe + 1;
			}
		}
		if (ok) {
			size_t n = toklen[0] < sizeof(chosen) - 1 ? toklen[0] : sizeof(chosen) - 1;
			memcpy(chosen, tok[0], n);
			chosen[n] = '\0';
			why = "the device tree";
		}
	}
	if (!chosen[0]) {
		strlcpy(chosen, dflt[0] ? dflt : pl->kernels[0].name, sizeof(chosen));
		why = "the default";
	}
	for (unsigned i = 0; i < pl->nkernels; i++) {
		if (!strncmp(pl->kernels[i].name, chosen, sizeof(pl->kernels[i].name))) {
			pl->kernel = pl->kernels[i].image;
			pl->kernel_size = pl->kernels[i].size;
			printf("pieboot: kernel %s (%s; %u in the bundle)\n", chosen, why, pl->nkernels);
			return;
		}
	}
	panic_nokernel("bundle has no kernel %s", chosen);
}

static uint64_t
chosen_addr(struct fdt_node *chosen, const char *name)
{
	struct fdt_prop *p = fdt_prop(chosen, name);

	if (!p) {
		return 0;
	}
	return p->len == 8 ? be64(p->val) : be32(p->val);
}

static struct fdt_node *
find_console(struct machine *m)
{
	struct fdt_node *chosen = fdt_path(&m->fdt, "/chosen");
	const char *path = fdt_prop_str(chosen, "stdout-path");

	if (path) {
		char buf[128];
		strlcpy(buf, path, sizeof(buf));
		char *colon = strchr(buf, ':');
		if (colon) {
			*colon = 0;
		}
		struct fdt_node *n = NULL;
		if (buf[0] == '/') {
			n = fdt_path(&m->fdt, buf);
		} else {
			const char *alias = fdt_prop_str(fdt_path(&m->fdt, "/aliases"), buf);
			n = alias ? fdt_path(&m->fdt, alias) : NULL;
		}
		if (n && fdt_is_compatible(n, "arm,pl011")) {
			return n;
		}
	}
	return fdt_find_compatible(m->fdt.root, "arm,pl011");
}

static bool
is_toplevel_skipped(const struct fdt_node *n)
{
	char name[64];

	fdt_basename(n, name, sizeof(name));
	return !strcmp(name, "chosen") || !strcmp(name, "aliases") || !strcmp(name, "cpus") ||
	       !strcmp(name, "memory") || !strcmp(name, "reserved-memory") ||
	       !strcmp(name, "__symbols__") || !strcmp(name, "__overrides__");
}

/* Lowest/highest physical address of memory-mapped devices. */
static void
scan_io_range(struct machine *m, const struct fdt_node *n)
{
	for (unsigned i = 0; i < fdt_reg_count(n); i++) {
		uint64_t pa, size;
		if (fdt_reg(n, i, &pa, &size) && pa) {
			m->io_base = MIN(m->io_base, pa);
			m->io_end = MAX(m->io_end, pa + size);
		}
	}
	for (const struct fdt_node *c = n->child; c; c = c->sibling) {
		scan_io_range(m, c);
	}
}

/* --- Apple device tree ---------------------------------------------------------- */

static uint32_t
node_phandle(struct machine *m, const struct fdt_node *n)
{
	uint32_t ph = fdt_phandle(n);
	return ph ? ph : m->next_phandle++;
}

/*
 * Interrupt properties are lists of u32 cells that IOKit's DT interrupt
 * mapping (IODeviceTreeSupport.cpp) reads in native byte order.
 */
static bool
is_irq_cell_prop(const char *name)
{
	return !strcmp(name, "interrupts") || !strcmp(name, "interrupt-parent") ||
	       !strcmp(name, "#interrupt-cells") || !strcmp(name, "interrupt-map") ||
	       !strcmp(name, "interrupt-map-mask");
}

static const void *
cells_to_native(const uint8_t *val, uint32_t len)
{
	uint32_t *out = balloc(len ? len : 4);
	for (uint32_t i = 0; i < len / 4; i++) {
		out[i] = be32(val + 4 * i);
	}
	return out;
}

static bool
is_gic(const struct fdt_node *n)
{
	return n && (fdt_is_compatible(n, "arm,gic-400") || fdt_is_compatible(n, "arm,cortex-a15-gic") ||
	       fdt_is_compatible(n, "arm,cortex-a9-gic") || fdt_is_compatible(n, "arm,gic-v2"));
}

/* GIC specifier { type, number, flags } -> GIC interrupt ID. */
static uint32_t
gic_intid(const uint8_t *spec)
{
	uint32_t type = be32(spec), num = be32(spec + 4);
	return type == 1 ? num + 16 : num + 32;     /* PPI : SPI */
}

/*
 * IOKit's interrupt controllers take the first specifier cell as the
 * vector number (Apple's AIC uses one cell), so GIC specifiers become
 * one cell each: the interrupt ID. The GIC node gets #interrupt-cells 1.
 */
static const void *
gic_specs_to_intids(const struct fdt_prop *p, uint32_t *out_len)
{
	uint32_t n = p->len / 12;
	uint32_t *out = balloc(n ? n * 4 : 4);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = gic_intid(p->val + 12 * i);
	}
	*out_len = n * 4;
	return out;
}

static uint32_t
irq_parent(const struct fdt_node *n)
{
	for (; n; n = n->parent) {
		uint32_t ip;
		if (fdt_prop_u32(n, "interrupt-parent", &ip)) {
			return ip;
		}
	}
	return 0;
}

/*
 * The rate of a device's first clock: a fixed-clock's frequency, a firmware
 * clock's rate from the mailbox; else (the Pi 4's cprman clocks, which
 * only the firmware knows) the firmware clock for the SD controllers.
 */
static uint32_t
clock_rate(struct machine *m, const struct fdt_node *n)
{
	struct fdt_prop *clocks = fdt_prop(n, "clocks");
	if (clocks && clocks->len >= 4) {
		struct fdt_node *provider = fdt_find_phandle(m->fdt.root, fdt_read_cells(clocks->val, 1));
		uint32_t cells = 0, rate = 0;
		if (provider) {
			fdt_prop_u32(provider, "#clock-cells", &cells);
		}
		if (provider && fdt_is_compatible(provider, "fixed-clock") &&
		    fdt_prop_u32(provider, "clock-frequency", &rate)) {
			return rate;
		}
		if (provider && fdt_is_compatible(provider, "raspberrypi,firmware-clocks") && cells == 1 &&
		    clocks->len >= 8) {
			return mbox_clock_rate((uint32_t)fdt_read_cells(clocks->val + 4, 1));
		}
	}
	uint32_t clock_id = fdt_is_compatible(n, "brcm,bcm2711-emmc2") ? MBOX_CLOCK_EMMC2 :
	    fdt_is_compatible(n, "brcm,bcm2835-sdhci") ? MBOX_CLOCK_EMMC : 0;
	return clock_id ? mbox_clock_rate(clock_id) : 0;
}

/* Convert an FDT device node (and its children) under the Apple /arm-io node. */
static void
convert_device(struct machine *m, const struct fdt_node *n, struct adt_node *parent)
{
	char name[64];
	fdt_basename(n, name, sizeof(name));
	struct adt_node *a = adt_new(parent, name);

	for (const struct fdt_prop *p = n->props; p; p = p->next) {
		if (!strcmp(p->name, "name") || !strcmp(p->name, "phandle") ||
		    !strcmp(p->name, "linux,phandle") || !strcmp(p->name, "reg") ||
		    !strcmp(p->name, "#address-cells") || !strcmp(p->name, "#size-cells")) {
			continue;
		}
		if (!strcmp(p->name, "interrupts") && p->len % 12 == 0 &&
		    is_gic(fdt_find_phandle(m->fdt.root, irq_parent(n)))) {
			uint32_t len;
			const void *intids = gic_specs_to_intids(p, &len);
			adt_set(a, "interrupts", intids, len);
			continue;
		}
		if (!strcmp(p->name, "#interrupt-cells") && is_gic(n)) {
			adt_set_u32(a, "#interrupt-cells", 1);
			continue;
		}
		if (is_irq_cell_prop(p->name)) {
			adt_set(a, p->name, cells_to_native(p->val, p->len), p->len & ~3u);
			continue;
		}
		/*
		 * Children's "reg" below are already translated to /arm-io
		 * offsets, so a bus maps them 1:1 (empty ranges) for
		 * IODTResolveAddressing. Its own map stays as "fdt,ranges".
		 */
		if (!strcmp(p->name, "ranges")) {
			adt_set(a, "ranges", NULL, 0);
			adt_set(a, "fdt,ranges", p->val, p->len);
			continue;
		}
		/* Other cell properties stay in FDT (big-endian) encoding. */
		adt_set(a, p->name, p->val, p->len);
	}

	/*
	 * FDT nodes inherit interrupt-parent from their ancestors; IOKit only
	 * looks at the node itself and otherwise walks to the tree parent.
	 */
	if (fdt_prop(n, "interrupts") && !fdt_prop(n, "interrupt-parent")) {
		uint32_t ip = irq_parent(n);
		if (ip) {
			adt_set_u32(a, "interrupt-parent", ip);
		}
	}
	/*
	 * interrupt-map entries embed the parent's #address-cells, so keep the
	 * FDT value on leaf interrupt nexuses (nodes with children get 2 below).
	 */
	uint32_t acells;
	if (!n->child && fdt_prop_u32(n, "#address-cells", &acells)) {
		adt_set_u32(a, "#address-cells", acells);
	}

	/*
	 * A device's clock rate, which drivers need where the hardware does not
	 * report it (SD controllers' base clocks): its first clock, when that
	 * is a fixed-clock or a firmware clock.
	 */
	uint32_t rate = 0;
	if (!fdt_prop(n, "clock-frequency")) {
		rate = clock_rate(m, n);
		if (rate) {
			adt_set_u32(a, "clock-frequency", rate);
		}
	}

	/*
	 * How the device's bus reaches RAM by DMA ({ CPU address, bus address,
	 * size } u64 triples), when not at the CPU's addresses.
	 */
	struct fdt_dma_range dr[8];
	unsigned ndr = fdt_dma_ranges(n, dr, 8);
	bool identity = true;
	for (unsigned i = 0; i < ndr; i++) {
		identity = identity && dr[i].cpu == dr[i].bus;
	}
	if (ndr && !identity) {
		adt_set(a, "pieboot,dma-ranges", dr, ndr * sizeof(dr[0]));
	}

	uint32_t phandle = node_phandle(m, n);
	adt_set_u32(a, "AAPL,phandle", phandle);
	if (n == m->uart) {
		m->uart_phandle = phandle;
	}

	/*
	 * Apple convention: "reg" is a list of { u64 offset from the /arm-io
	 * base, u64 size } in native byte order.
	 */
	unsigned nreg = fdt_reg_count(n);
	if (nreg) {
		uint64_t *regs = balloc(nreg * 16);
		unsigned ok = 0;
		for (unsigned i = 0; i < nreg && ok == i; i++) {
			uint64_t pa, size;
			if (fdt_reg(n, i, &pa, &size)) {
				regs[2 * i] = pa - m->io_base;
				regs[2 * i + 1] = size;
				ok++;
			}
		}
		if (ok) {
			adt_set(a, "reg", regs, ok * 16);       /* those the CPU can reach, in order */
		}
		if (ok < nreg) {
			/*
			 * Some are not CPU addresses (the BCM2712 MIP's MSI doorbell is a
			 * PCI one): all of them as { u64 address on the parent bus, u64
			 * size }, for the driver.
			 */
			struct fdt_prop *reg = fdt_prop(n, "reg");
			unsigned ac = fdt_address_cells(n->parent), sc = fdt_size_cells(n->parent);
			uint64_t *bus = balloc(nreg * 16);
			for (unsigned i = 0; i < nreg; i++) {
				bus[2 * i] = fdt_read_cells(reg->val + i * 4 * (ac + sc), ac);
				bus[2 * i + 1] = fdt_read_cells(reg->val + i * 4 * (ac + sc) + 4 * ac, sc);
			}
			adt_set(a, "pieboot,bus-reg", bus, nreg * 16);
			adt_set(a, "fdt,reg", reg->val, reg->len);
		}
	}
	if (n->child) {
		adt_set_u32(a, "#address-cells", 2);
		adt_set_u32(a, "#size-cells", 2);
	}
	for (const struct fdt_node *c = n->child; c; c = c->sibling) {
		convert_device(m, c, a);
	}
}

static void
build_cpus(struct machine *m, struct adt_node *root)
{
	struct adt_node *cpus = adt_new(root, "cpus");
	struct fdt_node *fcpus = fdt_path(&m->fdt, "/cpus");
	unsigned index = 0;

	adt_set_u32(cpus, "#address-cells", 1);
	adt_set_u32(cpus, "#size-cells", 0);

	/*
	 * AppleARMSMP wants an interrupt on each CPU nub (on Apple SoCs, the
	 * PMI). Use the PMU interrupt: one PPI for all, or one SPI per CPU.
	 */
	struct fdt_prop *pmu_irqs = NULL;
	struct fdt_node *pmu = fdt_find_compatible(m->fdt.root, "arm,armv8-pmuv3");
	if (!pmu) {
		pmu = fdt_find_compatible(m->fdt.root, "arm,cortex-a72-pmu");
	}
	if (pmu && is_gic(fdt_find_phandle(m->fdt.root, irq_parent(pmu)))) {
		pmu_irqs = fdt_prop(pmu, "interrupts");
	}
	for (struct fdt_node *c = fcpus ? fcpus->child : NULL; c; c = c->sibling) {
		const char *type = fdt_prop_str(c, "device_type");
		struct fdt_prop *reg = fdt_prop(c, "reg");
		if (!type || strcmp(type, "cpu") || !reg) {
			continue;
		}
		uint64_t mpidr = fdt_read_cells(reg->val, reg->len >= 8 ? 2 : 1);
		char name[16];
		snprintf(name, sizeof(name), "cpu%u", index);
		struct adt_node *a = adt_new(cpus, name);
		adt_set_str(a, "device_type", "cpu");
		adt_set_u32(a, "reg", (uint32_t)(mpidr & 0xffffff));
		adt_set_u32(a, "cpu-id", index);
		adt_set_u32(a, "timebase-frequency", (uint32_t)m->timebase);
		adt_set_str(a, "state", (mpidr & 0xffffff) == (m->boot_mpidr & 0xffffff) ? "running" : "waiting");
		struct fdt_prop *p;
		if ((p = fdt_prop(c, "compatible"))) {
			adt_set(a, "compatible", p->val, p->len);
		}
		if ((p = fdt_prop(c, "enable-method"))) {
			adt_set(a, "enable-method", p->val, p->len);
		}
		if ((p = fdt_prop(c, "cpu-release-addr"))) {
			adt_set_u64(a, "cpu-release-addr", fdt_read_cells(p->val, p->len / 4));
		}
		adt_set_u32(a, "AAPL,phandle", node_phandle(m, c));
		if (m->gic) {
			uint32_t intid = 16 + 7;        /* PMU PPI by convention */
			if (pmu_irqs && pmu_irqs->len >= 12) {
				uint32_t n = pmu_irqs->len / 12;
				intid = gic_intid(pmu_irqs->val + 12 * (n > index ? index : 0));
			}
			adt_set_u32(a, "interrupt-parent", node_phandle(m, m->gic));
			adt_set_u32(a, "interrupts", intid);
		}
		index++;
	}
	if (!index) {
		panic("no CPUs in the FDT");
	}
	printf("pieboot: %u CPU(s), timebase %lu Hz\n", index, m->timebase);
}

static struct adt_node *
build_adt(struct machine *m, uint64_t ramdisk_pa, uint64_t ramdisk_size)
{
	struct fdt_node *froot = m->fdt.root;
	struct fdt_node *fchosen = fdt_path(&m->fdt, "/chosen");
	struct adt_node *root = adt_new(NULL, "device-tree");
	struct fdt_prop *p;

	adt_set_u32(root, "#address-cells", 2);
	adt_set_u32(root, "#size-cells", 2);
	if ((p = fdt_prop(froot, "compatible"))) {
		adt_set(root, "compatible", p->val, p->len);
	}
	if ((p = fdt_prop(froot, "model"))) {
		adt_set(root, "model", p->val, p->len);
	}

	/* /chosen */
	struct adt_node *chosen = adt_new(root, "chosen");
	/*
	 * As on Apple hardware, the DRAM span includes the panic region above
	 * the memory the kernel manages: the kernel then maps the region as
	 * normal memory, not as a device, and the panic header's unaligned
	 * (packed) fields can be read.
	 */
	uint64_t dram_end = m->ram_base + m->ram_size;
	if (m->panic_size) {
		dram_end = MAX(dram_end, m->panic_pa + m->panic_size);
	}
	adt_set_u64(chosen, "dram-base", m->ram_base);
	adt_set_u64(chosen, "dram-size", dram_end - m->ram_base);
	/*
	 * Gaps between RAM ranges inside the managed span, as { u64 base,
	 * u64 size } pairs: the kernel keeps them out of the VM and maps them
	 * only as I/O (osfmk/arm64/arm_vm_init.c).
	 */
	if (m->nram > 1) {
		uint64_t holes[2 * (MAX_RAM_RANGES - 1)];
		for (unsigned i = 1; i < m->nram; i++) {
			holes[2 * (i - 1)] = m->ram[i - 1].end;
			holes[2 * (i - 1) + 1] = m->ram[i].base - m->ram[i - 1].end;
		}
		adt_set(chosen, "dram-holes", holes, 16 * (m->nram - 1));
	}
	adt_set_u32(chosen, "debug-enabled", 1);
	adt_set_str(chosen, "firmware-version", "pieboot");
	uint8_t seed[256];     /* xnu requests 4 x 64 bytes (prng_random.c) */
	uint64_t mix = read_cntpct() ^ 0x9e3779b97f4a7c15ULL;
	struct fdt_prop *fseed = fdt_prop(fchosen, "rng-seed");
	struct fdt_prop *kseed = fdt_prop(fchosen, "kaslr-seed");
	for (unsigned i = 0; i < sizeof(seed); i++) {
		mix ^= mix << 13;
		mix ^= mix >> 7;
		mix ^= mix << 17;
		seed[i] = (uint8_t)mix;
		if (fseed && fseed->len) {
			seed[i] ^= fseed->val[i % fseed->len];
		}
		if (kseed && kseed->len) {
			seed[i] ^= kseed->val[i % kseed->len];
		}
	}
	adt_set(chosen, "random-seed", seed, sizeof(seed));
	/*
	 * NVRAM geometry (IONVRAM.cpp): one empty bank and no proxy image, so
	 * NVRAM starts empty and lives in memory.
	 */
	adt_set_u32(chosen, "nvram-bank-size", 0x2000);
	adt_set_u32(chosen, "nvram-bank-count", 1);
	adt_set_u32(chosen, "nvram-current-bank", 0);
	/* No CTRR: kernel text is never locked down (ml_unsafe_kernel_text). */
	adt_set_u32(chosen, "kernel-ctrr-to-be-enabled", 0);
	/* PSCI CPU_ON entry point for secondary CPUs (start.S). */
	adt_set_u64(chosen, "pieboot,secondary-entry", (uint64_t)(uintptr_t)pieboot_secondary_entry);
	/*
	 * DEVELOPMENT kernels panic when interrupts stay masked or preemption
	 * stays disabled longer than a timeout in timer ticks, sized for
	 * Apple's 24 MHz timer (35 ms): with the Pi's 54 MHz timer (QEMU's
	 * 62.5 MHz) it is under 16 ms, and the emulator trips it. Trace only.
	 */
	struct adt_node *timeouts = adt_new(chosen, "machine-timeouts");
	adt_set_u32(timeouts, "interrupt-masked-debug-mode", 1);        /* SCHED_HYGIENE_MODE_TRACE */
	adt_set_u32(timeouts, "sched-preemption-disable-mode", 1);
	struct adt_node *memmap = adt_new(chosen, "memory-map");
	if (ramdisk_size) {
		uint64_t range[2] = { ramdisk_pa, ramdisk_size };
		adt_set(memmap, "RAMDisk", range, sizeof(range));
	}
	/*
	 * What the kernel was placed around (place_kernel()), for the record,
	 * as { u64 base, u64 size } pairs: this image, which stays; the FDT
	 * and the initrd, free memory once the kernel runs; the FDT's
	 * /memreserve/ entries.
	 */
	uint64_t range[2] = { (uint64_t)(uintptr_t)__image_start, (uint64_t)(__image_end - __image_start) };
	adt_set(chosen, "pieboot,image", range, sizeof(range));
	range[0] = m->fdt.pa;
	range[1] = m->fdt.size;
	adt_set(chosen, "pieboot,fdt", range, sizeof(range));
	range[0] = m->initrd_start;
	range[1] = m->initrd_end - m->initrd_start;
	adt_set(chosen, "pieboot,initrd", range, sizeof(range));
	if (m->fdt.nreserves) {
		adt_set(chosen, "pieboot,memreserve", m->fdt.reserves, m->fdt.nreserves * sizeof(m->fdt.reserves[0]));
	}
	/* The display pool (carve_display_pool), below the kernel, which the IOSurface kext hands out. */
	if (m->pool_size) {
		range[0] = m->pool_pa;
		range[1] = m->pool_size;
		adt_set(chosen, "pieboot,display-pool", range, sizeof(range));
	}

	build_cpus(m, root);

	/* /arm-io: every memory-mapped device, reg relative to io_base */
	struct adt_node *armio = adt_new(root, "arm-io");
	const char *soc = SOC_DEFAULT;
	if ((p = fdt_prop(froot, "compatible")) && p->len) {
		/* the last (most generic) compatible, without vendor prefix */
		const char *last = (const char *)p->val;
		for (uint32_t off = 0; off < p->len; off += strnlen((const char *)p->val + off, p->len - off) + 1) {
			last = (const char *)p->val + off;
		}
		const char *comma = strchr(last, ',');
		soc = comma ? comma + 1 : last;
	}
	adt_set_str(armio, "device_type", soc);
	adt_set_u32(armio, "#address-cells", 2);
	adt_set_u32(armio, "#size-cells", 2);
	uint64_t ranges[3] = { 0, m->io_base, m->io_end - m->io_base };
	adt_set(armio, "ranges", ranges, sizeof(ranges));
	for (struct fdt_node *c = froot->child; c; c = c->sibling) {
		if (!is_toplevel_skipped(c)) {
			convert_device(m, c, armio);
		}
	}

	/* /options: NVRAM variables (IODTNVRAM); empty, without a backing store. */
	adt_new(root, "options");
	/* The panic region (pexpert/arm/pe_init.c): /pram reg = { pa, size }. */
	if (m->panic_size) {
		struct adt_node *pram = adt_new(root, "pram");
		uint64_t reg[2] = { m->panic_pa, m->panic_size };
		adt_set(pram, "reg", reg, sizeof(reg));
		adt_set_u32(chosen, "embedded-panic-log-size", (uint32_t)m->panic_size);
	}

	/* PSCI served by pieboot itself: describe it like a firmware /psci node. */
	if (m->shim_psci) {
		struct adt_node *psci = adt_new(armio, "psci");
		static const char compat[] = "arm,psci-0.2";
		adt_set(psci, "compatible", compat, sizeof(compat));
		adt_set_str(psci, "method", "hvc");
		adt_set_u32(psci, "AAPL,phandle", m->next_phandle++);
	}

	/* /defaults */
	struct adt_node *defaults = adt_new(root, "defaults");
	if (m->uart_phandle && uart_working()) {       /* a dead UART would hang the kernel's console */
		adt_set_u32(defaults, "serial-device", m->uart_phandle);
	}
	/* The display pool, which the kernel maps write-combined (carve_display_pool). */
	if (m->pool_size) {
		struct pmap_io_range pool = { m->pool_pa, m->pool_size, VM_WIMG_WCOMB, 'DPOL' };
		adt_set(defaults, "pmap-io-ranges", &pool, sizeof(pool));
	}
	return root;
}

/* --- main --------------------------------------------------------------------- */

static bool
parse_u64(const char **pp, uint64_t *out)
{
	const char *p = *pp;
	unsigned base = 10;
	uint64_t v = 0;
	bool any = false;

	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
		base = 16;
		p += 2;
	}
	for (;; p++) {
		unsigned d;
		if (*p >= '0' && *p <= '9') {
			d = (unsigned)(*p - '0');
		} else if (base == 16 && (*p | 0x20) >= 'a' && (*p | 0x20) <= 'f') {
			d = (unsigned)((*p | 0x20) - 'a' + 10);
		} else {
			break;
		}
		v = v * base + d;
		any = true;
	}
	*pp = p;
	*out = v;
	return any;
}

/*
 * "pieboot.addmem=BASE,SIZE" adds a RAM range the firmware does not report
 * (QEMU's raspi4b describes only the first bank of its 2 GB).
 */
static void
scan_addmem(struct machine *m, const char *args, size_t len)
{
	static const char key[] = "pieboot.addmem=";
	for (size_t i = 0; args && i + sizeof(key) - 1 < len && args[i]; i++) {
		if ((i > 0 && args[i - 1] != ' ') || strncmp(args + i, key, sizeof(key) - 1)) {
			continue;
		}
		const char *p = args + i + sizeof(key) - 1;
		uint64_t base, size;
		if (parse_u64(&p, &base) && *p++ == ',' && parse_u64(&p, &size) && size &&
		    m->nram < MAX_RAM_RANGES) {
			m->ram[m->nram].base = base;
			m->ram[m->nram].end = base + size;
			m->nram++;
			printf("pieboot: adding RAM 0x%lx-0x%lx from boot-args\n", base, base + size);
		}
	}
}

/*
 * "pieboot.memhole=BASE,SIZE" takes a range out of RAM, splitting the range
 * it is in: to try the DRAM holes of a Raspberry Pi's memory map (the Pi
 * 5's below 1 GB) on QEMU's virt, whose RAM is in one piece.
 */
static void
scan_memhole(struct machine *m, const char *args, size_t len)
{
	static const char key[] = "pieboot.memhole=";
	for (size_t i = 0; args && i + sizeof(key) - 1 < len && args[i]; i++) {
		if ((i > 0 && args[i - 1] != ' ') || strncmp(args + i, key, sizeof(key) - 1)) {
			continue;
		}
		const char *p = args + i + sizeof(key) - 1;
		uint64_t base, size;
		if (!parse_u64(&p, &base) || *p++ != ',' || !parse_u64(&p, &size) || !size) {
			continue;
		}
		for (unsigned r = 0; r < m->nram && m->nram < MAX_RAM_RANGES; r++) {
			if (base > m->ram[r].base && base + size < m->ram[r].end) {
				for (unsigned j = m->nram; j > r + 1; j--) {
					m->ram[j] = m->ram[j - 1];
				}
				m->ram[r + 1].base = base + size;
				m->ram[r + 1].end = m->ram[r].end;
				m->ram[r].end = base;
				m->nram++;
				printf("pieboot: RAM hole 0x%lx-0x%lx from boot-args\n", base, base + size);
				break;
			}
		}
	}
}

/*
 * "pieboot.dmapool=SIZE" (bytes, or with K, M or G; 0 for none): the size
 * of the display pool (carve_display_pool). The bundle's boot-args come
 * after the FDT's, so the last one given wins. False if none is given.
 */
static bool
scan_dmapool(const char *args, size_t len, uint64_t *size)
{
	static const char key[] = "pieboot.dmapool=";
	bool found = false;
	for (size_t i = 0; args && i + sizeof(key) - 1 < len && args[i]; i++) {
		if ((i > 0 && args[i - 1] != ' ') || strncmp(args + i, key, sizeof(key) - 1)) {
			continue;
		}
		const char *p = args + i + sizeof(key) - 1;
		uint64_t v;
		if (!parse_u64(&p, &v)) {
			continue;
		}
		switch (*p | 0x20) {
		case 'k':
			v <<= 10;
			break;
		case 'm':
			v <<= 20;
			break;
		case 'g':
			v <<= 30;
			break;
		}
		*size = v;
		found = true;
	}
	return found;
}

/*
 * The display pool: RAM set aside at boot for the buffers a display scans
 * out from, which the IOSurface kext hands out (scan-out IOSurfaces, and
 * piedrm's display buffers through it). The BCM2711's display (its HVS
 * and TXP) reaches only the low GB, and xnu has no CMA: within minutes of
 * a boot, wired pages scattered through the low GB leave no free run of a
 * 4K buffer's 33 MB there, and soon none of 1080p's 8 MB. So the pool
 * defaults to 96 MB on the BCM2711 and to none elsewhere (the BCM2712's
 * display reaches 4 GB); pieboot.dmapool=SIZE sets it.
 *
 * It lies low in the first RAM range, in the lowest gap above this image
 * that holds it on a 2 MB boundary, clear of the /memreserve/ entries and
 * a framebuffer there, and the kernel goes above it (place_kernel): xnu
 * manages memory only from the kernel's physical base up, so the pool is
 * never the VM's, and what lies below the kernel's 32 MB floor goes into
 * it. The firmware's FDT and initrd may lie in it, and so may an entry
 * that only reserves either (as for place_kernel): nothing reads them
 * once the kernel runs, and nothing writes the pool before then. Above
 * them it would cost the VM all the RAM below them (about 750 MB on a 4
 * GB Pi 4, whose firmware leaves them just below 0x2F000000). It cannot be
 * a DRAM hole above the kernel: xnu maps memory it does not manage as a
 * device's (pmap_cache_attributes: VM_WIMG_IO, where unaligned accesses
 * fault) whatever cache mode IOKit asks for, unless pmap-io-ranges says
 * otherwise, and those ranges must lie outside the managed span. So
 * /defaults pmap-io-ranges has the pool, write-combined, and /chosen
 * pieboot,display-pool says where it is. None is carved where it would
 * leave the kernel less than half of the first RAM range, or outside the
 * display's reach (the low GB).
 */
#define DISPLAY_POOL_REACH      0x40000000ULL
#define DISPLAY_POOL_BCM2711    (96ULL << 20)

/* A /memreserve/ entry that lies within the FDT or the initrd: a bootloader reserving what it loaded for the OS. */
static bool
reserves_loaded(const struct machine *m, uint64_t base, uint64_t end)
{
	return (base >= m->fdt.pa && end <= m->fdt.pa + m->fdt.size) ||
	       (base >= m->initrd_start && end <= m->initrd_end);
}

static void
carve_display_pool(struct machine *m, const char *fdt_args, const struct payload *pl, const struct Boot_Video *video)
{
	uint64_t size = fdt_is_compatible(m->fdt.root, "brcm,bcm2711") ? DISPLAY_POOL_BCM2711 : 0;
	scan_dmapool(fdt_args, fdt_args ? strlen(fdt_args) + 1 : 0, &size);
	scan_dmapool(pl->bootargs, pl->bootargs_size, &size);
	if (!size) {
		return;
	}
	size = ALIGN_UP(size, SZ_2M);
	struct place_range ram = { m->ram[0].base, m->ram[0].end };
	struct place_range *busy = balloc(sizeof(*busy) * (m->fdt.nreserves + 1));
	unsigned nbusy = 0;
	for (unsigned i = 0; i < m->fdt.nreserves; i++) {
		uint64_t base = m->fdt.reserves[i].addr, end = base + m->fdt.reserves[i].size;
		if (end > ram.base && base < ram.end && !reserves_loaded(m, base, end)) {
			busy[nbusy++] = (struct place_range){ base, end };
		}
	}
	if (video && video->v_rowBytes && video->v_height) {
		busy[nbusy++] = (struct place_range){ video->v_baseAddr,
			video->v_baseAddr + video->v_rowBytes * video->v_height };
	}
	uint64_t base;
	if (!place_lowest(ram, (uint64_t)(uintptr_t)__image_end, SZ_2M, 0, size, busy, nbusy, &base) ||
	    ram.end - base < 2 * size || base + size > DISPLAY_POOL_REACH) {
		printf("pieboot: no display pool of %lu MB: no room for it below the kernel\n", size >> 20);
		return;
	}
	m->pool_pa = base;
	m->pool_size = size;
	printf("pieboot: display pool 0x%lx-0x%lx (%lu MB)\n", base, base + size, size >> 20);
}

/*
 * The kernel's physBase, for size bytes from there (the kernel, then its
 * device tree, RAM disk and boot_args), as low in the first RAM range as
 * they fit: xnu manages memory from physBase up only (gPhysBase; its VM
 * starts at topOfKernelData), so RAM below the kernel is lost to it. The
 * firmware's FDT and initrd may lie high (the Pi 4's and Pi 5's firmware
 * leave them just below 0x2F000000, which cost 752 MB when the kernel went
 * above them); the kernel goes below them when it fits there.
 *
 * - Above this image, .bss included: PSCI's EL2 vectors, stacks and data
 *   and the secondary CPUs' entry run from it after the kernel starts.
 * - At or above 32 MB (KERNEL_L2_BLOCK): xnu's arm_vm_page_granular_helper()
 *   takes the physical address an L2 block starts at as that of a range
 *   less the range's offset in the block, which wraps below 0 for a range
 *   lower than that offset. Its physical aperture is mapped page by page
 *   (PHYSMAP_GRANULE 0), not in step with the blocks, so a 16K-page kernel
 *   at 16 MB lost the aperture's page below it on about half the boots
 *   (arm_vm_prot_finalize: "phystokv: illegal PA: 0x0").
 * - Above every /memreserve/ entry in that range, so that none reaches
 *   the VM, except those within the FDT or the initrd (a bootloader
 *   reserving what it loaded for the OS).
 * - Clear of the FDT and the initrd, which pieboot reads while it writes
 *   the kernel. Once the kernel runs nothing reads either (pieboot has
 *   copied what it needs: the kernel, the RAM disk, the boot-args and
 *   the device tree's properties), and the VM takes their memory.
 * - Clear of the framebuffer, should it lie in RAM, and of room below it
 *   for the kernel's first allocations: the kernel keeps the framebuffer
 *   out of the VM as a DRAM hole, but those take pages upward from
 *   topOfKernelData without looking at holes (pmap_next_page). They take
 *   about 60 MB on the 4 GB Pi 4; a 32nd of the RAM span (126 MB there)
 *   is room enough.
 * - Above the display pool (carve_display_pool), which lies low in the
 *   first RAM range so that the VM never has it.
 */
static uint64_t
place_kernel(const struct machine *m, const struct Boot_Video *video, uint64_t virt_base, uint64_t size)
{
	struct place_range ram = { m->ram[0].base, m->ram[0].end };
	struct place_range fdt = { m->fdt.pa, m->fdt.pa + m->fdt.size };
	struct place_range initrd = { m->initrd_start, m->initrd_end };
	struct place_range busy[3] = { fdt, initrd, { 0, 0 } };
	unsigned nbusy = 2;
	uint64_t low = (uint64_t)(uintptr_t)__image_end;

	printf("pieboot: pieboot 0x%lx-0x%lx, FDT 0x%lx-0x%lx, initrd 0x%lx-0x%lx\n",
	    (uint64_t)(uintptr_t)__image_start, low, fdt.base, fdt.end, initrd.base, initrd.end);
	low = MAX(low, KERNEL_L2_BLOCK);
	if (m->pool_size) {
		low = MAX(low, m->pool_pa + m->pool_size);
	}
	for (unsigned i = 0; i < m->fdt.nreserves; i++) {
		uint64_t base = m->fdt.reserves[i].addr, end = base + m->fdt.reserves[i].size;
		printf("pieboot: /memreserve/ 0x%lx-0x%lx\n", base, end);
		if (end <= ram.base || base >= ram.end || reserves_loaded(m, base, end)) {
			continue;
		}
		low = MAX(low, end);
	}
	if (video && video->v_rowBytes && video->v_height) {
		uint64_t fb = video->v_baseAddr, guard = m->ram_size / 32;
		busy[nbusy].base = fb > guard ? fb - guard : 0;
		busy[nbusy].end = fb + video->v_rowBytes * video->v_height;
		nbusy++;
	}
	/*
	 * The kernel's bootstrap page tables (start.s) map it with L2 blocks:
	 * physBase is congruent to virtBase modulo the block.
	 */
	uint64_t phys_base;
	if (!place_lowest(ram, low, KERNEL_L2_BLOCK, virt_base & (KERNEL_L2_BLOCK - 1), size, busy, nbusy,
	    &phys_base)) {
		panic("not enough memory for the kernel image (%lu MB in 0x%lx-0x%lx above 0x%lx)",
		    size >> 20, ram.base, ram.end, low);
	}
	return phys_base;
}

void pieboot_main(void *fdt_blob);

void
pieboot_main(void *fdt_blob)
{
	static struct machine m;
	uint64_t pa, size;

	fdt_parse(&m.fdt, fdt_blob);
	/* The activity LED: on while pieboot runs (off at the kernel, blinking on an error). */
	led_init(&m.fdt);
	led_set(true);
	m.uart = find_console(&m);
	for (const char *const *c = (const char *const[]){ "arm,gic-400", "arm,cortex-a15-gic",
	    "arm,cortex-a9-gic", "arm,gic-v2", NULL }; *c && !m.gic; c++) {
		m.gic = fdt_find_compatible(m.fdt.root, *c);
	}
	if (m.uart && fdt_reg(m.uart, 0, &pa, &size)) {
		uart_set_base(pa);
		/* A UART the firmware has not set up: 115200 from its clock. */
		uint32_t clk = 0;
		struct fdt_prop *clocks = fdt_prop(m.uart, "clocks");
		struct fdt_node *provider = clocks && clocks->len >= 4 ?
		    fdt_find_phandle(m.fdt.root, (uint32_t)fdt_read_cells(clocks->val, 1)) : NULL;
		if (provider && fdt_is_compatible(provider, "fixed-clock")) {
			fdt_prop_u32(provider, "clock-frequency", &clk);
		}
		uart_setup(clk, 115200);
	}
	/*
	 * The screen first: the pie, or with -v everything after as text as
	 * well as on the UART. The kernel gets the same framebuffer.
	 */
	struct fdt_node *chosen = fdt_path(&m.fdt, "/chosen");
	const char *fdt_bootargs = fdt_prop_str(chosen, "bootargs");
	bool verbose = bootflag(fdt_bootargs, fdt_bootargs ? strlen(fdt_bootargs) + 1 : 0, "-v");
	static struct Boot_Video video;
	bool have_video = dt_framebuffer(&m.fdt, &video) || (mbox_init(&m.fdt) && mbox_framebuffer(&video));
	if (have_video) {
		fbcon_init(video.v_baseAddr, (uint32_t)video.v_width, (uint32_t)video.v_height,
		    (uint32_t)video.v_rowBytes, (uint32_t)(video.v_depth & 0xff), verbose);
	}
	const char *model = fdt_prop_str(m.fdt.root, "model");
	printf("\npieboot: boot shim for xnu, EL%u, FDT at %p%s%s\n", current_el(), fdt_blob,
	    model ? ", " : "", model ? model : "");
	if (have_video) {
		printf("pieboot: framebuffer 0x%lx, %lux%lu, %lu bytes/row, %lu bpp\n", video.v_baseAddr,
		    video.v_width, video.v_height, video.v_rowBytes, video.v_depth & 0xff);
	}
	if (m.uart && !uart_working()) {
		printf("pieboot: the serial console's UART does not take output; serial is off\n");
	}
	m.shim_psci = psci_setup(&m.fdt, current_el());

	m.initrd_start = chosen_addr(chosen, "linux,initrd-start");
	m.initrd_end = chosen_addr(chosen, "linux,initrd-end");
	if (!m.initrd_start || m.initrd_end <= m.initrd_start) {
		panic_nokernel("no initrd: load the xnu kernel or bundle as the initrd");
	}
	struct payload pl;
	open_payload((const uint8_t *)(uintptr_t)m.initrd_start, m.initrd_end - m.initrd_start, &pl);
	if (!verbose && bootflag(pl.bootargs, pl.bootargs_size, "-v")) {
		verbose = true;
		fbcon_text();           /* with what was printed so far */
	}

	/*
	 * Memory: every range of every /memory node, sorted and merged. The
	 * kernel manages the span from the first to the last; the gaps between
	 * (the VideoCore carve-out, the peripheral window) become dram-holes.
	 */
	for (struct fdt_node *c = m.fdt.root->child; c; c = c->sibling) {
		const char *type = fdt_prop_str(c, "device_type");
		if (!type || strcmp(type, "memory")) {
			continue;
		}
		for (unsigned i = 0; i < fdt_reg_count(c) && m.nram < MAX_RAM_RANGES; i++) {
			uint64_t base, size;
			if (fdt_reg(c, i, &base, &size) && size) {
				m.ram[m.nram].base = base;
				m.ram[m.nram].end = base + size;
				m.nram++;
			}
		}
	}
	select_kernel(&m.fdt, &pl, fdt_bootargs);
	scan_addmem(&m, fdt_bootargs, fdt_bootargs ? strlen(fdt_bootargs) + 1 : 0);
	scan_addmem(&m, pl.bootargs, pl.bootargs_size);
	if (!m.nram) {
		panic("no /memory node");
	}
	for (unsigned i = 1; i < m.nram; i++) {        /* insertion sort by base */
		for (unsigned j = i; j > 0 && m.ram[j].base < m.ram[j - 1].base; j--) {
			struct range t = m.ram[j];
			m.ram[j] = m.ram[j - 1];
			m.ram[j - 1] = t;
		}
	}
	unsigned merged = 1;
	for (unsigned i = 1; i < m.nram; i++) {
		if (m.ram[i].base <= m.ram[merged - 1].end) {
			m.ram[merged - 1].end = MAX(m.ram[merged - 1].end, m.ram[i].end);
		} else {
			m.ram[merged++] = m.ram[i];
		}
	}
	m.nram = merged;
	scan_memhole(&m, fdt_bootargs, fdt_bootargs ? strlen(fdt_bootargs) + 1 : 0);
	scan_memhole(&m, pl.bootargs, pl.bootargs_size);
	carve_display_pool(&m, fdt_bootargs, &pl, have_video ? &video : NULL);
	/*
	 * The panic region: the top of the last RAM range, above the memory the
	 * kernel manages, so that a panic log survives a warm reset.
	 */
	struct range *last = &m.ram[m.nram - 1];
	if (last->end - last->base > 2 * PANIC_REGION_SIZE) {
		m.panic_size = PANIC_REGION_SIZE;
		m.panic_pa = ALIGN_DOWN(last->end - m.panic_size, SZ_16K);
		last->end = m.panic_pa;
		show_previous_panic(&m);
	}
	m.ram_base = m.ram[0].base;
	m.ram_size = m.ram[m.nram - 1].end - m.ram_base;
	for (unsigned i = 0; i < m.nram; i++) {
		printf("pieboot: RAM 0x%lx-0x%lx\n", m.ram[i].base, m.ram[i].end);
	}

	struct kernel_image k;
	macho_inspect(pl.kernel, pl.kernel_size, &k);

	m.timebase = read_cntfrq();
	uint32_t tf;
	struct fdt_node *timer = fdt_find_compatible(m.fdt.root, "arm,armv8-timer");
	if (timer && fdt_prop_u32(timer, "clock-frequency", &tf)) {
		m.timebase = tf;
	}
	m.boot_mpidr = read_sysreg_mpidr();
	m.next_phandle = 0x10000;
	m.io_base = UINT64_MAX;
	for (struct fdt_node *c = m.fdt.root->child; c; c = c->sibling) {
		if (!is_toplevel_skipped(c)) {
			scan_io_range(&m, c);
		}
	}
	if (m.io_base == UINT64_MAX) {
		panic("no memory-mapped devices in the FDT");
	}

	uint64_t virt_base = ALIGN_DOWN(k.vbase, SZ_2M);
	uint64_t ram_end = m.ram_base + m.ram_size;
	printf("pieboot: kernel va 0x%lx-0x%lx entry 0x%lx\n", k.vbase, k.vend, k.entry);

	/*
	 * The device tree first, as the kernel's place depends on its size:
	 * built with a provisional RAM disk address, set below (the same size).
	 * After the kernel (offsets from physBase, which is 2 MB-aligned):
	 * the device tree, the RAM disk and boot_args.
	 */
	mbox_init(&m.fdt);
	struct adt_node *adt = build_adt(&m, 0, pl.ramdisk_size);
	uint64_t dt_size = adt_size(adt);
	uint64_t dt_off = ALIGN_UP(k.vend - virt_base, SZ_16K);
	uint64_t ramdisk_off = ALIGN_UP(dt_off + dt_size + SZ_16K, SZ_16K);
	/* memdev counts whole pages (and rounds down): pad with zeroes. */
	uint64_t ramdisk_len = ALIGN_UP(pl.ramdisk_size, SZ_16K);
	uint64_t ba_off = ALIGN_UP(ramdisk_off + ramdisk_len, SZ_16K);
	uint64_t top_off = ALIGN_UP(ba_off + sizeof(struct boot_args), SZ_16K);

	uint64_t phys_base = place_kernel(&m, have_video ? &video : NULL, virt_base, top_off);
	uint64_t dt_pa = phys_base + dt_off;
	uint64_t ramdisk_pa = phys_base + ramdisk_off;
	uint64_t ba_pa = phys_base + ba_off;
	uint64_t top = phys_base + top_off;
	printf("pieboot: physBase 0x%lx virtBase 0x%lx\n", phys_base, virt_base);
	/* Nothing cached for this memory may land on top of what pieboot writes there. */
	dcache_clean_invalidate(phys_base, top);
	macho_load(&k, virt_base, phys_base);

	if (pl.ramdisk_size) {
		struct adt_node *chosen_node = NULL;
		for (struct adt_node *c = adt->child; c; c = c->sibling) {
			struct adt_prop *n = adt_get(c, "name");
			if (n && !strcmp(n->val, "chosen")) {
				chosen_node = c;
			}
		}
		for (struct adt_node *c = chosen_node ? chosen_node->child : NULL; c; c = c->sibling) {
			struct adt_prop *n = adt_get(c, "name");
			if (n && !strcmp(n->val, "memory-map")) {
				uint64_t range[2] = { ramdisk_pa, ramdisk_len };
				adt_set(c, "RAMDisk", range, sizeof(range));
			}
		}
		dt_size = adt_size(adt);
	}
	if (dt_off + dt_size > ramdisk_off) {
		panic("the device tree outgrew its room (%lu bytes)", dt_size);
	}

	adt_serialize(adt, (uint8_t *)(uintptr_t)dt_pa);
	if (pl.ramdisk_size) {
		memmove((void *)(uintptr_t)ramdisk_pa, pl.ramdisk, pl.ramdisk_size);
		memset((void *)(uintptr_t)(ramdisk_pa + pl.ramdisk_size), 0, ramdisk_len - pl.ramdisk_size);
		printf("pieboot: RAM disk at 0x%lx, %lu bytes\n", ramdisk_pa, (uint64_t)pl.ramdisk_size);
	}

	struct boot_args *ba = (struct boot_args *)(uintptr_t)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->Revision = kBootArgsRevision2;
	ba->Version = kBootArgsVersion2;
	ba->virtBase = virt_base;
	ba->physBase = phys_base;
	ba->memSize = ram_end - phys_base;
	ba->memSizeActual = 0;
	for (unsigned i = 0; i < m.nram; i++) {
		ba->memSizeActual += m.ram[i].end - m.ram[i].base;
	}
	ba->topOfKernelData = top;
	ba->deviceTreeP = virt_base + (dt_pa - phys_base);
	ba->deviceTreeLength = (uint32_t)dt_size;
	if (have_video) {
		ba->Video = video;
		/*
		 * As Apple's boot.efi does: graphics mode, unless -v, keeps xnu's
		 * text console off the screen (PE_create_console), so the pie
		 * stays; text mode shows the kernel's messages.
		 */
		ba->Video.v_display = verbose ? 0 : 1;
	}
	if (fdt_bootargs) {
		strlcpy(ba->CommandLine, fdt_bootargs, sizeof(ba->CommandLine));
	}
	if (pl.bootargs_size) {
		char extra[BOOT_LINE_LENGTH];
		size_t n = MIN(pl.bootargs_size, sizeof(extra) - 1);
		memcpy(extra, pl.bootargs, n);
		extra[n] = 0;
		if (ba->CommandLine[0]) {
			strlcat(ba->CommandLine, " ", sizeof(ba->CommandLine));
		}
		strlcat(ba->CommandLine, extra, sizeof(ba->CommandLine));
	}

	uint64_t entry_pa = phys_base + (k.entry - virt_base);
	printf("pieboot: device tree 0x%lx (%lu bytes), boot_args 0x%lx, top 0x%lx\n",
	    dt_pa, dt_size, ba_pa, top);
	printf("pieboot: boot-args \"%s\"\n", ba->CommandLine);
	printf("pieboot: entering kernel at 0x%lx\n\n", entry_pa);
	led_set(false);
	/* What pieboot wrote with the caches off, as the kernel will read it with them on. */
	dcache_clean_invalidate(phys_base, top);
	if (m.panic_size) {
		dcache_clean_invalidate(m.panic_pa, m.panic_pa + m.panic_size);
	}
	pieboot_enter_kernel(entry_pa, ba_pa);
}
