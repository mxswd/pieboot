/* SPDX-License-Identifier: BSD-2-Clause */
/* Apple flattened device tree (the format xnu's pexpert/gen/device_tree.c reads). */
#ifndef PIEBOOT_ADT_H
#define PIEBOOT_ADT_H
#include "lib.h"

#define ADT_PROP_NAME_LEN 32

struct adt_prop {
	char name[ADT_PROP_NAME_LEN];
	void *val;
	uint32_t len;
	struct adt_prop *next;
};

struct adt_node {
	struct adt_prop *props, *last_prop;
	unsigned nprops;
	struct adt_node *child, *last_child, *sibling;
	unsigned nchildren;
};

/* New node with a "name" property, appended to parent (if any). */
struct adt_node *adt_new(struct adt_node *parent, const char *name);
struct adt_prop *adt_set(struct adt_node *n, const char *name, const void *val, uint32_t len);
struct adt_prop *adt_get(struct adt_node *n, const char *name);
void adt_set_u32(struct adt_node *n, const char *name, uint32_t v);
void adt_set_u64(struct adt_node *n, const char *name, uint64_t v);
void adt_set_str(struct adt_node *n, const char *name, const char *s);

/* Size of the serialized tree, and serialization into buf. */
size_t adt_size(const struct adt_node *root);
size_t adt_serialize(const struct adt_node *root, uint8_t *buf);
#endif
