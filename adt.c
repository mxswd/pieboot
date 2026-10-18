/* SPDX-License-Identifier: BSD-2-Clause */
#include "adt.h"

struct adt_node *
adt_new(struct adt_node *parent, const char *name)
{
	struct adt_node *n = balloc(sizeof(*n));

	adt_set_str(n, "name", name);
	if (parent) {
		if (parent->last_child) {
			parent->last_child->sibling = n;
		} else {
			parent->child = n;
		}
		parent->last_child = n;
		parent->nchildren++;
	}
	return n;
}

struct adt_prop *
adt_get(struct adt_node *n, const char *name)
{
	for (struct adt_prop *p = n->props; p; p = p->next) {
		if (!strncmp(p->name, name, ADT_PROP_NAME_LEN)) {
			return p;
		}
	}
	return NULL;
}

struct adt_prop *
adt_set(struct adt_node *n, const char *name, const void *val, uint32_t len)
{
	if (strlen(name) >= ADT_PROP_NAME_LEN) {
		return NULL;                    /* cannot be represented */
	}
	struct adt_prop *p = adt_get(n, name);
	if (!p) {
		p = balloc(sizeof(*p));
		strlcpy(p->name, name, sizeof(p->name));
		if (n->last_prop) {
			n->last_prop->next = p;
		} else {
			n->props = p;
		}
		n->last_prop = p;
		n->nprops++;
	}
	p->val = balloc(len ? len : 1);
	memcpy(p->val, val, len);
	p->len = len;
	return p;
}

void
adt_set_u32(struct adt_node *n, const char *name, uint32_t v)
{
	adt_set(n, name, &v, sizeof(v));
}

void
adt_set_u64(struct adt_node *n, const char *name, uint64_t v)
{
	adt_set(n, name, &v, sizeof(v));
}

void
adt_set_str(struct adt_node *n, const char *name, const char *s)
{
	adt_set(n, name, s, (uint32_t)strlen(s) + 1);
}

size_t
adt_size(const struct adt_node *n)
{
	size_t size = 8;

	for (const struct adt_prop *p = n->props; p; p = p->next) {
		size += ADT_PROP_NAME_LEN + 4 + ALIGN_UP(p->len, 4);
	}
	for (const struct adt_node *c = n->child; c; c = c->sibling) {
		size += adt_size(c);
	}
	return size;
}

size_t
adt_serialize(const struct adt_node *n, uint8_t *buf)
{
	uint8_t *p = buf;

	memcpy(p, &n->nprops, 4);
	memcpy(p + 4, &n->nchildren, 4);
	p += 8;
	for (const struct adt_prop *pr = n->props; pr; pr = pr->next) {
		memset(p, 0, ADT_PROP_NAME_LEN);
		memcpy(p, pr->name, strnlen(pr->name, ADT_PROP_NAME_LEN));
		memcpy(p + ADT_PROP_NAME_LEN, &pr->len, 4);
		p += ADT_PROP_NAME_LEN + 4;
		memset(p, 0, ALIGN_UP(pr->len, 4));
		memcpy(p, pr->val, pr->len);
		p += ALIGN_UP(pr->len, 4);
	}
	for (const struct adt_node *c = n->child; c; c = c->sibling) {
		p += adt_serialize(c, p);
	}
	return (size_t)(p - buf);
}
