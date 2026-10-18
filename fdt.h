/* SPDX-License-Identifier: BSD-2-Clause */
/* Flattened device tree (DTB) parsed into a tree. */
#ifndef PIEBOOT_FDT_H
#define PIEBOOT_FDT_H
#include "lib.h"

struct fdt_prop {
	const char *name;
	const uint8_t *val;
	uint32_t len;
	struct fdt_prop *next;
};

struct fdt_node {
	const char *name;           /* full node name, e.g. "pl011@9000000" */
	struct fdt_node *parent;
	struct fdt_node *child;
	struct fdt_node *sibling;
	struct fdt_prop *props;
};

struct fdt_reserve {
	uint64_t addr;
	uint64_t size;
};

struct fdt {
	struct fdt_node *root;
	uint64_t pa;                /* where the DTB itself lives */
	uint32_t size;
	struct fdt_reserve *reserves;   /* /memreserve/ entries */
	unsigned nreserves;
};

void fdt_parse(struct fdt *fdt, const void *blob);

struct fdt_prop *fdt_prop(const struct fdt_node *n, const char *name);
struct fdt_node *fdt_path(const struct fdt *fdt, const char *path);
struct fdt_node *fdt_child(const struct fdt_node *n, const char *name);  /* name without unit */
struct fdt_node *fdt_find_compatible(struct fdt_node *from, const char *compat);
struct fdt_node *fdt_find_phandle(struct fdt_node *from, uint32_t phandle);
bool fdt_is_compatible(const struct fdt_node *n, const char *compat);
const char *fdt_prop_str(const struct fdt_node *n, const char *name);
bool fdt_prop_u32(const struct fdt_node *n, const char *name, uint32_t *out);
uint32_t fdt_phandle(const struct fdt_node *n);
size_t fdt_basename(const struct fdt_node *n, char *buf, size_t size);

unsigned fdt_address_cells(const struct fdt_node *bus);
unsigned fdt_size_cells(const struct fdt_node *bus);
uint64_t fdt_read_cells(const uint8_t *p, unsigned ncells);

/* Translate reg entry `index` of node n to a CPU physical address. */
bool fdt_reg(const struct fdt_node *n, unsigned index, uint64_t *pa, uint64_t *size);

struct fdt_dma_range {
	uint64_t cpu, bus, size;
};
unsigned fdt_dma_ranges(const struct fdt_node *n, struct fdt_dma_range *out, unsigned max);
bool fdt_dma_address(const struct fdt_node *n, uint64_t pa, uint64_t *bus);
bool fdt_dma_to_cpu(const struct fdt_node *n, uint64_t bus, uint64_t *pa);
unsigned fdt_reg_count(const struct fdt_node *n);
#endif
