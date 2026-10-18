/* SPDX-License-Identifier: BSD-2-Clause */
#include "fdt.h"

#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

void
fdt_parse(struct fdt *fdt, const void *blob)
{
	const uint8_t *b = blob;

	if (be32(b) != FDT_MAGIC) {
		panic("bad FDT magic 0x%x at %p", be32(b), blob);
	}
	uint32_t totalsize = be32(b + 4);
	uint32_t off_struct = be32(b + 8);
	uint32_t off_strings = be32(b + 12);
	uint32_t off_rsvmap = be32(b + 16);
	uint32_t size_struct = be32(b + 36);

	fdt->pa = (uintptr_t)blob;
	fdt->size = totalsize;

	unsigned n = 0;
	for (const uint8_t *r = b + off_rsvmap; be64(r) || be64(r + 8); r += 16) {
		n++;
	}
	fdt->reserves = balloc(sizeof(*fdt->reserves) * (n + 1));
	fdt->nreserves = n;
	for (unsigned i = 0; i < n; i++) {
		fdt->reserves[i].addr = be64(b + off_rsvmap + 16 * i);
		fdt->reserves[i].size = be64(b + off_rsvmap + 16 * i + 8);
	}

	const uint8_t *p = b + off_struct;
	const uint8_t *end = p + size_struct;
	const char *strings = (const char *)b + off_strings;
	struct fdt_node *cur = NULL;
	struct fdt_node *last_child[64] = { 0 };   /* indexed by depth */
	struct fdt_prop *last_prop = NULL;
	int depth = -1;

	while (p < end) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			struct fdt_node *node = balloc(sizeof(*node));
			node->name = (const char *)p;
			p += ALIGN_UP(strlen((const char *)p) + 1, 4);
			node->parent = cur;
			depth++;
			if (depth >= 63) {
				panic("FDT too deep");
			}
			if (cur) {
				if (last_child[depth]) {
					last_child[depth]->sibling = node;
				} else {
					cur->child = node;
				}
			} else {
				fdt->root = node;
			}
			last_child[depth] = node;
			last_child[depth + 1] = NULL;
			cur = node;
			last_prop = NULL;
			break;
		}
		case FDT_END_NODE:
			cur = cur->parent;
			depth--;
			last_prop = NULL;
			if (cur) {
				for (last_prop = cur->props; last_prop && last_prop->next; last_prop = last_prop->next) {
				}
			}
			break;
		case FDT_PROP: {
			uint32_t len = be32(p);
			uint32_t nameoff = be32(p + 4);
			struct fdt_prop *prop = balloc(sizeof(*prop));
			prop->name = strings + nameoff;
			prop->val = p + 8;
			prop->len = len;
			p += 8 + ALIGN_UP(len, 4);
			if (last_prop) {
				last_prop->next = prop;
			} else {
				cur->props = prop;
			}
			last_prop = prop;
			break;
		}
		case FDT_NOP:
			break;
		case FDT_END:
			return;
		default:
			panic("bad FDT token 0x%x", tok);
		}
	}
}

struct fdt_prop *
fdt_prop(const struct fdt_node *n, const char *name)
{
	for (struct fdt_prop *p = n ? n->props : NULL; p; p = p->next) {
		if (!strcmp(p->name, name)) {
			return p;
		}
	}
	return NULL;
}

size_t
fdt_basename(const struct fdt_node *n, char *buf, size_t size)
{
	const char *at = strchr(n->name, '@');
	size_t len = at ? (size_t)(at - n->name) : strlen(n->name);

	if (size) {
		size_t c = MIN(len, size - 1);
		memcpy(buf, n->name, c);
		buf[c] = 0;
	}
	return len;
}

struct fdt_node *
fdt_child(const struct fdt_node *n, const char *name)
{
	char buf[64];

	for (struct fdt_node *c = n ? n->child : NULL; c; c = c->sibling) {
		if (!strcmp(c->name, name)) {
			return c;
		}
		fdt_basename(c, buf, sizeof(buf));
		if (!strcmp(buf, name)) {
			return c;
		}
	}
	return NULL;
}

struct fdt_node *
fdt_path(const struct fdt *fdt, const char *path)
{
	struct fdt_node *n = fdt->root;
	char comp[64];

	while (n && *path) {
		while (*path == '/') {
			path++;
		}
		if (!*path) {
			break;
		}
		size_t len = 0;
		while (path[len] && path[len] != '/') {
			len++;
		}
		if (len >= sizeof(comp)) {
			return NULL;
		}
		memcpy(comp, path, len);
		comp[len] = 0;
		n = fdt_child(n, comp);
		path += len;
	}
	return n;
}

bool
fdt_is_compatible(const struct fdt_node *n, const char *compat)
{
	struct fdt_prop *p = fdt_prop(n, "compatible");

	if (!p) {
		return false;
	}
	for (uint32_t off = 0; off < p->len;) {
		const char *s = (const char *)p->val + off;
		if (!strcmp(s, compat)) {
			return true;
		}
		off += strnlen(s, p->len - off) + 1;
	}
	return false;
}

struct fdt_node *
fdt_find_compatible(struct fdt_node *from, const char *compat)
{
	if (!from) {
		return NULL;
	}
	if (fdt_is_compatible(from, compat)) {
		return from;
	}
	for (struct fdt_node *c = from->child; c; c = c->sibling) {
		struct fdt_node *r = fdt_find_compatible(c, compat);
		if (r) {
			return r;
		}
	}
	return NULL;
}

uint32_t
fdt_phandle(const struct fdt_node *n)
{
	uint32_t v;

	if (fdt_prop_u32(n, "phandle", &v) || fdt_prop_u32(n, "linux,phandle", &v)) {
		return v;
	}
	return 0;
}

struct fdt_node *
fdt_find_phandle(struct fdt_node *from, uint32_t phandle)
{
	if (!from) {
		return NULL;
	}
	if (fdt_phandle(from) == phandle) {
		return from;
	}
	for (struct fdt_node *c = from->child; c; c = c->sibling) {
		struct fdt_node *r = fdt_find_phandle(c, phandle);
		if (r) {
			return r;
		}
	}
	return NULL;
}

const char *
fdt_prop_str(const struct fdt_node *n, const char *name)
{
	struct fdt_prop *p = fdt_prop(n, name);

	if (!p || !p->len || p->val[p->len - 1] != 0) {
		return NULL;
	}
	return (const char *)p->val;
}

bool
fdt_prop_u32(const struct fdt_node *n, const char *name, uint32_t *out)
{
	struct fdt_prop *p = fdt_prop(n, name);

	if (!p || p->len < 4) {
		return false;
	}
	*out = be32(p->val);
	return true;
}

unsigned
fdt_address_cells(const struct fdt_node *bus)
{
	uint32_t v;
	return (bus && fdt_prop_u32(bus, "#address-cells", &v)) ? v : 2;
}

unsigned
fdt_size_cells(const struct fdt_node *bus)
{
	uint32_t v;
	return (bus && fdt_prop_u32(bus, "#size-cells", &v)) ? v : 1;
}

uint64_t
fdt_read_cells(const uint8_t *p, unsigned ncells)
{
	uint64_t v = 0;

	for (unsigned i = 0; i < ncells; i++) {
		v = (v << 32) | be32(p + 4 * i);
	}
	return v;
}

/* Translate an address on the bus `bus` up to the CPU physical address space. */
static bool
fdt_translate(const struct fdt_node *bus, uint64_t *addr)
{
	while (bus && bus->parent) {
		struct fdt_prop *ranges = fdt_prop(bus, "ranges");
		if (!ranges) {
			return false;           /* not memory mapped (e.g. i2c, cpus) */
		}
		if (ranges->len) {
			unsigned ca = fdt_address_cells(bus);
			unsigned pa = fdt_address_cells(bus->parent);
			unsigned sc = fdt_size_cells(bus);
			unsigned entry = 4 * (ca + pa + sc);
			bool found = false;
			for (uint32_t off = 0; off + entry <= ranges->len; off += entry) {
				uint64_t child = fdt_read_cells(ranges->val + off, ca);
				uint64_t parent = fdt_read_cells(ranges->val + off + 4 * ca, pa);
				uint64_t size = fdt_read_cells(ranges->val + off + 4 * (ca + pa), sc);
				if (*addr >= child && *addr - child < size) {
					*addr = parent + (*addr - child);
					found = true;
					break;
				}
			}
			if (!found) {
				return false;
			}
		}
		bus = bus->parent;
	}
	return true;
}

/*
 * A DMA address on the bus `bus` (as its masters put it out) up to the
 * CPU's, through each ancestor's dma-ranges (none: the same address).
 */
static bool
fdt_dma_up(const struct fdt_node *bus, uint64_t *addr)
{
	for (; bus && bus->parent; bus = bus->parent) {
		struct fdt_prop *dr = fdt_prop(bus, "dma-ranges");
		if (!dr || !dr->len) {
			continue;
		}
		unsigned ca = fdt_address_cells(bus);
		unsigned pa = fdt_address_cells(bus->parent);
		unsigned sc = fdt_size_cells(bus);
		unsigned entry = 4 * (ca + pa + sc);
		bool found = false;
		for (uint32_t off = 0; off + entry <= dr->len; off += entry) {
			uint64_t child = fdt_read_cells(dr->val + off, ca);
			uint64_t parent = fdt_read_cells(dr->val + off + 4 * ca, pa);
			uint64_t size = fdt_read_cells(dr->val + off + 4 * (ca + pa), sc);
			if (*addr >= child && *addr - child < size) {
				*addr = parent + (*addr - child);
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

/*
 * The DMA view of the bus a device sits on: its parent's dma-ranges, each
 * followed up through the ancestors' (PCIe behind RP1, say), as up to max
 * { CPU physical address, bus address, size } triples. 0 when the bus has
 * none (DMA addresses are the CPU's).
 */
unsigned
fdt_dma_ranges(const struct fdt_node *n, struct fdt_dma_range *out, unsigned max)
{
	const struct fdt_node *bus = n->parent;
	struct fdt_prop *dr = bus ? fdt_prop(bus, "dma-ranges") : NULL;
	if (!dr || !dr->len || !bus->parent) {
		return 0;
	}
	unsigned ca = fdt_address_cells(bus);
	unsigned pa = fdt_address_cells(bus->parent);
	unsigned sc = fdt_size_cells(bus);
	unsigned entry = 4 * (ca + pa + sc), count = 0;
	for (uint32_t off = 0; off + entry <= dr->len && count < max; off += entry) {
		out[count].bus = fdt_read_cells(dr->val + off, ca);
		out[count].cpu = fdt_read_cells(dr->val + off + 4 * ca, pa);
		out[count].size = fdt_read_cells(dr->val + off + 4 * (ca + pa), sc);
		if (fdt_dma_up(bus->parent, &out[count].cpu)) {
			count++;
		}
	}
	return count;
}

/* The CPU physical address of bus address bus as device n uses it. */
bool
fdt_dma_to_cpu(const struct fdt_node *n, uint64_t bus, uint64_t *pa)
{
	struct fdt_dma_range r[8];
	unsigned count = fdt_dma_ranges(n, r, 8);
	if (count == 0) {
		*pa = bus;
		return true;
	}
	for (unsigned i = 0; i < count; i++) {
		if (bus >= r[i].bus && bus - r[i].bus < r[i].size) {
			*pa = r[i].cpu + (bus - r[i].bus);
			return true;
		}
	}
	return false;
}

/* A CPU physical address as device n reaches it by DMA; false if it cannot. */
bool
fdt_dma_address(const struct fdt_node *n, uint64_t pa, uint64_t *bus)
{
	struct fdt_dma_range r[8];
	unsigned count = fdt_dma_ranges(n, r, 8);
	if (count == 0) {
		*bus = pa;
		return true;
	}
	for (unsigned i = 0; i < count; i++) {
		if (pa >= r[i].cpu && pa - r[i].cpu < r[i].size) {
			*bus = r[i].bus + (pa - r[i].cpu);
			return true;
		}
	}
	return false;
}

unsigned
fdt_reg_count(const struct fdt_node *n)
{
	struct fdt_prop *reg = fdt_prop(n, "reg");

	if (!reg || !n->parent) {
		return 0;
	}
	unsigned entry = 4 * (fdt_address_cells(n->parent) + fdt_size_cells(n->parent));
	return entry ? reg->len / entry : 0;
}

bool
fdt_reg(const struct fdt_node *n, unsigned index, uint64_t *pa, uint64_t *size)
{
	struct fdt_prop *reg = fdt_prop(n, "reg");

	if (!reg || !n->parent || index >= fdt_reg_count(n)) {
		return false;
	}
	unsigned ac = fdt_address_cells(n->parent);
	unsigned sc = fdt_size_cells(n->parent);
	const uint8_t *e = reg->val + index * 4 * (ac + sc);
	uint64_t addr = fdt_read_cells(e, ac);
	*size = fdt_read_cells(e + 4 * ac, sc);
	if (!fdt_translate(n->parent, &addr)) {
		return false;
	}
	*pa = addr;
	return true;
}
