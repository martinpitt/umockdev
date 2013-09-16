/*
 * Copyright (C) 2013 Canonical Ltd.
 * Author: Martin Pitt <martin.pitt@ubuntu.com>
 *
 * umockdev is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * umockdev is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/input.h>

#include "ioctl_tree.h"

#ifdef DEBUG
#    define DBG(...) printf(__VA_ARGS__)
#    define IFDBG(x) x
#else
#    define DBG(...) {}
#    define IFDBG(x) {}
#endif

#define TRUE 1
#define FALSE 0

/***********************************
 *
 * ioctl_tree
 *
 ***********************************/

ioctl_tree *
ioctl_tree_new_from_bin(unsigned long id, const void *data, int ret)
{
    const ioctl_type *type;
    ioctl_tree *t;

    type = ioctl_type_get_by_id(id);
    if (type == NULL) {
	DBG("ioctl_tree_new_from_bin: unknown ioctl %lX\n", id);
	return NULL;
    }
    /* state independent ioctl? */
    if (type->init_from_bin == NULL)
	return NULL;

    t = calloc(sizeof(ioctl_tree), 1);
    t->type = type;
    t->ret = ret;
    t->id = id;
    type->init_from_bin(t, data);
    return t;
}

ioctl_tree *
ioctl_tree_new_from_text(const char *line)
{
    static char lead_ws[1000];
    static char ioctl_name[100];
    int ret, offset;
    unsigned long id;
    const ioctl_type *type;
    ioctl_tree *t;

    if (line[0] == ' ') {
	if (sscanf(line, "%1000[ ]%100s %i %n", lead_ws, ioctl_name, &ret, &offset) < 2) {
	    DBG("ioctl_tree_new_from_text: failed to parse indent, ioctl name, and return value from '%s'\n", line);
	    return NULL;
	}
    } else {
	if (sscanf(line, "%100s %i %n", ioctl_name, &ret, &offset) < 1) {
	    DBG("ioctl_tree_new_from_text: failed to parse ioctl name and return value from '%s'\n", line);
	    return NULL;
	}
	lead_ws[0] = '\0';
    }

    type = ioctl_type_get_by_name(ioctl_name, &id);
    if (type == NULL) {
	DBG("ioctl_tree_new_from_text: unknown ioctl %s\n", ioctl_name);
	return NULL;
    }

    t = calloc(sizeof(ioctl_tree), 1);
    t->type = type;
    t->depth = strlen(lead_ws);
    t->ret = ret;
    t->id = id;
    if (!type->init_from_text(t, line + offset)) {
	DBG("ioctl_tree_new_from_text: ioctl %s failed to initialize from data '%s'\n", ioctl_name, line + offset);
	free(t);
	return NULL;
    }
    return t;
}

void
ioctl_tree_free(ioctl_tree * tree)
{
    if (tree == NULL)
	return;

    ioctl_tree_free(tree->child);
    ioctl_tree_free(tree->next);
    if (tree->type != NULL && tree->type->free_data != NULL)
	tree->type->free_data(tree);
    if (tree->last_added != NULL)
	ioctl_node_list_free(tree->last_added);

    free(tree);
}

static ioctl_tree *
ioctl_tree_last_sibling(ioctl_tree * node)
{
    while (node != NULL && node->next != NULL)
	node = node->next;
    return node;
}

/**
 * ioctl_tree_insert:
 *
 * If an equal node already exists, return that node and do not insert "node".
 * Otherwise, insert it and return NULL.
 */
ioctl_tree *
ioctl_tree_insert(ioctl_tree * tree, ioctl_tree * node)
{
    ioctl_tree *existing;

    assert(node != NULL);

    /* creating the root element? */
    if (tree == NULL) {
	node->last_added = ioctl_node_list_new();
	ioctl_node_list_append(node->last_added, node);
	return NULL;
    }

    /* trying to insert into itself? */
    assert(tree != node);

    existing = ioctl_tree_find_equal(tree, node);
    if (existing) {
	DBG("ioctl_tree_insert: node of type %s ptr %p already exists\n", node->type->name, node);
	ioctl_node_list_append(tree->last_added, existing);
	return existing;
    }

    node->parent = node->type->insertion_parent(tree, node);
    if (node->parent == NULL) {
	fprintf(stderr, "ioctl_tree_insert: did not get insertion parent for node type %s ptr %p\n",
		node->type->name, node);
	abort();
    }

    /* if the parent is the whole tree, then we put it as a sibling, not a
     * child */
    if (node->parent == tree) {
	ioctl_tree_last_sibling(tree)->next = node;
	node->depth = 0;
    } else {
	if (node->parent->child == NULL)
	    node->parent->child = node;
	else
	    ioctl_tree_last_sibling(node->parent->child)->next = node;

	node->depth = node->parent->depth + 1;
    }

    ioctl_node_list_append(tree->last_added, node);
    return NULL;
}

ioctl_tree *
ioctl_tree_read(FILE * f)
{
    ioctl_tree *tree = NULL;
    ioctl_tree *node, *prev = NULL;
    ioctl_tree *sibling;
    char *line = NULL;
    size_t line_len;

    while (getline(&line, &line_len, f) >= 0) {
	/* skip empty and comment lines */
	if (line[0] == '\n' || line[0] == '#')
	    continue;

	node = ioctl_tree_new_from_text(line);
	if (node == NULL) {
	    DBG("ioctl_tree_read: failure to parse line: %s", line);
	    free(line);
	    line = NULL;
	    break;
	}

	if (tree == NULL) {
	    tree = node;
	    node->last_added = ioctl_node_list_new();
	} else {
	    /* insert at the right depth */
	    if (node->depth > prev->depth) {
		assert(node->depth == prev->depth + 1);
		assert(prev->child == NULL);
		prev->child = node;
		node->parent = prev;
	    } else {
		for (sibling = prev; sibling != NULL; sibling = sibling->parent) {
		    if (node->depth == sibling->depth) {
			assert(sibling->next == NULL);
			sibling->next = node;
			//ioctl_tree_last_sibling (sibling)->next = node;
			node->parent = sibling->parent;
			break;
		    }
		}
	    }
	}

	free(line);
	line = NULL;
	prev = node;
    }
    if (line != NULL)
	free(line);

    return tree;
}

void
ioctl_tree_write(FILE * f, const ioctl_tree * tree)
{
    int i;
    if (tree == NULL)
	return;

    /* write indent */
    for (i = 0; i < tree->depth; ++i)
	fputc(' ', f);
    if (tree->id != tree->type->id) {
	long offset;
	offset = _IOC_NR(tree->id) - _IOC_NR(tree->type->id);
	assert(offset >= 0);
	assert(offset <= tree->type->nr_range);
	fprintf(f, "%s(%li) %i ", tree->type->name, offset, tree->ret);
    } else {
	fprintf(f, "%s %i ", tree->type->name, tree->ret);
    }
    tree->type->write(tree, f);
    assert(fputc('\n', f) == '\n');

    ioctl_tree_write(f, tree->child);
    ioctl_tree_write(f, tree->next);
}

ioctl_tree *
ioctl_tree_find_equal(ioctl_tree * tree, ioctl_tree * node)
{
    ioctl_tree *t;

    if (node->id == tree->id && node->type->equal(node, tree))
	return tree;
    if (tree->child) {
	t = ioctl_tree_find_equal(tree->child, node);
	if (t != NULL)
	    return t;
    }
    if (tree->next) {
	t = ioctl_tree_find_equal(tree->next, node);
	if (t != NULL)
	    return t;
    }
    return NULL;
}

ioctl_tree *
ioctl_tree_next(const ioctl_tree * node)
{
    if (node->child != NULL)
	return node->child;
    if (node->next != NULL)
	return node->next;
    /* walk up the parents until we find an alternative sibling */
    for (; node != NULL; node = node->parent)
	if (node->next != NULL)
	    return node->next;

    /* no alternative siblings left, iteration done */
    return NULL;
}

/***********************************
 *
 * ioctl_node_list
 *
 ***********************************/

ioctl_node_list *
ioctl_node_list_new(void)
{
    ioctl_node_list *l;
    l = malloc(sizeof(ioctl_node_list));
    l->n = 0;
    l->capacity = 10;
    l->items = calloc(sizeof(ioctl_tree *), l->capacity);
    return l;
}

void
ioctl_node_list_free(ioctl_node_list * list)
{
    free(list->items);
    list->items = NULL;
    free(list);
}

void
ioctl_node_list_append(ioctl_node_list * list, ioctl_tree * element)
{
    if (list->n == list->capacity) {
	list->capacity *= 2;
	list->items = realloc(list->items, list->capacity * sizeof(ioctl_tree *));
	assert(list->items != NULL);
    }

    list->items[list->n++] = element;
}

ioctl_tree *
ioctl_tree_execute(ioctl_tree * tree, ioctl_tree * last, unsigned long id, void *arg, int *ret)
{
    const ioctl_type *t;
    ioctl_tree *i;
    int r, handled;

    DBG("ioctl_tree_execute ioctl %lX\n", id);

    /* check if it's a hardware independent stateless ioctl */
    t = ioctl_type_get_by_id(id);
    if (t != NULL && t->insertion_parent == NULL) {
	DBG("  ioctl_tree_execute: stateless\n");
	if (t->execute(NULL, id, arg, &r))
	    *ret = r;
	else
	    *ret = -1;
	return last;
    }

    if (tree == NULL)
	return NULL;

    i = ioctl_tree_next_wrap(tree, last);
    /* start at the previously executed node to maintain original order of
     * ioctls as much as possible (i. e. maintain it while the requests come in
     * at the same order as originally recorded) */
    for (;;) {
	DBG("   ioctl_tree_execute: checking node %s(%lX, base id %lX) ", i->type->name, i->id, i->type->id);
	IFDBG(i->type->write(i, stdout));
	DBG("\n");
	handled = i->type->execute(i, id, arg, &r);
	if (handled) {
	    DBG("    -> match, ret %i, adv: %i\n", r, handled);
	    *ret = r;
	    if (handled == 1)
		return i;
	    else
		return last;
	}

	if (last != NULL && i == last) {
	    /* we did a full circle */
	    DBG("    -> full iteration, not found\n");
	    break;
	}

	i = ioctl_tree_next_wrap(tree, i);

	if (last == NULL && i == tree) {
	    /* we did a full circle */
	    DBG("    -> full iteration with last == NULL, not found\n");
	    break;
	}
    }

    /* not found */
    return NULL;
}

/***********************************
 *
 * Utility functions for ioctl implementations
 *
 ***********************************/

static inline signed char
hexdigit(char c)
{
    if (c >= '0' && c <= '9')
	return c - '0';
    if (c >= 'A' && c <= 'F')
	return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
	return c - 'a' + 10;
    return -1;
}

static int
read_hex(const char *hex, char *buf, size_t bufsize)
{
    const char *hexptr = hex;
    size_t written = 0;
    signed char upper, lower;

    while ((upper = hexdigit(hexptr[0])) >= 0) {
	if (written >= bufsize) {
	    DBG("read_hex: data is larger than buffer size %zu\n", bufsize);
	    return FALSE;
	}

	lower = hexdigit(hexptr[1]);
	if (lower < 0) {
	    DBG("read_hex: data has odd number of digits: '%s'\n", hex);
	    return FALSE;
	}
	buf[written++] = upper << 4 | lower;
	hexptr += 2;
    }
    return TRUE;
}

static void
write_hex(FILE * file, const char *buf, size_t len)
{
    size_t i;

    if (len == 0)
	return;

    for (i = 0; i < len; ++i)
	fprintf(file, "%02X", (unsigned)(unsigned char)buf[i]);
}

/***********************************
 *
 * ioctls with simple struct data (i. e. no pointers)
 *
 ***********************************/

#define NSIZE(node) ((node->type && node->type->real_size >= 0) ? node->type->real_size : _IOC_SIZE(node->id))

static inline int
id_matches_type(unsigned long id, const ioctl_type *type)
{
    return _IOC_TYPE(id) == _IOC_TYPE(type->id) &&
           _IOC_DIR(id) == _IOC_DIR(type->id) &&
           _IOC_NR(id) >= _IOC_NR(type->id) &&
           _IOC_NR(id) <= _IOC_NR(type->id) + type->nr_range;
}

static void
ioctl_simplestruct_init_from_bin(ioctl_tree * node, const void *data)
{
    DBG("ioctl_simplestruct_init_from_bin: %s(%lX): size is %lu bytes\n", node->type->name, node->id, NSIZE(node));
    node->data = malloc(NSIZE(node));
    memcpy(node->data, data, NSIZE(node));
}

static int
ioctl_simplestruct_init_from_text(ioctl_tree * node, const char *data)
{
    /* node->id is initialized at this point, but does not necessarily have the
     * correct length for data; this happens for variable length ioctls such as
     * EVIOCGBIT */
    size_t data_len = strlen(data) / 2;
    node->data = malloc(data_len);

    if (NSIZE(node) != data_len) {
	DBG("ioctl_simplestruct_init_from_text: adjusting ioctl ID %lX (size %lu) to actual data length %zu\n",
	    node->id, NSIZE(node), data_len);
	node->id = _IOC(_IOC_DIR(node->id), _IOC_TYPE(node->id), _IOC_NR(node->id), data_len);
    }

    if (!read_hex(data, node->data, NSIZE(node))) {
	DBG("ioctl_simplestruct_init_from_text: failed to parse '%s'\n", data);
	free(node->data);
	return FALSE;
    }
    return TRUE;
}

static void
ioctl_simplestruct_free_data(ioctl_tree * node)
{
    if (node->data != NULL)
	free(node->data);
}

static void
ioctl_simplestruct_write(const ioctl_tree * node, FILE * f)
{
    assert(node->data != NULL);
    write_hex(f, node->data, NSIZE(node));
}

static int
ioctl_simplestruct_equal(const ioctl_tree * n1, const ioctl_tree * n2)
{
    return n1->type == n2->type && memcmp(n1->data, n2->data, NSIZE(n1)) == 0;
}

static int
ioctl_simplestruct_in_execute(const ioctl_tree * node, unsigned long id, void *arg, int *ret)
{
    if (id == node->id) {
	memcpy(arg, node->data, NSIZE(node));
	*ret = node->ret;
	return 1;
    }

    return 0;
}

/***********************************
 *
 * USBDEVFS_REAPURB
 *
 ***********************************/

static void
usbdevfs_reapurb_init_from_bin(ioctl_tree * node, const void *data)
{
    const struct usbdevfs_urb *urb = *((struct usbdevfs_urb **)data);
    struct usbdevfs_urb *copy;

    copy = calloc(sizeof(struct usbdevfs_urb), 1);
    memcpy(copy, urb, sizeof(struct usbdevfs_urb));
    /* we need to make a copy of the buffer */
    copy->buffer = calloc(urb->buffer_length, 1);
    memcpy(copy->buffer, urb->buffer, urb->buffer_length);
    node->data = copy;
}

static int
usbdevfs_reapurb_init_from_text(ioctl_tree * node, const char *data)
{
    struct usbdevfs_urb *info = calloc(sizeof(struct usbdevfs_urb), 1);
    int offset, result;
    unsigned type, endpoint;
    result = sscanf(data, "%u %u %i %u %i %i %i %n", &type, &endpoint,
		    &info->status, &info->flags, &info->buffer_length,
		    &info->actual_length, &info->error_count, &offset);
    /* ambiguity of counting or not %n */
    if (result < 7) {
	DBG("usbdevfs_reapurb_init_from_text: failed to parse record '%s'\n", data);
	free(info);
	return FALSE;
    }
    info->type = (unsigned char)type;
    info->endpoint = (unsigned char)endpoint;

    /* read buffer */
    info->buffer = calloc(info->buffer_length, 1);
    if (!read_hex(data + offset, info->buffer, info->buffer_length)) {
	DBG("usbdevfs_reapurb_init_from_text: failed to parse buffer '%s'\n", data + offset);
	free(info->buffer);
	free(info);
	return FALSE;
    };

    node->data = info;
    return TRUE;
}

static void
usbdevfs_reapurb_free_data(ioctl_tree * node)
{
    struct usbdevfs_urb *info = node->data;
    if (info != NULL) {
	if (info->buffer != NULL)
	    free(info->buffer);
	free(info);
    }
}

static void
usbdevfs_reapurb_write(const ioctl_tree * node, FILE * f)
{
    const struct usbdevfs_urb *urb = node->data;
    assert(node->data != NULL);
    fprintf(f, "%u %u %i %u %i %i %i ", (unsigned)urb->type,
	    (unsigned)urb->endpoint, urb->status, (unsigned)urb->flags,
	    urb->buffer_length, urb->actual_length, urb->error_count);
    write_hex(f, urb->buffer, urb->endpoint & 0x80 ? urb->actual_length : urb->buffer_length);
    /* for debugging test cases with ASCII contents */
    /*fwrite (urb->buffer, urb->endpoint & 0x80 ? urb->actual_length : urb->buffer_length, 1, f); */
}

static int
usbdevfs_reapurb_equal(const ioctl_tree * n1, const ioctl_tree * n2)
{
    struct usbdevfs_urb *u1 = n1->data;
    struct usbdevfs_urb *u2 = n2->data;

    /* never consider input URBs equal as that might give a mismatch between
     * identical SUBMITs and different REAPs */
    if (u1->endpoint & 0x80 || u2->endpoint & 0x80)
        return FALSE;

    return u1->type == u2->type && u1->endpoint == u2->endpoint &&
	u1->status == u2->status && u1->flags == u2->flags &&
	u1->buffer_length == u2->buffer_length &&
	u1->actual_length == u2->actual_length && memcmp(u1->buffer, u2->buffer, u1->buffer_length) == 0;
}

static int
usbdevfs_reapurb_execute(const ioctl_tree * node, unsigned long id, void *arg, int *ret)
{
    /* set in SUBMIT, cleared in REAP */
    static const ioctl_tree *submit_node = NULL;
    static struct usbdevfs_urb *submit_urb = NULL;

    if (id == USBDEVFS_SUBMITURB) {
	const struct usbdevfs_urb *n_urb = node->data;
	struct usbdevfs_urb *a_urb = arg;
	assert(submit_node == NULL);

	if (n_urb->type != a_urb->type || n_urb->endpoint != a_urb->endpoint ||
	    n_urb->flags != a_urb->flags || n_urb->buffer_length != a_urb->buffer_length)
	    return 0;

	DBG("  usbdevfs_reapurb_execute: handling SUBMITURB, metadata match\n");

	/* for an output URB we also require the buffer contents to match; for
	 * an input URB it can be uninitialized */
	if ((n_urb->endpoint & 0x80) == 0 && memcmp(n_urb->buffer, a_urb->buffer, n_urb->buffer_length) != 0) {
	    DBG("  usbdevfs_reapurb_execute: handling SUBMITURB, buffer mismatch, rejecting\n");
	    return 0;
	}
	DBG("  usbdevfs_reapurb_execute: handling SUBMITURB, buffer match, remembering\n");

	/* remember the node for the next REAP */
	submit_node = node;
	submit_urb = a_urb;
	*ret = 0;
	return 1;
    }

    if (id == node->type->id) {
	struct usbdevfs_urb *orig_node_urb;
	if (submit_node == NULL) {
	    DBG("  usbdevfs_reapurb_execute: handling %s, but no submit node -> EAGAIN\n", node->type->name);
	    *ret = -1;
	    errno = EAGAIN;
	    return 2;
	}
	orig_node_urb = submit_node->data;

	submit_urb->actual_length = orig_node_urb->actual_length;
	submit_urb->error_count = orig_node_urb->error_count;
	/* for an input EP we need to copy the buffer data */
	if (orig_node_urb->endpoint & 0x80) {
	    memcpy(submit_urb->buffer, orig_node_urb->buffer, orig_node_urb->actual_length);
	}
	submit_urb->status = orig_node_urb->status;
	*((struct usbdevfs_urb **)arg) = submit_urb;

	DBG("  usbdevfs_reapurb_execute: handling %s %u %u %i %u %i %i %i ",
	    node->type->name, (unsigned)submit_urb->type,
	    (unsigned)submit_urb->endpoint, submit_urb->status,
	    (unsigned)submit_urb->flags, submit_urb->buffer_length, submit_urb->actual_length, submit_urb->error_count);
	IFDBG(write_hex(stdout, submit_urb->buffer, submit_urb->endpoint & 0x80 ?
			submit_urb->actual_length : submit_urb->buffer_length));

	submit_urb = NULL;
	submit_node = NULL;
	*ret = 0;
	return 2;
    }

    return 0;
}

static ioctl_tree *
usbdevfs_reapurb_insertion_parent(ioctl_tree * tree, ioctl_tree * node)
{
    if (((struct usbdevfs_urb *)node->data)->endpoint & 0x80) {
	/* input node: child of last REAPURB request */
	ssize_t i;
	ioctl_tree *t;
	for (i = tree->last_added->n - 1; i >= 0; --i) {
	    t = ioctl_node_list_get(tree->last_added, i);
	    if (t->type->id == USBDEVFS_REAPURB || t->type->id == USBDEVFS_REAPURBNDELAY)
		return t;
	}
	/* no REAPURB so far, make it a top level node */
	return tree;
    } else {
	/* output node: top level node */
	return tree;
    }
}

/***********************************
 *
 * generic implementations for hardware/state independent ioctls
 *
 ***********************************/

static int
ioctl_execute_success(const ioctl_tree * node, unsigned long id, void *arg, int *ret)
{
    errno = 0;
    *ret = 0;
    return 1;
}

static int
ioctl_execute_enodata(const ioctl_tree * node, unsigned long id, void *arg, int *ret)
{
    errno = ENODATA;
    *ret = -1;
    return 1;
}

static int
ioctl_execute_enotty(const ioctl_tree * node, unsigned long id, void *arg, int *ret)
{
    errno = ENOTTY;
    *ret = -1;
    return 1;
}

static ioctl_tree *
ioctl_insertion_parent_stateless(ioctl_tree * tree, ioctl_tree * node)
{
    return tree;
}

/***********************************
 *
 * Known ioctls
 *
 ***********************************/

#define I_NOSTATE(name, execute_result) \
    {name, -1, 0, #name, NULL, NULL, NULL, NULL, NULL, ioctl_execute_ ## execute_result, NULL}

#define I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, namestr, size, nr_range, insertion_parent_fn) \
    {name, size, nr_range, namestr,                                            \
     ioctl_simplestruct_init_from_bin, ioctl_simplestruct_init_from_text,      \
     ioctl_simplestruct_free_data,                                             \
     ioctl_simplestruct_write, ioctl_simplestruct_equal,                       \
     ioctl_simplestruct_in_execute, insertion_parent_fn}

#define I_NAMED_SIMPLE_STRUCT_IN(name, namestr, nr_range, insertion_parent_fn) \
    I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, namestr, -1, nr_range, insertion_parent_fn)

#define I_SIZED_SIMPLE_STRUCT_IN(name, size, nr_range, insertion_parent_fn) \
    I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, #name, size, nr_range, insertion_parent_fn)

#define I_SIMPLE_STRUCT_IN(name, nr_range, insertion_parent_fn) \
    I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, #name, -1, nr_range, insertion_parent_fn)

#define I_CUSTOM(name, nr_range, fn_prefix)                     \
    {name, -1, nr_range, #name,                                 \
     fn_prefix ## _init_from_bin, fn_prefix ## _init_from_text, \
     fn_prefix ## _free_data,                                   \
     fn_prefix ## _write, fn_prefix ## _equal,                  \
     fn_prefix ## _execute, fn_prefix ## _insertion_parent}

ioctl_type ioctl_db[] = {
    I_SIMPLE_STRUCT_IN(USBDEVFS_CONNECTINFO, 0, ioctl_insertion_parent_stateless),

    /* we assume that every SUBMITURB is followed by a REAPURB and that
     * ouput EPs don't change the buffer, so we ignore USBDEVFS_SUBMITURB */
    I_CUSTOM(USBDEVFS_REAPURB, 0, usbdevfs_reapurb),
    I_CUSTOM(USBDEVFS_REAPURBNDELAY, 0, usbdevfs_reapurb),
#ifdef USBDEVFS_GET_CAPABILITIES
    I_SIMPLE_STRUCT_IN(USBDEVFS_GET_CAPABILITIES, 0, ioctl_insertion_parent_stateless),
#endif

    /* hardware/state independent ioctls */
    I_NOSTATE(USBDEVFS_CLAIMINTERFACE, success),
    I_NOSTATE(USBDEVFS_RELEASEINTERFACE, success),
    I_NOSTATE(USBDEVFS_CLEAR_HALT, success),
    I_NOSTATE(USBDEVFS_RESET, success),
    I_NOSTATE(USBDEVFS_RESETEP, success),
    I_NOSTATE(USBDEVFS_GETDRIVER, enodata),
    I_NOSTATE(USBDEVFS_IOCTL, enotty),

    I_NOSTATE(EVIOCGRAB, success),

    /* evdev */
    I_SIMPLE_STRUCT_IN(EVIOCGVERSION, 0, ioctl_insertion_parent_stateless),
    I_SIMPLE_STRUCT_IN(EVIOCGID, 0, ioctl_insertion_parent_stateless),
    I_SIMPLE_STRUCT_IN(EVIOCGREP, 0, ioctl_insertion_parent_stateless),
    I_SIMPLE_STRUCT_IN(EVIOCGKEYCODE, 0, ioctl_insertion_parent_stateless),
    I_SIMPLE_STRUCT_IN(EVIOCGKEYCODE_V2, 0, ioctl_insertion_parent_stateless),
    I_SIMPLE_STRUCT_IN(EVIOCGEFFECTS, 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGABS(0), "EVIOCGABS", ABS_MAX, ioctl_insertion_parent_stateless),
    /* we define these with len==32, but they apply to any len */
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGBIT(0, 32), "EVIOCGBIT", EV_MAX, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGNAME(32), "EVIOCGNAME", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGPHYS(32), "EVIOCGPHYS", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGUNIQ(32), "EVIOCGUNIQ", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGPROP(32), "EVIOCGPROP", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGKEY(32), "EVIOCGKEY", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGLED(32), "EVIOCGLED", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGSND(32), "EVIOCGSND", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGSW(32), "EVIOCGSW", 0, ioctl_insertion_parent_stateless),

    /* this was introduced not too long ago */
#ifdef EVIOCGMTSLOTS
    I_NAMED_SIMPLE_STRUCT_IN(EVIOCGMTSLOTS(32), "EVIOCGMTSLOTS", 0, ioctl_insertion_parent_stateless),
#endif

    /* terminator */
    {0, 0, 0, "", NULL, NULL, NULL, NULL, NULL}
};

const ioctl_type *
ioctl_type_get_by_id(unsigned long id)
{
    ioctl_type *cur;
    for (cur = ioctl_db; cur->name[0] != '\0'; ++cur)
	if (id_matches_type(id, cur))
	    return cur;
    return NULL;
}

const ioctl_type *
ioctl_type_get_by_name(const char *name, unsigned long *out_id)
{
    ioctl_type *cur;
    char *parens;
    char *real_name;
    const ioctl_type *result = NULL;
    long offset = 0;

    /* chop off real name from offset */
    real_name = strdup(name);
    parens = strchr(real_name, '(');
    if (parens != NULL) {
	*parens = '\0';
	offset = atol(parens + 1);
    }

    for (cur = ioctl_db; cur->name[0] != '\0'; ++cur)
	if (strcmp(cur->name, real_name) == 0) {
	    if (out_id != NULL)
		*out_id = cur->id + offset;
	    result = cur;
	    break;
	}

    free(real_name);
    return result;
}
