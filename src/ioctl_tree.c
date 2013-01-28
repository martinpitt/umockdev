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
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#include "ioctl_tree.h"

#ifdef DEBUG
#   define DBG(...) printf(__VA_ARGS__)
#   define IFDBG(x) x
#else
#   define DBG(...) {}
#   define IFDBG(x) {}
#endif

#define TRUE 1
#define FALSE 0


/***********************************
 *
 * ioctl_tree
 *
 ***********************************/

ioctl_tree*
ioctl_tree_new_from_bin (unsigned long id, const void* data)
{
    const ioctl_type* type;
    ioctl_tree* t;

    type = ioctl_type_get_by_id (id);
    if (type == NULL) {
        DBG ("ioctl_tree_new_from_bin: unknown ioctl %lu\n", id);
        return NULL;
    }
    t = calloc(sizeof (ioctl_tree), 1);
    t->type = type;
    type->init_from_bin (t, data);
    return t;
}

ioctl_tree*
ioctl_tree_new_from_text (const char* line)
{
    static char lead_ws[1000];
    static char ioctl_name[100];
    int         offset;
    const ioctl_type* type;
    ioctl_tree* t;

    if (line[0] == ' ') {
        if (sscanf (line, "%1000[ ]%100s %n", lead_ws, ioctl_name, &offset) < 2) {
            DBG ("ioctl_tree_new_from_text: failed to parse indent and ioctl name from '%s'\n", line);
            return NULL;
        }
    } else {
        if (sscanf (line, "%100s %n", ioctl_name, &offset) < 1) {
            DBG ("ioctl_tree_new_from_text: failed to parse ioctl name from '%s'\n", line);
            return NULL;
        }
        lead_ws[0] = '\0';
    }

    type = ioctl_type_get_by_name (ioctl_name);
    if (type == NULL) {
        DBG ("ioctl_tree_new_from_text: unknown ioctl %s\n", ioctl_name);
        return NULL;
    }

    t = calloc(sizeof (ioctl_tree), 1);
    t->type = type;
    t->depth = strlen (lead_ws);
    if (!type->init_from_text (t, line + offset)) {
        DBG ("ioctl_tree_new_from_text: ioctl %s failed to initialize from data '%s'\n",
             ioctl_name, line + offset);
        free (t);
        return NULL;
    }
    return t;
}

static ioctl_tree*
ioctl_tree_last_sibling (ioctl_tree* node)
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
ioctl_tree*
ioctl_tree_insert (ioctl_tree* tree, ioctl_tree* node)
{
    ioctl_tree* existing;

    /* creating the root element? */
    if (tree == NULL) {
        node->last_added = ioctl_node_list_new ();
        ioctl_node_list_append (node->last_added, node);
        return NULL;
    }

    /* trying to insert into itself? */
    assert (tree != node);

    existing = ioctl_tree_find_equal (tree, node);
    if (existing) {
        DBG ("ioctl_tree_insert: node of type %s ptr %p already exists\n", node->type->name, node);
        ioctl_node_list_append (tree->last_added, existing);
        return existing;
    }

    node->parent = node->type->insertion_parent (tree, node);
    if (node->parent == NULL) {
        fprintf (stderr, "ioctl_tree_insert: did not get insertion parent for node type %s ptr %p\n",
                 node->type->name, node);
        abort();
    }

    /* if the parent is the whole tree, then we put it as a sibling, not a
     * child */
    if (node->parent == tree) {
        ioctl_tree_last_sibling (tree)->next = node;
        node->depth = 0;
    } else {
        if (node->parent->child == NULL)
            node->parent->child = node;
        else
            ioctl_tree_last_sibling (node->parent->child)->next = node;

        node->depth = node->parent->depth + 1;
    }

    ioctl_node_list_append (tree->last_added, node);
    return NULL;
}

ioctl_tree*
ioctl_tree_read (FILE* f)
{
    ioctl_tree *tree = NULL;
    ioctl_tree *node, *prev = NULL;
    ioctl_tree *sibling;
    char *line = NULL;
    size_t line_len;

    while (getline (&line, &line_len, f) >= 0) {
        node = ioctl_tree_new_from_text (line);
        if (node == NULL) {
            DBG ("ioctl_tree_read: failure to parse line: %s", line);
            free (line);
            break;
        }

        if (tree == NULL) {
            tree = node;
            node->last_added = ioctl_node_list_new ();
        } else {
            /* insert at the right depth */
            if (node->depth > prev->depth) {
                assert (node->depth == prev->depth + 1);
                assert (prev->child == NULL);
                prev->child = node;
                node->parent = prev;
            } else {
                for (sibling = prev; sibling != NULL; sibling = sibling->parent) {
                    if (node->depth == sibling->depth) {
                        assert (sibling->next == NULL);
                        sibling->next = node;
                        //ioctl_tree_last_sibling (sibling)->next = node;
                        node->parent = sibling->parent;
                        break;
                    }
                }
            }
        }

        free (line);
        line = NULL;
        prev = node;
    }

    return tree;
}

void
ioctl_tree_write (FILE *f, const ioctl_tree* tree)
{
    int i;
    if (tree == NULL)
        return;

    /* write indent */
    for (i = 0; i < tree->depth; ++i)
        fputc (' ', f);
    fputs (tree->type->name, f);
    fputc (' ', f);
    tree->type->write (tree, f);
    assert (fputc ('\n', f) == '\n');

    ioctl_tree_write (f, tree->child);
    ioctl_tree_write (f, tree->next);
}

ioctl_tree*
ioctl_tree_find_equal (ioctl_tree* tree, ioctl_tree* node)
{
    ioctl_tree* t;

    if (node->type == tree->type && node->type->equal (node, tree))
        return tree;
    if (tree->child) {
        t = ioctl_tree_find_equal (tree->child, node);
        if (t != NULL)
            return t;
    }
    if (tree->next) {
        t = ioctl_tree_find_equal (tree->next, node);
        if (t != NULL)
            return t;
    }
    return NULL;
}

ioctl_tree*
ioctl_tree_next (const ioctl_tree* node)
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

ioctl_node_list*
ioctl_node_list_new (void)
{
    ioctl_node_list* l;
    l = malloc (sizeof (ioctl_node_list));
    l->n = 0;
    l->capacity = 10;
    l->items = calloc (sizeof (ioctl_tree*), l->capacity);
    return l;
}

void
ioctl_node_list_free (ioctl_node_list* list)
{
    free (list->items);
    list->items = NULL;
    free (list);
}

void
ioctl_node_list_append (ioctl_node_list* list, ioctl_tree* element)
{
    if (list->n == list->capacity) {
        list->capacity *= 2;
        list->items = realloc (list->items, list->capacity * sizeof (ioctl_tree*));
        assert (list->items != NULL);
    }

    list->items[list->n++] = element;
}

ioctl_tree*
ioctl_tree_execute (ioctl_tree* tree, ioctl_tree *last, unsigned long id,
                    void* arg, int* ret)
{
    ioctl_tree *i = ioctl_tree_next_wrap (tree, last);
    int r, handled;

    DBG ("ioctl_tree_execute ioctl %lu\n", id);

    /* start at the previously executed node to maintain original order of
     * ioctls as much as possible (i. e. maintain it while the requests come in
     * at the same order as originally recorded) */
    for (;;) {
        DBG ("   ioctl_tree_execute: checking node %s(%lu) ", i->type->name, i->type->id);
        IFDBG (i->type->write (i, stdout));
        DBG ("\n");
        if (i == last) {
            /* we did a full circle */
            DBG ("    -> full iteration, not found\n");
            break;
        }

        handled = i->type->execute (i, id, arg, &r);
        if (handled) {
            DBG ("    -> match, ret %i, adv: %i\n", r, handled);
            *ret = r;
            if (handled == 1)
                return i;
            else 
                return last;
        }

        i = ioctl_tree_next_wrap (tree, i);
    }

    /* not found */
    return NULL;
}

/***********************************
 *
 * Utility functions for ioctl implementations
 *
 ***********************************/

static inline char
hexdigit (char c)
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
read_hex (const char* hex, char* buf, size_t bufsize)
{
    const char *hexptr = hex;
    size_t written = 0;
    char upper, lower;

    while ((upper = hexdigit(hexptr[0])) >= 0) {
        if (written >= bufsize) {
            DBG ("read_hex: data is larger than buffer size %zu\n", bufsize);
            return FALSE;
        }

        lower = hexdigit(hexptr[1]);
        if (lower < 0) {
            DBG ("read_hex: data has odd number of digits: '%s'\n", hex);
            return FALSE;
        }
        buf[written++] = upper << 4 | lower;
        hexptr += 2;
    }
    return TRUE;
}

static void
write_hex (FILE* file, const char* buf, size_t len)
{
        size_t i;

        if (len == 0)
                return;

        for (i = 0; i < len; ++i)
                fprintf(file, "%02X", (unsigned) (unsigned char) buf[i]);
}

/***********************************
 *
 * USBDEVFS_CONNECTINFO
 *
 ***********************************/

static void
usbdevfs_connectinfo_init_from_bin (ioctl_tree* node, const void* data)
{
    node->data = malloc (sizeof (struct usbdevfs_connectinfo));
    memcpy (node->data, data, sizeof (struct usbdevfs_connectinfo));
}

static int
usbdevfs_connectinfo_init_from_text (ioctl_tree* node, const char* data)
{
    int slow;
    struct usbdevfs_connectinfo* info = malloc (sizeof (struct usbdevfs_connectinfo));
    if (sscanf (data, "%u %i\n", &info->devnum, &slow) != 2) {
        DBG ("usbdevfs_connectinfo_init_from_text: failed to parse '%s'\n", data);
        free (info);
        return FALSE;
    }
    info->slow = (char) slow;
    node->data = info;
    return TRUE;
}

static void
usbdevfs_connectinfo_write (const ioctl_tree* node, FILE* f)
{
    const struct usbdevfs_connectinfo* info = node->data;
    assert (node->data != NULL);
    fprintf (f, "%u %i", info->devnum, info->slow);
}

static int
usbdevfs_connectinfo_equal (const ioctl_tree* n1, const ioctl_tree *n2)
{
    return memcmp (n1->data, n2->data, sizeof (struct usbdevfs_connectinfo)) == 0;
}

static int
usbdevfs_connectinfo_execute (const ioctl_tree* node, unsigned long id, void* arg, int *ret)
{
    if (node->type->id == id) {
        memcpy (arg, node->data, sizeof (struct usbdevfs_connectinfo));
        *ret = 0;
        return 1;
    }

    return 0;
}

static ioctl_tree*
usbdevfs_connectinfo_insertion_parent (ioctl_tree* tree,
                                       ioctl_tree* node)
{
    /* stateless, always new top level item */
    return tree;
}

/***********************************
 *
 * USBDEVFS_REAPURB
 *
 ***********************************/

static void
usbdevfs_reapurb_init_from_bin (ioctl_tree* node, const void* data)
{
    const struct usbdevfs_urb* urb = *((struct usbdevfs_urb**) data);
    struct usbdevfs_urb* copy;

    copy = calloc (sizeof (struct usbdevfs_urb), 1);
    memcpy (copy, urb, sizeof (struct usbdevfs_urb));
    /* we need to make a copy of the buffer */
    copy->buffer = calloc (urb->buffer_length, 1);
    memcpy (copy->buffer, urb->buffer, urb->buffer_length);
    node->data = copy;
}

static int
usbdevfs_reapurb_init_from_text (ioctl_tree* node, const char* data)
{
    struct usbdevfs_urb* info = calloc (sizeof (struct usbdevfs_urb), 1);
    int offset, result;
    unsigned type, endpoint;
    result = sscanf (data, "%u %u %i %u %i %i %i %n", &type, &endpoint,
                     &info->status, &info->flags, &info->buffer_length,
                     &info->actual_length, &info->error_count, &offset);
    /* ambiguity of counting or not %n */
    if (result < 7) {
        DBG ("usbdevfs_reapurb_init_from_text: failed to parse record '%s'\n", data);
        free (info);
        return FALSE;
    }
    info->type = (unsigned char) type;
    info->endpoint = (unsigned char) endpoint;

    /* read buffer */
    info->buffer = calloc (info->buffer_length, 1);
    if (!read_hex (data + offset, info->buffer, info->buffer_length)) {
        DBG ("usbdevfs_reapurb_init_from_text: failed to parse buffer '%s'\n", data + offset);
        free (info->buffer);
        free (info);
        return FALSE;
    };

    node->data = info;
    return TRUE;
}

static void
usbdevfs_reapurb_write (const ioctl_tree* node, FILE* f)
{
    const struct usbdevfs_urb* urb = node->data;
    assert (node->data != NULL);
    fprintf (f, "%u %u %i %u %i %i %i ", (unsigned) urb->type,
             (unsigned) urb->endpoint, urb->status, (unsigned) urb->flags,
             urb->buffer_length, urb->actual_length, urb->error_count);
    write_hex (f, urb->buffer, urb->endpoint & 0x80 ? urb->actual_length : urb->buffer_length);
    /* for debugging test cases with ASCII contents */
    /*fwrite (urb->buffer, urb->endpoint & 0x80 ? urb->actual_length : urb->buffer_length, 1, f); */
} 

static int
usbdevfs_reapurb_equal (const ioctl_tree* n1, const ioctl_tree *n2)
{
    struct usbdevfs_urb* u1 = n1->data;
    struct usbdevfs_urb* u2 = n2->data;
    return u1->type == u2->type && u1->endpoint == u2->endpoint && 
           u1->status == u2->status && u1->flags == u2->flags &&
           u1->buffer_length == u2->buffer_length &&
           u1->actual_length == u2->actual_length &&
           memcmp (u1->buffer, u2->buffer, u1->buffer_length) == 0;
}

static int
usbdevfs_reapurb_execute (const ioctl_tree* node, unsigned long id, void* arg, int *ret)
{
    const struct usbdevfs_urb* n_urb = node->data;
    /* set in SUBMIT, cleared in REAP */
    static const ioctl_tree *submit_node = NULL;
    static struct usbdevfs_urb *submit_urb = NULL;

    if (id == USBDEVFS_SUBMITURB) {
        struct usbdevfs_urb *a_urb = arg;
        assert (submit_node == NULL);

        if (n_urb->type != a_urb->type || n_urb->endpoint != a_urb->endpoint ||
            n_urb->status != a_urb->status || n_urb->flags != a_urb->flags ||
            n_urb->buffer_length != a_urb->buffer_length)
            return 0;

        DBG ("  usbdevfs_reapurb_execute: handling SUBMITURB, metadata match\n");

        /* for an output URB we also require the buffer contents to match; for
         * an input URB it can be uninitialized */
        if ((n_urb->endpoint & 0x80) == 0 && 
            memcmp (n_urb->buffer, a_urb->buffer, n_urb->buffer_length) != 0) {
            DBG ("  usbdevfs_reapurb_execute: handling SUBMITURB, buffer mismatch, rejecting\n");
            return 0;
        }
        DBG ("  usbdevfs_reapurb_execute: handling SUBMITURB, buffer match, remembering\n");

        /* remember the node for the next REAP */
        submit_node = node;
        submit_urb = a_urb;
        *ret = 0;
        return 1;
    }

    if (id == node->type->id) {
        struct usbdevfs_urb* orig_node_urb = submit_node->data;
        assert (submit_node != NULL);

        DBG ("  usbdevfs_reapurb_execute: handling %s\n", node->type->name);

        /* for an input EP we need to copy the buffer data */
        if (n_urb->endpoint & 0x80) {
            submit_urb->actual_length = orig_node_urb->actual_length;
            memcpy (submit_urb->buffer,
                    orig_node_urb->buffer,
                    submit_urb->actual_length);
        }
        *((struct usbdevfs_urb**) arg) = submit_urb;
        submit_urb = NULL;
        submit_node = NULL;
        *ret = 0;
        return 2;
    }

    return 0;
}

static ioctl_tree*
usbdevfs_reapurb_insertion_parent (ioctl_tree* tree,
                                   ioctl_tree* node)
{
    if (((struct usbdevfs_urb*) node->data)->endpoint & 0x80)
    {
        /* input node: child of last REAPURB request */
        ssize_t i;
        ioctl_tree *t;
        for (i = tree->last_added->n - 1; i >= 0; --i) {
            t = ioctl_node_list_get (tree->last_added, i);
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
 * Known ioctls
 *
 ***********************************/

ioctl_type ioctl_db[] = {
    { USBDEVFS_CONNECTINFO, "USBDEVFS_CONNECTINFO",
      usbdevfs_connectinfo_init_from_bin, usbdevfs_connectinfo_init_from_text,
      usbdevfs_connectinfo_write, usbdevfs_connectinfo_equal,
      usbdevfs_connectinfo_execute, usbdevfs_connectinfo_insertion_parent },
    /* we assume that every SUBMITURB is followed by a REAPURB and that
     * ouput EPs don't change the buffer, so we ignore USBDEVFS_SUBMITURB */
    { USBDEVFS_REAPURB, "USBDEVFS_REAPURB",
      usbdevfs_reapurb_init_from_bin, usbdevfs_reapurb_init_from_text,
      usbdevfs_reapurb_write, usbdevfs_reapurb_equal,
      usbdevfs_reapurb_execute, usbdevfs_reapurb_insertion_parent },
    { USBDEVFS_REAPURBNDELAY, "USBDEVFS_REAPURBNDELAY",
      usbdevfs_reapurb_init_from_bin, usbdevfs_reapurb_init_from_text,
      usbdevfs_reapurb_write, usbdevfs_reapurb_equal,
      usbdevfs_reapurb_execute, usbdevfs_reapurb_insertion_parent },
    /* terminator */
    { 0, "", NULL, NULL, NULL, NULL, NULL }
};

const ioctl_type*
ioctl_type_get_by_id (unsigned long id)
{
    ioctl_type* cur;
    for (cur = ioctl_db; cur->name[0] != '\0'; ++cur)
        if (cur->id == id)
            return cur;
    return NULL;
}

const ioctl_type*
ioctl_type_get_by_name (const char *name)
{
    ioctl_type* cur;
    for (cur = ioctl_db; cur->name[0] != '\0'; ++cur)
        if (strcmp (cur->name, name) == 0)
            return cur;
    return NULL;
}
