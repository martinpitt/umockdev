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

#ifndef __IOCTL_TREE_H
#    define __IOCTL_TREE_H

struct ioctl_tree;
typedef struct ioctl_tree ioctl_tree;

typedef struct {
    unsigned long id;
    ssize_t real_size;		/* for legacy ioctls with _IOC_SIZE == 0 */
    unsigned long nr_range;
    const char name[100];
    void (*init_from_bin) (ioctl_tree *, const void *);
    int (*init_from_text) (ioctl_tree *, const char *);
    void (*free_data) (ioctl_tree *);
    void (*write) (const ioctl_tree *, FILE *);
    int (*equal) (const ioctl_tree *, const ioctl_tree *);
    /* ret: 0: unhandled, 1: handled, move to next node, 2: handled, keep node */
    int (*execute) (const ioctl_tree *, unsigned long, void *, int *);
    ioctl_tree *(*insertion_parent) (ioctl_tree *, ioctl_tree *);
} ioctl_type;

typedef struct {
    ssize_t n;
    ssize_t capacity;
    ioctl_tree **items;
} ioctl_node_list;

struct ioctl_tree {
    const ioctl_type *type;
    int depth;
    void *data;
    int ret;
    unsigned long id;		/* usually type->id, but needed for patterns like EVIOCGABS(abs) */
    ioctl_tree *child;
    ioctl_tree *next;		/* sibling */
    ioctl_tree *parent;

    /* below are internal private fields */
    ioctl_node_list *last_added;
};

ioctl_tree *ioctl_tree_new_from_bin(unsigned long id, const void *data, int ret);
ioctl_tree *ioctl_tree_new_from_text(const char *line);
void ioctl_tree_free(ioctl_tree * tree);
ioctl_tree *ioctl_tree_read(FILE * f);
void ioctl_tree_write(FILE * f, const ioctl_tree * tree);
ioctl_tree *ioctl_tree_insert(ioctl_tree * tree, ioctl_tree * node);
ioctl_tree *ioctl_tree_find_equal(ioctl_tree * tree, ioctl_tree * node);
ioctl_tree *ioctl_tree_next(const ioctl_tree * node);
ioctl_tree *ioctl_tree_execute(ioctl_tree * tree, ioctl_tree * last, unsigned long id, void *arg, int *ret);

/* node lists */
ioctl_node_list *ioctl_node_list_new(void);
void ioctl_node_list_free(ioctl_node_list * list);
void ioctl_node_list_append(ioctl_node_list * list, ioctl_tree * element);

static inline ioctl_tree *
ioctl_node_list_get(ioctl_node_list * list, ssize_t n)
{
    /* negative values count from the end */
    return (n >= 0) ? list->items[n] : list->items[list->n + n];
}

static inline ioctl_tree *
ioctl_tree_next_wrap(ioctl_tree * tree, ioctl_tree * node)
{
    if (node == NULL)
	return tree;
    ioctl_tree *t = ioctl_tree_next(node);
    return (t != NULL) ? t : tree;
}

/* database of known ioctls; return NULL for unknown ones */
const ioctl_type *ioctl_type_get_by_id(unsigned long id);
const ioctl_type *ioctl_type_get_by_name(const char *name, unsigned long *out_id);

#endif				/* __IOCTL_TREE_H */
