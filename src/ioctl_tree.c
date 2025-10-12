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
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <err.h>
#include <linux/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/input.h>
#include <linux/hidraw.h>

#include "debug.h"
#include "utils.h"
#include "cros_ec.h"
#include "ioctl_tree.h"

#define TRUE 1
#define FALSE 0

#define UNUSED __attribute__ ((unused))


/***********************************
 *
 * ioctl_tree
 *
 ***********************************/

ioctl_tree *
ioctl_tree_new_from_bin(IOCTL_REQUEST_TYPE id, const void *data, int ret)
{
    const ioctl_type *type;
    ioctl_tree *t;

    type = ioctl_type_get_by_id(id);
    if (type == NULL) {
	DBG(DBG_IOCTL_TREE, "ioctl_tree_new_from_bin: unknown ioctl %X\n", (unsigned) id);
	return NULL;
    }
    /* state independent ioctl? */
    if (type->init_from_bin == NULL)
	return NULL;

    t = callocx(sizeof(ioctl_tree), 1);
    t->type = type;
    t->ret = ret;
    t->id = id;
    type->init_from_bin(t, data);
    return t;
}

ioctl_tree *
ioctl_tree_new_from_text(const char *line)
{
    static char lead_ws[1001];
    static char ioctl_name[101];
    int ret, offset;
    IOCTL_REQUEST_TYPE id;
    const ioctl_type *type;
    ioctl_tree *t;

    if (line[0] == ' ') {
	if (sscanf(line, "%1000[ ]%100s %i %n", lead_ws, ioctl_name, &ret, &offset) < 2) {
	    DBG(DBG_IOCTL_TREE, "ioctl_tree_new_from_text: failed to parse indent, ioctl name, and return value from '%s'\n", line);
	    return NULL;
	}
    } else {
	if (sscanf(line, "%100s %i %n", ioctl_name, &ret, &offset) < 1) {
	    DBG(DBG_IOCTL_TREE, "ioctl_tree_new_from_text: failed to parse ioctl name and return value from '%s'\n", line);
	    return NULL;
	}
	lead_ws[0] = '\0';
    }

    type = ioctl_type_get_by_name(ioctl_name, &id);
    if (type == NULL) {
	DBG(DBG_IOCTL_TREE, "ioctl_tree_new_from_text: unknown ioctl %s\n", ioctl_name);
	return NULL;
    }

    t = callocx(sizeof(ioctl_tree), 1);
    t->type = type;
    t->depth = strlen(lead_ws);
    t->ret = ret;
    t->id = id;
    if (!type->init_from_text(t, line + offset)) {
	DBG(DBG_IOCTL_TREE, "ioctl_tree_new_from_text: ioctl %s failed to initialize from data '%s'\n", ioctl_name, line + offset);
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
 * Insert the node and return the (new) root of the tree. If an equal node
 * already exist, the node will not be inserted.
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
	return node;
    }

    /* trying to insert into itself? */
    assert(tree != node);

    existing = ioctl_tree_find_equal(tree, node);
    if (existing) {
	DBG(DBG_IOCTL_TREE, "ioctl_tree_insert: node of type %s ptr %p already exists\n", node->type->name, node);
	ioctl_node_list_append(tree->last_added, existing);
	ioctl_tree_free(node);
	return tree;
    }

    node->parent = node->type->insertion_parent(tree, node);
    if (node->parent == NULL)
	errx(EXIT_FAILURE, "ioctl_tree_insert: did not get insertion parent for node type %s ptr %p",
	     node->type->name, node);

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
    return tree;
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

        /* skip lines with metadata */
        if (line[0] == '@')
            continue;

	node = ioctl_tree_new_from_text(line);
	if (node == NULL) {
	    DBG(DBG_IOCTL_TREE, "ioctl_tree_read: failure to parse line: %s", line);
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
    int res;
    if (tree == NULL)
	return;

    /* write indent */
    for (int i = 0; i < tree->depth; ++i)
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
    res = fputc('\n', f);
    assert(res == '\n');

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
    l = mallocx(sizeof(ioctl_node_list));
    l->n = 0;
    l->capacity = 10;
    l->items = callocx(sizeof(ioctl_tree *), l->capacity);
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
	assert(list->capacity < SIZE_MAX / 2);
	list->capacity *= 2;
	list->items = reallocarray(list->items, list->capacity, sizeof(ioctl_tree *));
	assert(list->items != NULL);
    }

    list->items[list->n++] = element;
}

ioctl_tree *
ioctl_tree_execute(ioctl_tree * tree, ioctl_tree * last, IOCTL_REQUEST_TYPE id, void *arg, int *ret)
{
    const ioctl_type *t;
    ioctl_tree *i;
    int r, handled;

    /* initialize return code */
    assert(ret != NULL);
    *ret = -1;

    DBG(DBG_IOCTL_TREE, "ioctl_tree_execute ioctl %X\n", (unsigned) id);

    t = ioctl_type_get_by_id(id);
    /* Ignore dummy entries that only exist for size resolution. */
    if (t && t->execute == NULL)
	t = NULL;

    /* check if it's a hardware independent stateless ioctl */
    if (t != NULL && t->insertion_parent == NULL) {
	DBG(DBG_IOCTL_TREE, "  ioctl_tree_execute: stateless\n");
	if (t->execute(NULL, id, arg, &r))
	    *ret = r;
	return last;
    }

    if (tree == NULL)
	return NULL;

    i = ioctl_tree_next_wrap(tree, last);
    /* start at the previously executed node to maintain original order of
     * ioctls as much as possible (i. e. maintain it while the requests come in
     * at the same order as originally recorded) */
    for (;;) {
	DBG(DBG_IOCTL_TREE, "   ioctl_tree_execute: checking node %s(%X, base id %X) ", i->type->name, (unsigned) i->id, (unsigned) i->type->id);
	if (debug_categories & DBG_IOCTL_TREE)
	    i->type->write(i, stderr);
	DBG(DBG_IOCTL_TREE, "\n");
	handled = i->type->execute(i, id, arg, &r);
	if (handled) {
	    DBG(DBG_IOCTL_TREE, "    -> match, ret %i, adv: %i\n", r, handled);
	    *ret = r;
	    if (handled == 1)
		return i;
	    else
		return last;
	}

	if (last != NULL && i == last) {
	    /* we did a full circle */
	    DBG(DBG_IOCTL_TREE, "    -> full iteration, not found\n");
	    break;
	}

	i = ioctl_tree_next_wrap(tree, i);

	if (last == NULL && i == tree) {
	    /* we did a full circle */
	    DBG(DBG_IOCTL_TREE, "    -> full iteration with last == NULL, not found\n");
	    break;
	}
    }

    /* not found */
    return NULL;
}

int
ioctl_tree_next_ret(ioctl_tree * tree, ioctl_tree * last)
{
    const ioctl_tree *i = ioctl_tree_next_wrap(tree, last);

    if (i == NULL) {
        return 0;
    }

    return i->ret;
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
	    DBG(DBG_IOCTL_TREE, "read_hex: data is larger than buffer size %zu\n", bufsize);
	    return FALSE;
	}

	lower = hexdigit(hexptr[1]);
	if (lower < 0) {
	    DBG(DBG_IOCTL_TREE, "read_hex: data has odd number of digits: '%s'\n", hex);
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

#define TSIZE(type, id) ((type && type->real_size >= 0) ? type->real_size : _IOC_SIZE(id))
#define NSIZE(node) TSIZE(node->type, node->id)

static inline int
id_matches_type(IOCTL_REQUEST_TYPE id, const ioctl_type *type)
{
    return _IOC_TYPE(id) == _IOC_TYPE(type->id) &&
           _IOC_DIR(id) == _IOC_DIR(type->id) &&
           _IOC_NR(id) >= _IOC_NR(type->id) &&
           _IOC_NR(id) <= _IOC_NR(type->id) + type->nr_range;
}

static void
ioctl_simplestruct_init_from_bin(ioctl_tree * node, const void *data)
{
    DBG(DBG_IOCTL_TREE, "ioctl_simplestruct_init_from_bin: %s(%X): size is %u bytes\n", node->type->name, (unsigned) node->id, (unsigned) NSIZE(node));
    node->data = mallocx(NSIZE(node));
    memcpy(node->data, data, NSIZE(node));
}

static int
ioctl_simplestruct_init_from_text(ioctl_tree * node, const char *data)
{
    /* node->id is initialized at this point, but does not necessarily have the
     * correct length for data; this happens for variable length ioctls such as
     * EVIOCGBIT */
    size_t data_len = strlen(data) / 2;
    node->data = mallocx(data_len);

    if (NSIZE(node) != data_len) {
	DBG(DBG_IOCTL_TREE, "ioctl_simplestruct_init_from_text: adjusting ioctl ID %X (size %u) to actual data length %zu\n",
	    (unsigned) node->id, (unsigned) NSIZE(node), data_len);
	node->id = _IOC(_IOC_DIR(node->id), _IOC_TYPE(node->id), _IOC_NR(node->id), data_len);
    }

    if (!read_hex(data, node->data, NSIZE(node))) {
	DBG(DBG_IOCTL_TREE, "ioctl_simplestruct_init_from_text: failed to parse '%s'\n", data);
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
ioctl_simplestruct_in_execute(const ioctl_tree * node, IOCTL_REQUEST_TYPE id, void *arg, int *ret)
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
 * ioctls with dynamic length struct data, but no pointers to substructs
 *
 ***********************************/

static void
ioctl_varlenstruct_init_from_bin(ioctl_tree * node, const void *data)
{
    size_t size = node->type->get_data_size(node->id, data);
    DBG(DBG_IOCTL_TREE, "ioctl_varlenstruct_init_from_bin: %s(%X): size is %zu bytes\n", node->type->name, (unsigned) node->id, size);
    node->data = mallocx(size);
    memcpy(node->data, data, size);
}

static int
ioctl_varlenstruct_init_from_text(ioctl_tree * node, const char *data)
{
    size_t data_len = strlen(data) / 2;

    node->data = mallocx(data_len);

    if (!read_hex(data, node->data, data_len)) {
	fprintf(stderr, "ioctl_varlenstruct_init_from_text: failed to parse '%s'\n", data);
	free(node->data);
	return FALSE;
    }

    /* verify that the text data size actually matches get_data_size() */
    size_t size = node->type->get_data_size(node->id, node->data);

    if (size != data_len) {
	fprintf(stderr, "ioctl_varlenstruct_init_from_text: ioctl %X: expected data length %zu, but got %zu bytes from text data\n",
		(unsigned) node->id, size, data_len);
	free(node->data);
	return FALSE;
    }

    return TRUE;
}

static void
ioctl_varlenstruct_write(const ioctl_tree * node, FILE * f)
{
    assert(node->data != NULL);
    write_hex(f, node->data, node->type->get_data_size(node->id, node->data));
}

static int
ioctl_varlenstruct_equal(const ioctl_tree * n1, const ioctl_tree * n2)
{
    size_t size1 = n1->type->get_data_size(n1->id, n1->data);
    size_t size2 = n2->type->get_data_size(n2->id, n2->data);
    return n1->id == n2->id && size1 == size2 && memcmp(n1->data, n2->data, size1) == 0;
}

static int
ioctl_varlenstruct_in_execute(const ioctl_tree * node, IOCTL_REQUEST_TYPE id, void *arg, int *ret)
{
    if (id == node->id) {
	size_t size = node->type->get_data_size(id, node->data);
	memcpy(arg, node->data, size);
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

    copy = callocx(sizeof(struct usbdevfs_urb), 1);
    memcpy(copy, urb, sizeof(struct usbdevfs_urb));
    /* we need to make a copy of the buffer */
    copy->buffer = callocx(urb->buffer_length, 1);
    memcpy(copy->buffer, urb->buffer, urb->buffer_length);
    node->data = copy;
}

static int
usbdevfs_reapurb_init_from_text(ioctl_tree * node, const char *data)
{
    struct usbdevfs_urb *info = callocx(sizeof(struct usbdevfs_urb), 1);
    int offset, result;
    unsigned type, endpoint;
    result = sscanf(data, "%u %u %i %u %i %i %i %n", &type, &endpoint,
		    &info->status, &info->flags, &info->buffer_length,
		    &info->actual_length, &info->error_count, &offset);
    /* ambiguity of counting or not %n */
    if (result < 7) {
	DBG(DBG_IOCTL_TREE, "usbdevfs_reapurb_init_from_text: failed to parse record '%s'\n", data);
	free(info);
	return FALSE;
    }
    info->type = (unsigned char)type;
    info->endpoint = (unsigned char)endpoint;

    /* read buffer */
    info->buffer = callocx(info->buffer_length, 1);
    if (!read_hex(data + offset, info->buffer, info->buffer_length)) {
	DBG(DBG_IOCTL_TREE, "usbdevfs_reapurb_init_from_text: failed to parse buffer '%s'\n", data + offset);
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
usbdevfs_reapurb_execute(const ioctl_tree * node, IOCTL_REQUEST_TYPE id, void *arg, int *ret)
{
    /* set in SUBMIT, cleared in REAP */
    static const ioctl_tree *submit_node = NULL;
    static struct usbdevfs_urb *submit_urb = NULL;

    /* have to cast here, as with musl USBDEVFS* have the wrong type "unsigned long" */
    if (id == (IOCTL_REQUEST_TYPE) USBDEVFS_SUBMITURB) {
	const struct usbdevfs_urb *n_urb = node->data;
	struct usbdevfs_urb *a_urb = arg;
	assert(submit_node == NULL);

	if (n_urb->type != a_urb->type || n_urb->endpoint != a_urb->endpoint ||
	    n_urb->flags != a_urb->flags || n_urb->buffer_length != a_urb->buffer_length)
	    return 0;

	DBG(DBG_IOCTL_TREE, "  usbdevfs_reapurb_execute: handling SUBMITURB, metadata match\n");

	/* for an output URB we also require the buffer contents to match; for
	 * an input URB it can be uninitialized */
	if ((n_urb->endpoint & 0x80) == 0 && memcmp(n_urb->buffer, a_urb->buffer, n_urb->buffer_length) != 0) {
	    DBG(DBG_IOCTL_TREE, "  usbdevfs_reapurb_execute: handling SUBMITURB, buffer mismatch, rejecting\n");
	    return 0;
	}
	DBG(DBG_IOCTL_TREE, "  usbdevfs_reapurb_execute: handling SUBMITURB, buffer match, remembering\n");

	/* remember the node for the next REAP */
	submit_node = node;
	submit_urb = a_urb;
	*ret = 0;
	return 1;
    }

    if (id == node->type->id) {
	struct usbdevfs_urb *orig_node_urb;
	if (submit_node == NULL) {
	    DBG(DBG_IOCTL_TREE, "  usbdevfs_reapurb_execute: handling %s, but no submit node -> EAGAIN\n", node->type->name);
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

	DBG(DBG_IOCTL_TREE, "  usbdevfs_reapurb_execute: handling %s %u %u %i %u %i %i %i ",
	    node->type->name, (unsigned)submit_urb->type,
	    (unsigned)submit_urb->endpoint, submit_urb->status,
	    (unsigned)submit_urb->flags, submit_urb->buffer_length, submit_urb->actual_length, submit_urb->error_count);
	if (debug_categories & DBG_IOCTL_TREE)
	    write_hex(stderr, submit_urb->buffer, submit_urb->endpoint & 0x80 ?
		    submit_urb->actual_length : submit_urb->buffer_length);

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
ioctl_execute_success(UNUSED const ioctl_tree * _node, UNUSED IOCTL_REQUEST_TYPE _id, UNUSED void *_arg, int *ret)
{
    errno = 0;
    *ret = 0;
    return 1;
}

static int
ioctl_execute_enodata(UNUSED const ioctl_tree * _node, UNUSED IOCTL_REQUEST_TYPE _id, UNUSED void *_arg, int *ret)
{
    errno = ENODATA;
    *ret = -1;
    return 1;
}

static int
ioctl_execute_enotty(UNUSED const ioctl_tree * _node, UNUSED IOCTL_REQUEST_TYPE _id, UNUSED void *_arg, int *ret)
{
    errno = ENOTTY;
    *ret = -1;
    return 1;
}

static ioctl_tree *
ioctl_insertion_parent_stateless(ioctl_tree * tree, UNUSED ioctl_tree *_node)
{
    return tree;
}


/***********************************
 *
 * Known ioctls
 *
 ***********************************/

/* ioctl which does not need to consider its data argument and has no state */
#define I_NOSTATE(name, execute_result) \
    {name, 0, 0, #name, NULL, NULL, NULL, NULL, NULL, ioctl_execute_ ## execute_result, NULL}

/* input structs with fixed length with explicit size */
#define I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, namestr, size, nr_range, insertion_parent_fn) \
    {name, size, nr_range, namestr,                                            \
     ioctl_simplestruct_init_from_bin, ioctl_simplestruct_init_from_text,      \
     ioctl_simplestruct_free_data,                                             \
     ioctl_simplestruct_write, ioctl_simplestruct_equal,                       \
     ioctl_simplestruct_in_execute, insertion_parent_fn, NULL}

#define I_SIZED_SIMPLE_STRUCT_IN(name, size, nr_range, insertion_parent_fn) \
    I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, #name, size, nr_range, insertion_parent_fn)

/* input structs with fixed length with size encoded in ioctl code */
#define I_NAMED_SIMPLE_STRUCT_IN(name, namestr, nr_range, insertion_parent_fn) \
    I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, namestr, -1, nr_range, insertion_parent_fn)

#define I_SIMPLE_STRUCT_IN(name, nr_range, insertion_parent_fn) \
    I_NAMED_SIZED_SIMPLE_STRUCT_IN(name, #name, -1, nr_range, insertion_parent_fn)

/* input structs with a variable length (but no pointers to substructures) */
#define I_VARLEN_STRUCT_IN(name, equal_fn, insertion_parent_fn, data_size_fn) \
    {name, -1, 0, #name,                                                       \
     ioctl_varlenstruct_init_from_bin, ioctl_varlenstruct_init_from_text,      \
     ioctl_simplestruct_free_data,                                             \
     ioctl_varlenstruct_write, equal_fn,                       \
     ioctl_varlenstruct_in_execute, insertion_parent_fn, data_size_fn}

/* data with custom handlers; necessary for structs with pointers to nested
 * structs, or keeping stateful handlers */
#define I_CUSTOM(name, size, nr_range, fn_prefix)               \
    {name, size, nr_range, #name,                  \
     fn_prefix ## _init_from_bin, fn_prefix ## _init_from_text, \
     fn_prefix ## _free_data,                                   \
     fn_prefix ## _write, fn_prefix ## _equal,                  \
     fn_prefix ## _execute, fn_prefix ## _insertion_parent}

#define I_DUMMY(name, size, nr_range)               \
    {name, size, nr_range, #name,                  \
     NULL, NULL, NULL, NULL, NULL, NULL, NULL}     \

static int
cros_ec_ioctl_equal(UNUSED const ioctl_tree *_n1, UNUSED const ioctl_tree *_n2)
{
  return FALSE;
}

static size_t
cros_ec_ioctl_get_data_size(IOCTL_REQUEST_TYPE _id, const void *data)
{
    const struct cros_ec_command_v2 *s_cmd = (struct cros_ec_command_v2 *)data;

    return sizeof(struct cros_ec_command_v2) + le32toh(s_cmd->insize);
}

ioctl_type ioctl_db[] = {
    I_SIMPLE_STRUCT_IN(USBDEVFS_CONNECTINFO, 0, ioctl_insertion_parent_stateless),

    /* we assume that every SUBMITURB is followed by a REAPURB and that
     * ouput EPs don't change the buffer, so we ignore USBDEVFS_SUBMITURB */
    I_DUMMY(USBDEVFS_SUBMITURB, -1, 0),
    I_CUSTOM(USBDEVFS_REAPURB, -1, 0, usbdevfs_reapurb),
    I_CUSTOM(USBDEVFS_REAPURBNDELAY, -1, 0, usbdevfs_reapurb),
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

    /* hidraw */
    I_SIMPLE_STRUCT_IN(HIDIOCGRDESCSIZE, 0, ioctl_insertion_parent_stateless),
    I_SIMPLE_STRUCT_IN(HIDIOCGRDESC, 0, ioctl_insertion_parent_stateless),
    I_SIMPLE_STRUCT_IN(HIDIOCGRAWINFO, 0, ioctl_insertion_parent_stateless),
    /* we define these with len==32, but they apply to any len */
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCGRAWNAME(32), "HIDIOCGRAWNAME", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCGRAWPHYS(32), "HIDIOCGRAWPHYS", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCSFEATURE(32), "HIDIOCSFEATURE", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCGFEATURE(32), "HIDIOCGFEATURE", 0, ioctl_insertion_parent_stateless),
    /* this was introduced not too long ago */
#ifdef HIDIOCGRAWUNIQ
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCGRAWUNIQ(32), "HIDIOCGRAWUNIQ", 0, ioctl_insertion_parent_stateless),
#endif
    /* these were introduced not too long ago */
#ifdef HIDIOCSINPUT
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCSINPUT(32), "HIDIOCSINPUT", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCGINPUT(32), "HIDIOCGINPUT", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCSOUTPUT(32), "HIDIOCSOUTPUT", 0, ioctl_insertion_parent_stateless),
    I_NAMED_SIMPLE_STRUCT_IN(HIDIOCGOUTPUT(32), "HIDIOCGOUTPUT", 0, ioctl_insertion_parent_stateless),
#endif

    /* cros_ec */
    I_VARLEN_STRUCT_IN(CROS_EC_DEV_IOCXCMD_V2, cros_ec_ioctl_equal, ioctl_insertion_parent_stateless, cros_ec_ioctl_get_data_size),
    I_NOSTATE(CROS_EC_DEV_IOCEVENTMASK_V2, enodata),

    /* terminator */
    {0, 0, 0, "", NULL, NULL, NULL, NULL, NULL}
};

const ioctl_type *
ioctl_type_get_by_id(IOCTL_REQUEST_TYPE id)
{
    ioctl_type *cur;
    for (cur = ioctl_db; cur->name[0] != '\0'; ++cur)
	if (id_matches_type(id, cur))
	    return cur;
    return NULL;
}

int ioctl_data_size_by_id(IOCTL_REQUEST_TYPE id)
{
    ioctl_type *cur;
    for (cur = ioctl_db; cur->name[0] != '\0'; ++cur)
	if (id_matches_type(id, cur))
	    return TSIZE(cur, id);
    return 0;
}

const ioctl_type *
ioctl_type_get_by_name(const char *name, IOCTL_REQUEST_TYPE *out_id)
{
    ioctl_type *cur;
    char *parens;
    char *real_name;
    const ioctl_type *result = NULL;
    long offset = 0;

    /* chop off real name from offset */
    real_name = strdupx(name);
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
