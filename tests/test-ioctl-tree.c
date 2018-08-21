/*
 * test-ioctl-tree
 *
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

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <linux/usbdevice_fs.h>
#include <linux/input.h>

#include "ioctl_tree.h"

#if !defined(GLIB_VERSION_2_36)
#    include <glib-object.h>
#endif

/* test ioctl data */
const struct usbdevfs_connectinfo ci = { 11, 0 };
const struct usbdevfs_connectinfo ci2 = { 12, 0 };
const struct usbdevfs_urb s_out1 = { 1, 2, 0, 0, "what", 4, 4 };
const struct usbdevfs_urb s_in1a = { 1, 129, 0, 0, "this\0\0\0\0\0\0", 10, 4 };
const struct usbdevfs_urb s_in1b = { 1, 129, 0, 0, "andthat\xFF\xC0\0", 10, 9 };
const struct usbdevfs_urb s_out2 = { 1, 2, 0, 0, "readfile", 8, 8 };
const struct usbdevfs_urb s_in2a = { 1, 129, 0, 0, "file1a\0\0\0\0\0\0\0\0\0", 15, 6 };
const struct usbdevfs_urb s_in2b = { 1, 129, 0, 0, "file1bb\0\0\0\0\0\0\0\0", 15, 7 };
const struct usbdevfs_urb s_in2c = { 1, 129, 0, 0, "file1ccc\0\0\0\0\0\0\0", 15, 8 };
const struct usbdevfs_urb s_in3 = { 1, 129, -5, 0, "file2\0\0\0\0\0\0\0\0\0\0", 15, 5 };

/* REAPURB expects a pointer to a urb struct pointer */
const struct usbdevfs_urb *out1 = &s_out1, *in1a = &s_in1a, *in1b = &s_in1b,
    *out2 = &s_out2, *in2a = &s_in2a, *in2b = &s_in2b, *in2c = &s_in2c, *in3 = &s_in3;

const gchar test_tree_str[] =
#if __BYTE_ORDER == __LITTLE_ENDIAN
    "USBDEVFS_CONNECTINFO 0 0B00000000000000\n"
#else
    "USBDEVFS_CONNECTINFO 0 0000000B00000000\n"
#endif
    "USBDEVFS_REAPURB 0 1 2 0 0 4 4 0 77686174\n"
    " USBDEVFS_REAPURB 0 1 129 0 0 10 4 0 74686973\n"
    "  USBDEVFS_REAPURB 0 1 129 0 0 10 9 0 616E6474686174FFC0\n"
    "USBDEVFS_REAPURB 0 1 2 0 0 8 8 0 7265616466696C65\n"
    " USBDEVFS_REAPURB 0 1 129 0 0 15 6 0 66696C653161\n"
    "  USBDEVFS_REAPURB 0 1 129 0 0 15 7 0 66696C65316262\n"
    "   USBDEVFS_REAPURB 0 1 129 0 0 15 8 0 66696C6531636363\n"
    " USBDEVFS_REAPURB 0 1 129 -5 0 15 5 0 66696C6532\n"
#if __BYTE_ORDER == __LITTLE_ENDIAN
    "USBDEVFS_CONNECTINFO 42 0C00000000000000\n";
#else
    "USBDEVFS_CONNECTINFO 42 0000000C00000000\n";
#endif

static ioctl_tree *
get_test_tree(void)
{
    FILE *f;
    ioctl_tree *tree;

    f = tmpfile();
    g_assert(f != NULL);
    g_assert_cmpint(fwrite(test_tree_str, strlen(test_tree_str), 1, f), ==, 1);
    rewind(f);
    tree = ioctl_tree_read(f);
    fclose(f);
    g_assert(tree != NULL);
    return tree;
}

static void
t_type_get_by(void)
{
    IOCTL_REQUEST_TYPE id;
    const ioctl_type *t;

    g_assert(ioctl_type_get_by_id(-1) == NULL);
    g_assert_cmpstr(ioctl_type_get_by_id(USBDEVFS_CONNECTINFO)->name, ==, "USBDEVFS_CONNECTINFO");
    g_assert_cmpstr(ioctl_type_get_by_id(USBDEVFS_REAPURBNDELAY)->name, ==, "USBDEVFS_REAPURBNDELAY");

    g_assert(ioctl_type_get_by_name("no_such_ioctl", NULL) == NULL);
    g_assert_cmpint(ioctl_type_get_by_name("USBDEVFS_CONNECTINFO", &id)->id, ==, USBDEVFS_CONNECTINFO);
    g_assert_cmpuint(id, ==, USBDEVFS_CONNECTINFO);
    g_assert_cmpuint(ioctl_type_get_by_name("USBDEVFS_REAPURBNDELAY", NULL)->id, ==, USBDEVFS_REAPURBNDELAY);

    t = ioctl_type_get_by_id(USBDEVFS_CONNECTINFO);
    g_assert(t != NULL);
    g_assert_cmpuint(t->id, ==, USBDEVFS_CONNECTINFO);
    g_assert_cmpstr(t->name, ==, "USBDEVFS_CONNECTINFO");

    t = ioctl_type_get_by_id(EVIOCGABS(0));
    g_assert(t != NULL);
    /* have to cast here, as with musl EVIO* have the wrong type "unsigned long" */
    g_assert_cmpuint(t->id, ==, (IOCTL_REQUEST_TYPE) EVIOCGABS(0));
    g_assert_cmpstr(t->name, ==, "EVIOCGABS");

    t = ioctl_type_get_by_id(EVIOCGABS(ABS_X));
    g_assert(t != NULL);
    g_assert_cmpuint(t->id, ==, (IOCTL_REQUEST_TYPE) EVIOCGABS(0));
    g_assert_cmpstr(t->name, ==, "EVIOCGABS");
    g_assert(ioctl_type_get_by_id(EVIOCGABS(ABS_WHEEL)) == t);
    g_assert(ioctl_type_get_by_id(EVIOCGABS(ABS_MAX)) == t);

    g_assert(ioctl_type_get_by_name("EVIOCGABS", &id) == t);
    g_assert(ioctl_type_get_by_name("EVIOCGABS(0)", &id) == t);
    g_assert_cmpuint(id, ==, (IOCTL_REQUEST_TYPE) EVIOCGABS(ABS_X));
    g_assert(ioctl_type_get_by_name("EVIOCGABS(8)", &id) == t);
    g_assert_cmpuint(id, ==, (IOCTL_REQUEST_TYPE) EVIOCGABS(ABS_WHEEL));

    t = ioctl_type_get_by_id(EVIOCGBIT(EV_SYN, 10));
    g_assert(t != NULL);
    g_assert_cmpuint(t->id, ==, (IOCTL_REQUEST_TYPE) EVIOCGBIT(EV_SYN, 32));
    g_assert_cmpstr(t->name, ==, "EVIOCGBIT");
    g_assert(ioctl_type_get_by_id(EVIOCGBIT(EV_KEY, 20)) == t);
    g_assert(ioctl_type_get_by_id(EVIOCGBIT(EV_PWR, 1000)) == t);
}

#define assert_node(n,p,c,nx) \
    g_assert (n->parent == p);      \
    g_assert (n->child == c);        \
    g_assert (n->next == nx);

static void
t_create_from_bin(void)
{
    ioctl_tree *tree = NULL;
    ioctl_tree *n_ci, *n_ci2, *n_out1, *n_in1a, *n_in1b, *n_out2, *n_in2a, *n_in2b, *n_in2c, *n_out2_2, *n_in3;

    /* inputs should always follow (child) their outputs; outputs should be
     * top level nodes, but they should stay in order of addition; different
     * possibile inputs for an output chould be represented as alternative
     * children:
     *
     * (R) ---------------------
     *  | \      \              \
     *  v  v      v              v
     * CI OUT1   OUT2 -----     CI2
     *     |      |        \
     *     v      v         v
     *    IN1a   IN2a      IN3
     *     |      |
     *     v      v
     *    IN1b   IN2b
     *            |
     *            v
     *           IN2c
     */

    /* add ci */
    tree = n_ci = ioctl_tree_new_from_bin(USBDEVFS_CONNECTINFO, &ci, 0);
    g_assert(ioctl_tree_insert(NULL, n_ci) == NULL);
    g_assert(n_ci->data != &ci);	/* data should get copied */
    assert_node(n_ci, NULL, NULL, NULL);

    /* add out1, in1a, in1b */
    n_out1 = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &out1, 0);
    g_assert(ioctl_tree_insert(tree, n_out1) == NULL);
    g_assert(n_out1->data != &out1);	/* data should get copied */
    assert_node(n_out1, tree, NULL, NULL);
    g_assert(n_ci->next == n_out1);

    n_in1a = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in1a, 0);
    g_assert(ioctl_tree_insert(tree, n_in1a) == NULL);
    assert_node(n_in1a, n_out1, NULL, NULL);
    assert_node(n_out1, tree, n_in1a, NULL);

    n_in1b = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in1b, 0);
    g_assert(ioctl_tree_insert(tree, n_in1b) == NULL);

    /* add CI again, should not change anything */
    n_ci2 = ioctl_tree_new_from_bin(USBDEVFS_CONNECTINFO, &ci, 0);
    g_assert(ioctl_tree_insert(tree, n_ci2) == n_ci);
    g_assert(n_ci2->parent == NULL);
    g_assert(tree == n_ci);
    ioctl_tree_free(n_ci2);

    /* add out2, should become a new top-level node */
    n_out2 = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &out2, 0);
    g_assert(ioctl_tree_insert(tree, n_out2) == NULL);
    assert_node(n_out2, tree, NULL, NULL);

    /* add in2a and in2b */
    n_in2a = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in2a, 0);
    g_assert(ioctl_tree_insert(tree, n_in2a) == NULL);
    n_in2b = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in2b, 0);
    g_assert(ioctl_tree_insert(tree, n_in2b) == NULL);

    /* and append in2c, with an interjected ci */
    n_ci2 = ioctl_tree_new_from_bin(USBDEVFS_CONNECTINFO, &ci2, 42);
    g_assert(ioctl_tree_insert(tree, n_ci2) == NULL);
    n_in2c = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in2c, 0);
    g_assert(ioctl_tree_insert(tree, n_in2c) == NULL);

    /* add out2 again, should not get added but should become "last added" */
    n_out2_2 = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &out2, 0);
    g_assert(ioctl_tree_insert(tree, n_out2_2) == n_out2);
    ioctl_tree_free(n_out2_2);

    /* add in3, should become alternative of out2 */
    n_in3 = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in3, 0);
    g_assert(ioctl_tree_insert(tree, n_in3) == NULL);

    /* check tree structure */
    g_assert(tree == n_ci);
    assert_node(n_ci, NULL, NULL, n_out1);
    assert_node(n_out1, tree, n_in1a, n_out2);
    assert_node(n_in1a, n_out1, n_in1b, NULL);
    assert_node(n_in1b, n_in1a, NULL, NULL);
    assert_node(n_out2, tree, n_in2a, n_ci2);
    assert_node(n_in2a, n_out2, n_in2b, n_in3);
    assert_node(n_in2b, n_in2a, n_in2c, NULL);
    assert_node(n_in2c, n_in2b, NULL, NULL);
    assert_node(n_in3, n_out2, NULL, NULL);
    assert_node(n_ci2, tree, NULL, NULL);

    ioctl_tree_free(tree);
}

static void
t_write(void)
{
    ioctl_tree *tree = NULL, *t;
    FILE *f;
    char contents[1000];

    /* same tree as in t_create_from_bin() */
    tree = ioctl_tree_new_from_bin(USBDEVFS_CONNECTINFO, &ci, 0);
    ioctl_tree_insert(NULL, tree);
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &out1, 0));
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in1a, 0));
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in1b, 0));
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &out2, 0));
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in2a, 0));
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in2b, 0));
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_CONNECTINFO, &ci2, 42));
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in2c, 0));
    /* duplicate */
    t = ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &out2, 0);
    g_assert(ioctl_tree_insert(tree, t) != NULL);
    ioctl_tree_free(t);
    ioctl_tree_insert(tree, ioctl_tree_new_from_bin(USBDEVFS_REAPURB, &in3, 0));

    f = tmpfile();
    g_assert(f != NULL);
    ioctl_tree_write(f, tree);
    rewind(f);
    memset(contents, 0, sizeof(contents));
    g_assert_cmpint(fread(contents, 1, sizeof(contents), f), >, 10);
    g_assert_cmpstr(contents, ==, test_tree_str);

    fclose(f);
    ioctl_tree_free(tree);
}

static void
t_read(void)
{
    FILE *f;
    char contents[1000];
    /* this tests the actual ioctl_tree_read() */
    ioctl_tree *tree = get_test_tree();

    /* write test tree into the tempfile and read it back to compare with original
     * (easier than comparing nodes) */
    f = tmpfile();
    ioctl_tree_write(f, tree);
    ioctl_tree_free(tree);
    rewind(f);
    memset(contents, 0, sizeof(contents));
    g_assert_cmpint(fread(contents, 1, sizeof(contents), f), >, 10);
    g_assert_cmpstr(contents, ==, test_tree_str);

    fclose(f);
}

#define assert_ci(n,d) { \
    struct usbdevfs_connectinfo *nd = n->data;      \
    g_assert (n->type->id == USBDEVFS_CONNECTINFO); \
    g_assert_cmpint (nd->devnum, ==, (d)->devnum);    \
    g_assert_cmpint (nd->slow, ==, (d)->slow);        \
}

#define assert_urb(n,urb) { \
    g_assert (n != NULL);                       \
    struct usbdevfs_urb *nd = n->data;          \
    g_assert (n->type->id == USBDEVFS_REAPURB); \
    g_assert (nd->endpoint == (urb)->endpoint);   \
    g_assert (nd->status == (urb)->status);       \
    g_assert (nd->flags == (urb)->flags);         \
    g_assert (nd->buffer_length == (urb)->buffer_length);   \
    g_assert (nd->actual_length == (urb)->actual_length);   \
    g_assert (memcmp (nd->buffer, (urb)->buffer, nd->buffer_length) == 0);   \
}

static void
t_iteration(void)
{
    ioctl_tree *tree = get_test_tree();
    ioctl_tree *i;

    i = tree;
    assert_ci(i, &ci);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_out1);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_in1a);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_in1b);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_out2);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_in2a);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_in2b);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_in2c);
    i = ioctl_tree_next(i);
    assert_urb(i, &s_in3);
    i = ioctl_tree_next(i);
    assert_ci(i, &ci2);

    g_assert(ioctl_tree_next(i) == NULL);
    g_assert(ioctl_tree_next_wrap(tree, NULL) == tree);
    g_assert(ioctl_tree_next_wrap(tree, i) == tree);
    ioctl_tree_free(tree);
}

static void
init_urb(struct usbdevfs_urb *urb, const struct usbdevfs_urb *orig)
{
    urb->type = orig->type;
    urb->endpoint = orig->endpoint;
    urb->status = orig->status;
    urb->flags = orig->flags;
    urb->buffer_length = orig->buffer_length;

    if (urb->endpoint & 0x80) {
	/* clear buffer for input EPs */
	urb->actual_length = 0;
	memset(urb->buffer, 0, urb->buffer_length);
    } else {
	/* copy for output */
	urb->actual_length = orig->buffer_length;
	memcpy(urb->buffer, orig->buffer, orig->buffer_length);
    }
}

static void
t_execute_check_outurb(const struct usbdevfs_urb *orig, ioctl_tree * tree, ioctl_tree ** last)
{
    char urbbuf[15];
    struct usbdevfs_urb urb;
    struct usbdevfs_urb *urb_ret;
    int ret;

    urb.buffer = urbbuf;
    init_urb(&urb, orig);
    *last = ioctl_tree_execute(tree, *last, USBDEVFS_SUBMITURB, &urb, &ret);
    g_assert(*last != NULL);
    g_assert(ret == 0);
    g_assert(urb.buffer == urbbuf);

    /* now reap it */
    *last = ioctl_tree_execute(tree, *last, USBDEVFS_REAPURB, &urb_ret, &ret);
    g_assert(ret == 0);
    g_assert(urb_ret == &urb);
    g_assert_cmpint(urb.actual_length, ==, orig->actual_length);
    g_assert(memcmp(urbbuf, orig->buffer, urb.actual_length) == 0);
}

static void
t_execute_check_inurb(const struct usbdevfs_urb *orig, ioctl_tree * tree, ioctl_tree ** last)
{
    char urbbuf[15];
    struct usbdevfs_urb urb;
    struct usbdevfs_urb *urb_ret;
    int ret;

    urb.buffer = urbbuf;
    init_urb(&urb, orig);

    *last = ioctl_tree_execute(tree, *last, USBDEVFS_SUBMITURB, &urb, &ret);
    g_assert(*last != NULL);
    g_assert(ret == 0);
    g_assert(urb.buffer == urbbuf);
    /* not reaped yet */
    g_assert_cmpint(urbbuf[0], ==, 0);
    g_assert_cmpint(urb.actual_length, ==, 0);

    /* now reap */
    *last = ioctl_tree_execute(tree, *last, USBDEVFS_REAPURB, &urb_ret, &ret);
    g_assert(*last != NULL);
    g_assert(ret == 0);
    g_assert(urb_ret == &urb);
    g_assert_cmpint(urb.actual_length, ==, orig->actual_length);
    g_assert(memcmp(urbbuf, orig->buffer, urb.actual_length) == 0);
}

static void
t_execute(void)
{
    ioctl_tree *tree = get_test_tree();
    ioctl_tree *last = NULL;
    int ret;
    struct usbdevfs_connectinfo ci;

    /* should first get CI, then CI2 */
    last = ioctl_tree_execute(tree, last, USBDEVFS_CONNECTINFO, &ci, &ret);
    g_assert(last == tree);
    g_assert(ret == 0);
    g_assert_cmpint(ci.devnum, ==, 11);
    g_assert_cmpint(ci.slow, ==, 0);

    last = ioctl_tree_execute(tree, last, USBDEVFS_CONNECTINFO, &ci, &ret);
    g_assert(last != NULL);
    g_assert(ret == 42);
    g_assert_cmpint(ci.devnum, ==, 12);
    g_assert_cmpint(ci.slow, ==, 0);

    /* output URB, should leave buffer untouched */
    t_execute_check_outurb(&s_out1, tree, &last);
    g_assert(last == tree->next);
    /* now get the input */
    t_execute_check_inurb(&s_in1a, tree, &last);
    g_assert(last == tree->next->child);

    /* jump to out2 */
    t_execute_check_outurb(&s_out2, tree, &last);
    /* get in2[abc], but interject unknown ioctl */
    t_execute_check_inurb(&s_in2a, tree, &last);
    g_assert(ioctl_tree_execute(tree, last, TCGETS, NULL, &ret) == NULL);
    t_execute_check_inurb(&s_in2b, tree, &last);
    t_execute_check_inurb(&s_in2c, tree, &last);

    /* URB with last == NULL */
    last = NULL;
    t_execute_check_outurb(&s_out1, tree, &last);
    g_assert(last == tree->next);

    ioctl_tree_free(tree);
}

static void
t_execute_unknown(void)
{
    ioctl_tree *tree = get_test_tree();
    struct usbdevfs_urb unknown_urb = { 1, 9, 0, 0, "yo!", 3, 3 };
    int ret;

    /* not found with last != NULL */
    g_assert(ioctl_tree_execute(tree, tree->next, USBDEVFS_SUBMITURB, &unknown_urb, &ret) == NULL);
    /* not found with last == NULL */
    g_assert(ioctl_tree_execute(tree, NULL, USBDEVFS_SUBMITURB, &unknown_urb, &ret) == NULL);

    ioctl_tree_free(tree);
}

static void
t_evdev(void)
{
    ioctl_tree *tree = NULL, *t;
    FILE *f;
    char contents[1000];
    struct input_absinfo absinfo_x = { 100, 50, 150, 2, 5, 1 };
    struct input_absinfo absinfo_volume = { 30, 0, 100, 0, 9, 10 };
    struct input_absinfo abs_query;
    char synbits[4] = "\x01\x02\x03\x04";
    char keybits[48] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    char pwrbits[4] = "\x00\x00\x00\x00";
    char bits_query[48];
    int ret;

    /* create a tree from ioctls */
    tree = ioctl_tree_new_from_bin(EVIOCGABS(ABS_X), &absinfo_x, 0);
    g_assert(tree != NULL);
    ioctl_tree_insert(NULL, tree);
    g_assert(ioctl_tree_insert(tree, ioctl_tree_new_from_bin(EVIOCGABS(ABS_VOLUME), &absinfo_volume, 8)) == NULL);
    /* duplicate */
    t = ioctl_tree_new_from_bin(EVIOCGABS(ABS_X), &absinfo_x, 0);
    g_assert(ioctl_tree_insert(tree, t) != NULL);
    ioctl_tree_free(t);

    g_assert(ioctl_tree_insert(tree, ioctl_tree_new_from_bin(EVIOCGBIT(EV_SYN, sizeof(synbits)), synbits, 0x81)) ==
	     NULL);
    g_assert(ioctl_tree_insert(tree, ioctl_tree_new_from_bin(EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits, 0x82)) ==
	     NULL);
    g_assert(ioctl_tree_insert(tree, ioctl_tree_new_from_bin(EVIOCGBIT(EV_PWR, sizeof(pwrbits)), pwrbits, 0x83)) ==
	     NULL);

    /* write it into file */
    f = tmpfile();
    ioctl_tree_write(f, tree);
    rewind(f);
    ioctl_tree_free(tree);

    /* check text representation */
    memset(contents, 0, sizeof(contents));
    g_assert_cmpint(fread(contents, 1, sizeof(contents), f), >, 10);
    g_assert_cmpstr(contents, ==,
#if __BYTE_ORDER == __LITTLE_ENDIAN
		    "EVIOCGABS 0 640000003200000096000000020000000500000001000000\n"
		    "EVIOCGABS(32) 8 1E000000000000006400000000000000090000000A000000\n"
#else
		    "EVIOCGABS 0 000000640000003200000096000000020000000500000001\n"
		    "EVIOCGABS(32) 8 0000001E000000000000006400000000000000090000000A\n"
#endif
		    "EVIOCGBIT(0) 129 01020304\n"
		    "EVIOCGBIT(1) 130 616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161\n"
		    "EVIOCGBIT(22) 131 00000000\n");
    rewind(f);

    /* read it back */
    tree = ioctl_tree_read(f);
    fclose(f);

    /* execute EVIOCGABS ioctls */
    g_assert(ioctl_tree_execute(tree, NULL, EVIOCGABS(ABS_X), &abs_query, &ret));
    g_assert(ret == 0);
    g_assert(memcmp(&abs_query, &absinfo_x, sizeof(abs_query)) == 0);

    g_assert(ioctl_tree_execute(tree, NULL, EVIOCGABS(ABS_VOLUME), &abs_query, &ret));
    g_assert(ret == 8);
    g_assert(memcmp(&abs_query, &absinfo_volume, sizeof(abs_query)) == 0);

    g_assert(!ioctl_tree_execute(tree, NULL, EVIOCGABS(ABS_Y), &abs_query, &ret));

    /* execute EVIOCGBIT ioctls */
    /* ensure that it doesn't write beyond specified length */
    memset(bits_query, 0xAA, sizeof(bits_query));
    g_assert(ioctl_tree_execute(tree, NULL, EVIOCGBIT(EV_SYN, sizeof(synbits)), &bits_query, &ret));
    g_assert(ret == 0x81);
    g_assert(memcmp(&bits_query, "\x01\x02\x03\x04\xAA\xAA\xAA\xAA", 8) == 0);

    memset(bits_query, 0xAA, sizeof(bits_query));
    g_assert(ioctl_tree_execute(tree, NULL, EVIOCGBIT(EV_KEY, sizeof(keybits)), &bits_query, &ret));
    g_assert(ret == 0x82);
    g_assert(memcmp(&bits_query, keybits, sizeof(bits_query)) == 0);

    memset(bits_query, 0xAA, sizeof(bits_query));
    g_assert(ioctl_tree_execute(tree, NULL, EVIOCGBIT(EV_PWR, sizeof(pwrbits)), &bits_query, &ret));
    g_assert(ret == 0x83);
    g_assert(memcmp(&bits_query, "\0\0\0\0\xAA\xAA\xAA\xAA", 8) == 0);

    /* undefined for other ev type */
    g_assert(!ioctl_tree_execute(tree, NULL, EVIOCGBIT(EV_REL, sizeof(synbits)), &bits_query, NULL));
    /* undefined for other length */
    g_assert(!ioctl_tree_execute(tree, NULL, EVIOCGBIT(EV_KEY, 4), &bits_query, NULL));

    ioctl_tree_free(tree);
}

int
main(int argc, char **argv)
{
#if !defined(GLIB_VERSION_2_36)
    g_type_init();
#endif
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/umockdev-ioctl-tree/type_get_by", t_type_get_by);
    g_test_add_func("/umockdev-ioctl-tree/create_from_bin", t_create_from_bin);
    g_test_add_func("/umockdev-ioctl-tree/write", t_write);
    g_test_add_func("/umockdev-ioctl-tree/read", t_read);
    g_test_add_func("/umockdev-ioctl-tree/iteration", t_iteration);
    g_test_add_func("/umockdev-ioctl-tree/execute", t_execute);
    g_test_add_func("/umockdev-ioctl-tree/execute_unknown", t_execute_unknown);

    g_test_add_func("/umockdev-ioctl-tree/evdev", t_evdev);

    return g_test_run();
}
