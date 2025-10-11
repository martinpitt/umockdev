/*
 * test-umockdev
 *
 * Copyright (C) 2012 Canonical Ltd.
 * Copyright (C) 2017, 2018 Martin Pitt
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

/* for O_TMPFILE */
#define _GNU_SOURCE

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <linux/usbdevice_fs.h>
#include <linux/input.h>
#include <linux/magic.h>

#include <libudev.h>
#include <gudev/gudev.h>

#include "umockdev.h"

/* fix missing O_TMPFILE on some systems */
#ifndef O_TMPFILE
#define O_TMPFILE (__O_TMPFILE | O_DIRECTORY)
#endif

#define UNUSED __attribute__ ((unused))
#define UNUSED_DATA UNUSED gconstpointer data

static gboolean has_real_sysfs;

static int slow_testbed_factor = 1;

typedef struct {
    UMockdevTestbed *testbed;
    gchar *root_dir;
    gchar *sys_dir;
} UMockdevTestbedFixture;

static void
t_testbed_fixture_setup(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    fixture->testbed = umockdev_testbed_new();
    g_assert(fixture->testbed != NULL);
    fixture->root_dir = umockdev_testbed_get_root_dir(fixture->testbed);
    fixture->sys_dir = umockdev_testbed_get_sys_dir(fixture->testbed);
}

static void
t_testbed_fixture_teardown(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    g_object_unref(fixture->testbed);

    /* verify that temp dir gets cleaned up properly */
    g_assert(!g_file_test(fixture->root_dir, G_FILE_TEST_EXISTS));
    g_free(fixture->root_dir);
    g_free(fixture->sys_dir);
}

static void
t_testbed_read_buf_delay(ulong delay, int fd, char* buf, ssize_t length)
{
    ssize_t len = 0;
    gint64 start;

    usleep (delay);
    start = g_get_monotonic_time();

    while (len < length && 5000000 > g_get_monotonic_time() - start) {
        ssize_t ret;

        errno = 0;
        ret = read (fd, &buf[len], length - len);
        if (ret > 0)
            len += ret;
        else if (ret == 0)
            break;
        else
            g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK);
    }

    g_assert_cmpint(len, ==, length);
}

/* Return number of devices that libudev can see */
static guint
num_udev_devices(void)
{
    struct udev *udev;
    struct udev_enumerate *enumerate;
    struct udev_list_entry *entry;
    int err;
    guint num = 0;

    udev = udev_new();
    g_assert(udev);

    enumerate = udev_enumerate_new(udev);
    g_assert(enumerate);

    // NB: using libudev here instead of GUdev so that we can check the return
    // value of udev_enumerate_scan_devices().
    // https://github.com/martinpitt/umockdev/issues/144
    err = udev_enumerate_scan_devices(enumerate);
    g_assert_cmpint(err, >=, 0);

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
        num++;
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);

    return num;
}

/* Empty UMockdevTestbed without any devices */
static void
t_testbed_empty(UNUSED UMockdevTestbedFixture *fixture, UNUSED_DATA)
{
    g_assert_cmpuint(num_udev_devices(), ==, 0);
}

/* common checks for umockdev_testbed_add_device{,v}() */
static void
_t_testbed_check_extkeyboard1(const gchar * syspath)
{
    GUdevEnumerator *enumerator;
    GList *result;
    GUdevDevice *device;
    g_autoptr (GUdevClient) client = g_udev_client_new(NULL);
    g_assert(client);

    enumerator = g_udev_enumerator_new(client);
    g_assert(enumerator);
    result = g_udev_enumerator_execute(enumerator);
    g_assert_cmpuint(g_list_length(result), ==, 1);

    /* check that the entry matches what we put into our test bed */
    device = G_UDEV_DEVICE(result->data);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_name(device), ==, "extkeyboard1");
    g_assert_cmpstr(g_udev_device_get_sysfs_path(device), ==, syspath);

    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idProduct"), ==, "AFFE");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "noSuchAttr"), ==, NULL);

    g_assert_cmpstr(g_udev_device_get_property(device, "DEVPATH"), ==, "/devices/extkeyboard1");
    g_assert_cmpstr(g_udev_device_get_property(device, "SUBSYSTEM"), ==, "usb");
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT"), ==, "1");
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT_KEYBOARD"), ==, "1");
    g_assert_cmpstr(g_udev_device_get_property(device, "NO_SUCH_PROP"), ==, NULL);

    g_list_free_full(result, g_object_unref);
    g_object_unref(enumerator);
}

/* UMockdevTestbed add_devicev() with adding one device */
static void
t_testbed_add_devicev(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    gchar *attributes[] = { "idVendor", "0815", "idProduct", "AFFE", NULL };
    gchar *properties[] = { "ID_INPUT", "1", "ID_INPUT_KEYBOARD", "1", NULL };
    struct udev *udev;
    struct udev_monitor *udev_mon;
    struct udev_device *device;

    /* Set up Udev monitor to check for added event */
    udev = udev_new();
    g_assert(udev != NULL);
    udev_mon = udev_monitor_new_from_netlink(udev, "udev");
    g_assert(udev_mon != NULL);
    g_assert_cmpint(udev_monitor_get_fd(udev_mon), >, 0);
    g_assert_cmpint(udev_monitor_enable_receiving(udev_mon), ==, 0);

    g_autofree gchar *syspath = umockdev_testbed_add_devicev(fixture->testbed, "usb", "extkeyboard1", NULL, attributes, properties);
    g_assert_cmpstr(syspath, ==, "/sys/devices/extkeyboard1");

    _t_testbed_check_extkeyboard1(syspath);

    device = udev_monitor_receive_device(udev_mon);
    g_assert(device != NULL);
    g_assert_cmpstr(udev_device_get_syspath(device), ==, syspath);
    g_assert_cmpstr(udev_device_get_action(device), ==, "add");
    udev_device_unref(device);
    udev_monitor_unref(udev_mon);
    udev_unref(udev);
}

/* UMockdevTestbed add_device() with adding one device */
static void
t_testbed_add_device(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    struct udev *udev;
    struct udev_monitor *udev_mon;
    struct udev_device *device;

    /* Set up Udev monitor to check for added event */
    udev = udev_new();
    g_assert(udev != NULL);
    udev_mon = udev_monitor_new_from_netlink(udev, "udev");
    g_assert(udev_mon != NULL);
    g_assert_cmpint(udev_monitor_get_fd(udev_mon), >, 0);
    g_assert_cmpint(udev_monitor_enable_receiving(udev_mon), ==, 0);

    g_autofree gchar *syspath = umockdev_testbed_add_device(
            fixture->testbed, "usb", "extkeyboard1", NULL,
            /* attributes */
            "idVendor", "0815", "idProduct", "AFFE", NULL,
            /* properties */
            "DEVTYPE", "usb_device", "DEVNAME", "/dev/mydev",
            "ID_INPUT", "1", "ID_INPUT_KEYBOARD", "1", NULL);
    g_assert(syspath);
    g_assert(g_str_has_suffix(syspath, "/sys/devices/extkeyboard1"));

    _t_testbed_check_extkeyboard1(syspath);

    device = udev_monitor_receive_device(udev_mon);
    g_assert(device != NULL);
    g_assert_cmpstr(udev_device_get_syspath(device), ==, syspath);
    g_assert_cmpstr(udev_device_get_action(device), ==, "add");
    g_assert_cmpstr(udev_device_get_devnode(device), ==, "/dev/mydev");
    g_assert_cmpstr(udev_device_get_devtype(device), ==, "usb_device");
    udev_device_unref(device);
    udev_monitor_unref(udev_mon);
    udev_unref(udev);
}

/* UMockdevTestbed add_device() with adding a child device */
static void
t_testbed_child_device(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    gchar *dev, *iface, *input;
    GUdevDevice *device, *device2;
    gchar *path;

    dev = umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", NULL,
				      /* attributes */
				      "idVendor", "0815", "idProduct", "AFFE", NULL,
				      /* properties */
				      "INTERFACES", ":3/1/1:", NULL);
    g_assert(dev);
    g_assert_cmpstr(dev, ==, "/sys/devices/usb1");

    iface = umockdev_testbed_add_device(fixture->testbed, "usb", "1-1", dev,
					/* attributes */
					"iClass", "2", NULL,
					/* properties */
					"INTERFACE", "3/1/1", NULL);
    g_assert(iface);
    g_assert_cmpstr(iface, ==, "/sys/devices/usb1/1-1");

    input = umockdev_testbed_add_device(fixture->testbed, "input", "kb1", iface,
					/* attributes */
					"name", "HID 123", NULL,
					/* properties */
					"ID_INPUT", "1", NULL);
    g_assert(input);
    g_assert_cmpstr(input, ==, "/sys/devices/usb1/1-1/kb1");

    g_autoptr (GUdevClient) client = g_udev_client_new(NULL);

    /* check dev device */
    device = g_udev_client_query_by_sysfs_path(client, dev);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "iClass"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "name"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_property(device, "INTERFACES"), ==, ":3/1/1:");
    g_assert_cmpstr(g_udev_device_get_property(device, "INTERFACE"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT"), ==, NULL);
    g_assert(g_udev_device_get_parent(device) == NULL);
    g_assert_cmpstr(g_udev_device_get_subsystem(device), ==, "usb");
    g_assert_cmpstr(g_udev_device_get_name(device), ==, "usb1");
    g_object_unref(device);

    /* dev's class symlinks */
    path = g_build_filename(dev, "subsystem", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(dev, "subsystem", "devices", "usb1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(dev, "subsystem", "devices", "usb1", "idVendor", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);

    /* dev's bus symlinks */
    path = g_build_filename(fixture->sys_dir, "bus", "usb", "devices", "usb1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(fixture->sys_dir,
			    "bus", "usb", "devices", "usb1", "idVendor", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);

    /* check iface device */
    device = g_udev_client_query_by_sysfs_path(client, iface);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "name"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "iClass"), ==, "2");
    g_assert_cmpstr(g_udev_device_get_property(device, "INTERFACE"), ==, "3/1/1");
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_subsystem(device), ==, "usb");
    g_assert_cmpstr(g_udev_device_get_name(device), ==, "1-1");
    device2 = g_udev_device_get_parent(device);
    g_assert(device2 != NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_path(device2), ==, dev);
    g_object_unref(device);
    g_object_unref(device2);

    /* iface's class symlinks */
    path = g_build_filename(iface, "subsystem", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(iface, "subsystem", "devices", "1-1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(iface, "subsystem", "devices", "1-1", "iClass", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);

    /* iface's bus symlinks */
    path = g_build_filename(fixture->sys_dir, "bus", "usb", "devices", "1-1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(fixture->sys_dir,
			    "bus", "usb", "devices", "1-1", "iClass", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);

    /* check input's device */
    device = g_udev_client_query_by_sysfs_path(client, input);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "name"), ==, "HID 123");
    g_assert_cmpstr(g_udev_device_get_property(device, "INTERFACE"), ==, NULL);
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT"), ==, "1");
    g_assert_cmpstr(g_udev_device_get_subsystem(device), ==, "input");
    g_assert_cmpstr(g_udev_device_get_name(device), ==, "kb1");
    device2 = g_udev_device_get_parent(device);
    g_assert(device2 != NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_path(device2), ==, iface);
    g_object_unref(device);
    g_object_unref(device2);

    /* inputs's class symlinks */
    path = g_build_filename(input, "subsystem", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(input, "subsystem", "kb1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(input, "subsystem", "kb1", "name", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);

    g_free(dev);
    g_free(iface);
    g_free(input);
}

/* UMockdevTestbed add_device() with adding a block device */
static void
t_testbed_add_block_device(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    g_autofree gchar *syspath = umockdev_testbed_add_device(
            fixture->testbed, "block", "scribble", NULL,
            /* attributes */
            "size", "10000", NULL,
            /* properties */
            "ID_TYPE", "disk", NULL);
    g_assert(syspath);
    g_assert_cmpstr(syspath, ==, "/sys/devices/scribble");

    /* check attributes */
    g_assert(g_file_test("/sys/devices/scribble/size", G_FILE_TEST_IS_REGULAR));
    g_assert(g_file_test("/sys/devices/scribble/uevent", G_FILE_TEST_IS_REGULAR));
    g_assert(g_file_test("/sys/devices/scribble/subsystem", G_FILE_TEST_IS_SYMLINK));

    /* check class symlinks */
    g_assert(g_file_test("/sys/class/block/scribble", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/class/block/scribble/size", G_FILE_TEST_IS_REGULAR));

    g_assert(g_file_test("/sys/block/scribble", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/block/scribble/size", G_FILE_TEST_IS_REGULAR));
}

struct TestbedErrorCatcherData {
    unsigned counter;
    GLogLevelFlags last_level;
    gchar *last_message;
};

static gboolean
t_testbed_error_catcher(UNUSED const gchar *log_domain, GLogLevelFlags log_level, const gchar * message, gpointer user_data)
{
    struct TestbedErrorCatcherData *data = (struct TestbedErrorCatcherData *)user_data;

    data->counter++;
    data->last_level = log_level;
    g_free(data->last_message);
    data->last_message = g_strdup(message);
    return FALSE;
}

static void
ignore_log_handler (UNUSED const gchar *log_domain, UNUSED GLogLevelFlags log_level,
                    UNUSED const gchar *message, UNUSED gpointer user_data)
{
}

/* UMockdevTestbed add_device() error conditions */
static void
t_testbed_add_device_errors(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    gchar *syspath;
    struct TestbedErrorCatcherData errors = { 0, 0, NULL };
    guint log_handler;

    g_test_log_set_fatal_handler(t_testbed_error_catcher, &errors);

    /* invalid parent */
    log_handler = g_log_set_handler(NULL, G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_FATAL, ignore_log_handler, NULL);
    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", "/sys/nosuchdevice", NULL, NULL);
    g_log_remove_handler(NULL, log_handler);
    g_assert(syspath == NULL);
    g_assert_cmpint(errors.counter, ==, 1);
    g_assert_cmpint(errors.last_level, ==, G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_FATAL);
    g_assert(strstr(errors.last_message, "/sys/nosuchdevice") != NULL);
    g_free(syspath);

    /* key/values do not pair up */
    log_handler = g_log_set_handler(NULL, G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL, ignore_log_handler, NULL);
    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", NULL,
					  /* attributes */
					  "idVendor", "0815", "idProduct", NULL, NULL);
    g_assert(syspath);
    g_free(syspath);
    g_assert_cmpint(errors.counter, ==, 2);
    g_assert_cmpint(errors.last_level & G_LOG_LEVEL_WARNING, !=, 0);
    g_assert(strstr(errors.last_message, "idProduct") != NULL);

    /* duplicate symlinks are a bug in the device description, but should not crash */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/target\nN: target\nS: link\n"
					      "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/target\n"
					      "A: dev=1:4\n", NULL));

    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/other\nN: other\nS: link\n"
					      "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/other\n"
					      "A: dev=1:5\n", NULL));
    g_assert_cmpint(errors.counter, ==, 3);
    g_assert_cmpint(errors.last_level & G_LOG_LEVEL_WARNING, !=, 0);
    g_assert(strstr(errors.last_message, "File exists") != NULL);

    g_log_remove_handler(NULL, log_handler);
    g_free (errors.last_message);
}

static void
t_testbed_set_attribute(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GUdevDevice *device;
    g_autofree gchar *contents = NULL;
    gsize length;
    g_autoptr (GUdevClient) client = g_udev_client_new(NULL);

    g_autofree gchar *syspath = umockdev_testbed_add_device(
            fixture->testbed, "usb", "extkeyboard1", NULL,
            /* attributes */
            "idVendor", "0815", "idProduct", "AFFE", NULL,
            /* properties */
            NULL);

    /* change an existing attribute */
    umockdev_testbed_set_attribute(fixture->testbed, syspath, "idProduct", "BEEF");
    /* add a new one */
    umockdev_testbed_set_attribute(fixture->testbed, syspath, "color", "yellow");
    /* add a binary attribute */
    umockdev_testbed_set_attribute_binary(fixture->testbed, syspath, "descriptor",
					  (guint8 *) "\x01\x00\xFF\x00\x05\x40\xA0", 7);
    /* add a link attribute */
    umockdev_testbed_set_attribute_link(fixture->testbed, syspath, "driver", "../drivers/hub");
    /* int attributes */
    umockdev_testbed_set_attribute_int(fixture->testbed, syspath, "count", 1000);
    umockdev_testbed_set_attribute_hex(fixture->testbed, syspath, "addr", 0x1a01);
    /* subdir attribute */
    umockdev_testbed_set_attribute(fixture->testbed, syspath, "knobs/red", "off");

    device = g_udev_client_query_by_sysfs_path(client, syspath);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idProduct"), ==, "BEEF");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "color"), ==, "yellow");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "count"), ==, "1000");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "addr"), ==, "1a01");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "knobs/red"), ==, "off");
    g_assert_cmpstr(g_udev_device_get_driver(device), ==, "hub");
    g_object_unref(device);

    /* validate binary attribute */
    g_autofree gchar *attrpath = g_build_filename(syspath, "descriptor", NULL);
    g_assert(g_file_get_contents(attrpath, &contents, &length, NULL));
    g_assert_cmpint(length, ==, 7);
    g_assert_cmpint(memcmp(contents, "\x01\x00\xFF\x00\x05\x40\xA0", 7), ==, 0);
}

static void
t_testbed_set_property(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GUdevDevice *device;
    gchar *prop;
    g_autoptr (GUdevClient) client = g_udev_client_new(NULL);

    g_autofree gchar *syspath = umockdev_testbed_add_device(
            fixture->testbed, "usb", "extkeyboard1", NULL,
            /* attributes */
            NULL,
            /* properties */
            "ID_INPUT", "1", NULL);

    /* change an existing property */
    prop = umockdev_testbed_get_property(fixture->testbed, syspath, "ID_INPUT");
    g_assert_cmpstr(prop, ==, "1");
    g_free (prop);

    umockdev_testbed_set_property(fixture->testbed, syspath, "ID_INPUT", "0");
    prop = umockdev_testbed_get_property(fixture->testbed, syspath, "ID_INPUT");
    g_assert_cmpstr(prop, ==, "0");
    g_free (prop);

    /* add a new one */
    g_assert(umockdev_testbed_get_property(fixture->testbed, syspath, "ID_COLOR") == NULL);
    umockdev_testbed_set_property(fixture->testbed, syspath, "ID_COLOR", "green");
    prop = umockdev_testbed_get_property(fixture->testbed, syspath, "ID_COLOR");
    g_assert_cmpstr(prop, ==, "green");
    g_free (prop);

    /* int properties */
    umockdev_testbed_set_property_int(fixture->testbed, syspath, "COUNT", 1000);
    umockdev_testbed_set_property_hex(fixture->testbed, syspath, "ADDR", 0x1a01);

    device = g_udev_client_query_by_sysfs_path(client, syspath);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT"), ==, "0");
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_COLOR"), ==, "green");
    g_assert_cmpstr(g_udev_device_get_property(device, "COUNT"), ==, "1000");
    g_assert_cmpstr(g_udev_device_get_property(device, "ADDR"), ==, "1a01");
    g_object_unref(device);
}

static void
t_testbed_uevent_libudev(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    struct udev *udev;
    struct udev_monitor *udev_mon, *kernel_mon;
    struct udev_device *device;

    g_autofree gchar *syspath = umockdev_testbed_add_device(
            fixture->testbed, "pci", "mydev", NULL,
            /* attributes */
            "idVendor", "0815", NULL,
            /* properties */
            "ID_INPUT", "1", NULL);
    g_assert(syspath);

    /* set up monitors */
    udev = udev_new();
    g_assert(udev != NULL);
    udev_mon = udev_monitor_new_from_netlink(udev, "udev");
    g_assert(udev_mon != NULL);
    g_assert_cmpint(udev_monitor_get_fd(udev_mon), >, 0);
    g_assert_cmpint(udev_monitor_enable_receiving(udev_mon), ==, 0);

    kernel_mon = udev_monitor_new_from_netlink(udev, "kernel");
    g_assert(kernel_mon != NULL);
    g_assert_cmpint(udev_monitor_get_fd(kernel_mon), >, 0);
    g_assert_cmpint(udev_monitor_get_fd(kernel_mon), !=, udev_monitor_get_fd(udev_mon));
    g_assert_cmpint(udev_monitor_enable_receiving(kernel_mon), ==, 0);

    /* generate event */
    umockdev_testbed_uevent(fixture->testbed, syspath, "add");

    /* check that it's on the monitors */
    device = udev_monitor_receive_device(udev_mon);
    g_assert(device != NULL);
    g_assert_cmpstr(udev_device_get_syspath(device), ==, syspath);
    g_assert_cmpstr(udev_device_get_action(device), ==, "add");
    g_assert_cmpstr(udev_device_get_subsystem(device), ==, "pci");
    g_assert_cmpstr(udev_device_get_sysattr_value(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(udev_device_get_property_value(device, "ID_INPUT"), ==, "1");
    udev_device_unref(device);

    device = udev_monitor_receive_device(kernel_mon);
    g_assert(device != NULL);
    g_assert_cmpstr(udev_device_get_syspath(device), ==, syspath);
    g_assert_cmpstr(udev_device_get_action(device), ==, "add");
    g_assert_cmpstr(udev_device_get_subsystem(device), ==, "pci");
    g_assert_cmpstr(udev_device_get_sysattr_value(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(udev_device_get_property_value(device, "ID_INPUT"), ==, "1");
    udev_device_unref(device);

    udev_monitor_unref(udev_mon);
    udev_monitor_unref(kernel_mon);
    udev_unref(udev);

    /* don't trip over netlink sockets that the client side already closed */
    umockdev_testbed_uevent(fixture->testbed, syspath, "remove");
}

static void
t_testbed_uevent_libudev_filter(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    struct udev *udev;
    struct udev_monitor *mon;
    struct udev_device *device;
    const int num_events = 10;

    /* set up monitor */
    udev = udev_new();
    g_assert(udev != NULL);
    mon = udev_monitor_new_from_netlink(udev, "udev");
    g_assert(mon != NULL);
    g_assert_cmpint(udev_monitor_get_fd(mon), >, 0);
    g_assert_cmpint(udev_monitor_filter_add_match_subsystem_devtype(mon, "pci", "fancy"), ==, 0);
    g_assert_cmpint(udev_monitor_filter_update(mon), ==, 0);
    g_assert_cmpint(udev_monitor_enable_receiving(mon), ==, 0);

    gboolean success = umockdev_testbed_add_from_string(
            fixture->testbed,
            "P: /devices/mydev\n"
            "E: SUBSYSTEM=pci\n"
            "A: idVendor=0815\n"
            "E: ID_INPUT=1\n"
            "E: DEVTYPE=fancy\n", NULL);
    g_assert (success);
    const gchar *syspath = "/sys/devices/mydev";

    /* queue a bunch of events */
    for (int i = 0; i < num_events; ++i)
        umockdev_testbed_uevent(fixture->testbed, syspath, "change");

    /* check that they are on the monitors */
    /* first, the add event from add_from_string() */
    device = udev_monitor_receive_device(mon);
    g_assert(device != NULL);
    g_assert_cmpstr(udev_device_get_syspath(device), ==, syspath);
    g_assert_cmpstr(udev_device_get_action(device), ==, "add");
    g_assert_cmpstr(udev_device_get_subsystem(device), ==, "pci");
    g_assert_cmpstr(udev_device_get_devtype(device), ==, "fancy");
    udev_device_unref(device);
    /* now the change events */
    for (int i = 0; i < num_events; ++i) {
        device = udev_monitor_receive_device(mon);
        g_assert(device != NULL);
        g_assert_cmpstr(udev_device_get_syspath(device), ==, syspath);
        g_assert_cmpstr(udev_device_get_action(device), ==, "change");
        g_assert_cmpstr(udev_device_get_subsystem(device), ==, "pci");
        g_assert_cmpstr(udev_device_get_devtype(device), ==, "fancy");
        udev_device_unref(device);
    }
    g_assert(udev_monitor_receive_device(mon) == NULL);

    udev_monitor_unref(mon);
    udev_unref(udev);
}

struct event_counter {
    unsigned add;
    unsigned remove;
    unsigned change;
    gchar last_device[1024];
};

static void
on_uevent(UNUSED GUdevClient *client, const gchar *action, GUdevDevice *device, gpointer user_data)
{
    struct event_counter *counter = (struct event_counter *)user_data;

    g_debug("on_uevent action %s device %s", action, g_udev_device_get_sysfs_path(device));

    if (strcmp(action, "add") == 0)
	counter->add++;
    else if (strcmp(action, "remove") == 0)
	counter->remove++;
    else if (strcmp(action, "change") == 0)
	counter->change++;
    else
	g_assert_not_reached();

    strncpy(counter->last_device, g_udev_device_get_sysfs_path(device), sizeof(counter->last_device) - 1);
}

static gboolean
on_timeout(gpointer user_data)
{
    GMainLoop *mainloop = (GMainLoop *) user_data;
    g_main_loop_quit(mainloop);
    return FALSE;
}


static void
t_testbed_uevent_gudev(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GMainLoop *mainloop;
    struct event_counter counter = { 0, 0, 0 };
    const gchar *subsystems[] = { "bluetooth", NULL };

    g_autofree gchar *syspath = umockdev_testbed_add_device(
            fixture->testbed, "bluetooth", "mydev", NULL,
            /* attributes */
            "idVendor", "0815", NULL,
            /* properties */
            "ID_INPUT", "1", NULL);
    g_assert(syspath);

    /* set up listener for uevent signal */
    g_autoptr(GUdevClient) client = g_udev_client_new(subsystems);
    g_signal_connect(client, "uevent", G_CALLBACK(on_uevent), &counter);

    mainloop = g_main_loop_new(NULL, FALSE);

    /* send a signal and run main loop for 0.5 seconds to catch it */
    umockdev_testbed_uevent(fixture->testbed, syspath, "add");
    g_timeout_add(500, on_timeout, mainloop);
    g_main_loop_run(mainloop);
    g_assert_cmpuint(counter.add, ==, 1);
    g_assert_cmpuint(counter.remove, ==, 0);
    g_assert_cmpuint(counter.change, ==, 0);
    g_assert_cmpstr(counter.last_device, ==, "/sys/devices/mydev");

    counter.add = 0;
    umockdev_testbed_uevent(fixture->testbed, syspath, "change");
    g_timeout_add(500, on_timeout, mainloop);
    g_main_loop_run(mainloop);
    g_assert_cmpuint(counter.add, ==, 0);
    g_assert_cmpuint(counter.remove, ==, 0);
    g_assert_cmpuint(counter.change, ==, 1);
    g_assert_cmpstr(counter.last_device, ==, "/sys/devices/mydev");

    g_main_loop_unref(mainloop);

    /* ensure that properties and attributes are still intact */
    g_autoptr(GUdevDevice) device = g_udev_client_query_by_sysfs_path(client, syspath);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT"), ==, "1");
}

static void
t_testbed_uevent_no_listener(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    g_autofree gchar *syspath = umockdev_testbed_add_device(fixture->testbed, "pci", "mydev", NULL, NULL, NULL);
    g_assert(syspath);

    /* generate event */
    umockdev_testbed_uevent(fixture->testbed, syspath, "add");
}

static void
t_testbed_uevent_error(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    struct udev *udev;
    struct udev_monitor *mon;

    /* set up monitor */
    udev = udev_new();
    g_assert(udev != NULL);
    mon = udev_monitor_new_from_netlink(udev, "udev");
    g_assert(mon != NULL);

    /* unknown device, shouldn't crash but print an error message */
    /* some libcs (musl) have readonly stderr, so only test with glibc */
#ifdef __GLIBC__
    FILE *orig_stderr = stderr;
    char buf[1000];

    stderr = tmpfile();
    g_assert(stderr != NULL);
    umockdev_testbed_uevent(fixture->testbed, "/devices/unknown", "add");
    fflush(stderr);
    rewind(stderr);
    g_assert(fgets(buf, sizeof(buf), stderr) != NULL);
    g_assert_cmpstr(buf, ==, "ERROR: uevent_sender_send: No such device /devices/unknown\n");
    fclose(stderr);
    stderr = orig_stderr;
#endif

    /* should not trigger an actual event */
    g_assert(udev_monitor_receive_device(mon) == NULL);

    udev_monitor_unref(mon);
    udev_unref(udev);
}

static void
t_testbed_uevent_null_action(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    g_autofree gchar *syspath = umockdev_testbed_add_device(fixture->testbed, "pci", "mydev", NULL, NULL, NULL);
    g_assert(syspath);

    if (g_test_subprocess()) {
        umockdev_testbed_uevent(fixture->testbed, syspath, NULL);
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr ("*action != NULL*");
}

static void
t_testbed_uevent_action_overflow(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    g_autofree gchar *syspath = umockdev_testbed_add_device(fixture->testbed, "pci", "mydev", NULL, NULL, NULL);
    g_assert(syspath);

    /* overly long action */
    if (g_test_subprocess()) {
        char long_action[16384];
        memset(long_action, 'a', sizeof(long_action));
        long_action[sizeof(long_action)-1] = '\0';
        umockdev_testbed_uevent(fixture->testbed, syspath, long_action);
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr ("*uevent_sender_send*Property buffer overflow*");
}

static void
t_testbed_uevent_property_overflow(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    /* overly long property */
    if (g_test_subprocess()) {
        char long_name[10000];
        memset(long_name, 'a', sizeof long_name);
        long_name[sizeof long_name - 1] = '\0';

        /* this sends an "add" uevent */
        umockdev_testbed_add_device(fixture->testbed, "pci", "mydev", NULL,
                                    NULL, /* attributes */
                                    long_name, long_name, NULL);
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr ("*uevent_sender_send*Property buffer overflow*");
}

static void
t_testbed_add_from_string(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GUdevDevice *device;
    gchar *contents;
    gsize length;
    gboolean success;
    GError *error = NULL;

    /* start with adding one device */
    success = umockdev_testbed_add_from_string(fixture->testbed,
					       "P: /devices/dev1\n"
					       "E: SIMPLE_PROP=1\n"
					       "E: SUBSYSTEM=pci\n"
					       "H: binary_attr=41A9FF0005FF00\n"
					       "A: multiline_attr=a\\\\b\\nc\\\\d\\nlast\n"
					       "A: simple_attr=1\n"
					       "A: knobs/red=off\n"
					       "L: driver=../../foo", &error);
    g_assert_no_error(error);
    g_assert(success);

    g_autoptr (GUdevClient) client = g_udev_client_new(NULL);

    /* should have exactly one device */
    g_assert_cmpuint(num_udev_devices(), ==, 1);

    /* check properties and attributes */
    device = g_udev_client_query_by_sysfs_path(client, "/sys/devices/dev1");
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_subsystem(device), ==, "pci");
    g_assert(g_udev_device_get_parent(device) == NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "simple_attr"), ==, "1");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "multiline_attr"), ==, "a\\b\nc\\d\nlast");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "knobs/red"), ==, "off");
    g_assert_cmpstr(g_udev_device_get_property(device, "SIMPLE_PROP"), ==, "1");
    g_assert_cmpstr(g_udev_device_get_driver(device), ==, "foo");
    g_object_unref(device);

    g_assert(g_file_get_contents("/sys/devices/dev1/binary_attr", &contents, &length, NULL));
    g_assert_cmpint(length, ==, 7);
    g_assert_cmpint(memcmp(contents, "\x41\xA9\xFF\x00\x05\xFF\x00", 7), ==, 0);
    g_free(contents);

    /* class symlink created */
    g_assert(g_file_test("/sys/devices/dev1/subsystem", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev1/subsystem/devices/dev1", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev1/subsystem/devices/dev1/simple_attr", G_FILE_TEST_IS_REGULAR));

    /* driver symlink created */
    g_assert(g_file_test("/sys/devices/dev1/driver", G_FILE_TEST_IS_SYMLINK));
    contents = g_file_read_link("/sys/devices/dev1/driver", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(contents, ==, "../../foo");
    g_free(contents);

    /* now add two more */
    umockdev_testbed_add_from_string(fixture->testbed,
				     "P: /devices/dev2/subdev1\n"
				     "E: SUBDEV1COLOR=YELLOW\n"
				     "E: SUBSYSTEM=input\n"
				     "A: subdev1color=yellow\n"
				     "\n"
				     "P: /devices/dev2\n"
				     "E: DEV2COLOR=GREEN\n" "E: SUBSYSTEM=hid\n" "A: dev2color=green\n", &error);
    g_assert_no_error(error);

    /* should have three devices now */
    g_assert_cmpuint(num_udev_devices(), ==, 3);

    /* dev1 is still there */
    device = g_udev_client_query_by_sysfs_path(client, "/sys/devices/dev1");
    g_assert_cmpstr(g_udev_device_get_subsystem(device), ==, "pci");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "simple_attr"), ==, "1");
    g_object_unref(device);

    /* check dev2 */
    device = g_udev_client_query_by_sysfs_path(client, "/sys/devices/dev2");
    g_assert(device);
    g_assert(g_udev_device_get_parent(device) == NULL);
    g_assert_cmpstr(g_udev_device_get_subsystem(device), ==, "hid");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "dev2color"), ==, "green");
    g_assert_cmpstr(g_udev_device_get_property(device, "DEV2COLOR"), ==, "GREEN");
    g_assert(g_file_test("/sys/devices/dev2/subsystem", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev2/subsystem/devices/dev2", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev2/subsystem/devices/dev2/dev2color", G_FILE_TEST_IS_REGULAR));

    /* check subdev1 */
    g_autoptr(GUdevDevice) subdev = g_udev_client_query_by_sysfs_path(client, "/sys/devices/dev2/subdev1");
    g_assert(subdev);
    g_autoptr(GUdevDevice) subdev_parent = g_udev_device_get_parent(subdev);
    g_assert_cmpstr(g_udev_device_get_sysfs_path(subdev_parent), ==, "/sys/devices/dev2");
    g_assert_cmpstr(g_udev_device_get_subsystem(subdev), ==, "input");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(subdev, "subdev1color"), ==, "yellow");
    g_assert_cmpstr(g_udev_device_get_property(subdev, "SUBDEV1COLOR"), ==, "YELLOW");
    g_assert(g_file_test("/sys/devices/dev2/subdev1/subsystem", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev2/subdev1/subsystem/subdev1", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev2/subdev1/subsystem/subdev1/subdev1color", G_FILE_TEST_IS_REGULAR));

    g_object_unref(device);
}

static void
t_testbed_add_from_string_errors(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GError *error = NULL;

    /* does not start with P: */
    g_assert(!umockdev_testbed_add_from_string(fixture->testbed, "E: SIMPLE_PROP=1\n", &error));
    g_assert_error(error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE);
    g_clear_error(&error);

    /* no value */
    g_assert(!umockdev_testbed_add_from_string(fixture->testbed, "P: /devices/dev1\n" "E: SIMPLE_PROP\n", &error));
    g_assert_error(error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE);
    g_clear_error(&error);

    /* unknown category */
    g_assert(!umockdev_testbed_add_from_string(fixture->testbed, "P: /devices/dev1\n" "X: SIMPLE_PROP=1\n", &error));
    g_assert_error(error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE);
    g_clear_error(&error);

    /* uneven hex string */
    g_assert(!umockdev_testbed_add_from_string(fixture->testbed,
					       "P: /devices/dev1\n"
					       "E: SUBSYSTEM=usb\n" "H: binary_attr=41F\n", &error));
    g_assert_error(error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE);
    g_clear_error(&error);

    /* invalid device path */
    g_assert(!umockdev_testbed_add_from_string(fixture->testbed, "P: /dev1\n" "E: SUBSYSTEM=usb\n", &error));
    g_assert_error(error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_VALUE);
    g_clear_error(&error);

    /* missing SUBSYSTEM */
    g_assert(!umockdev_testbed_add_from_string(fixture->testbed, "P: /devices/dev1\n" "E: ID_FOO=bar\n", &error));
    g_assert_error(error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_VALUE);
    g_clear_error(&error);
}

static void
t_testbed_add_from_file(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    gboolean success;
    GError *error = NULL;
    g_autofree gchar *contents = NULL;

    success = umockdev_testbed_add_from_file(fixture->testbed, "/non/existing.umockdev", &error);
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
    g_clear_error(&error);
    g_assert(!success);

    /* create umockdev dump file, invalid data */
    g_autofree gchar* path = g_build_filename(fixture->root_dir, "test.umockdev", NULL);
    success = g_file_set_contents(path, "P: /devices/dev1\nX: Whatever\n", -1, &error);
    g_assert_no_error(error);
    g_assert(success);

    success = umockdev_testbed_add_from_file(fixture->testbed, path, &error);
    g_assert_error(error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE);
    g_clear_error(&error);
    g_assert(!success);

    /* create umockdev dump file, valid data*/
    success = g_file_set_contents(path,
                                  "P: /devices/dev1\n"
                                  "E: SUBSYSTEM=pci\n"
                                  "H: binary_attr=41A9FF0005FF00\n"
                                  "A: simple_attr=1\n", -1, &error);
    g_assert_no_error(error);
    g_assert(success);

    success = umockdev_testbed_add_from_file(fixture->testbed, path, &error);
    g_assert_no_error(error);
    g_assert(success);

    /* verify that device was created */
    success = g_file_get_contents("/sys/devices/dev1/simple_attr", &contents, NULL, &error);
    g_assert_no_error(error);
    g_assert(success);
    g_assert_cmpstr(contents, ==, "1");
}

static void
t_testbed_libc(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    gboolean success;
    GError *error = NULL;
    char *path;
    char pathbuf[PATH_MAX];
    int fd;
    struct stat st;
    uid_t uid = getuid();

    /* start with adding one device */
    success = umockdev_testbed_add_from_string(fixture->testbed,
					       "P: /devices/dev1\n"
					       "N: pd1\n"
					       "E: SUBSYSTEM=pci\n"
					       "E: DEVNAME=/dev/pd1\n"
					       "A: simple_attr=1", &error);
    g_assert_no_error(error);
    g_assert(success);

    /* realpath */

    /* dir */
    path = realpath("/sys/devices/dev1", pathbuf);
    g_assert_cmpstr(path, ==, "/sys/devices/dev1");

    /* link */
    path = realpath("/sys/bus/../bus/pci/devices/./dev1", pathbuf);
    g_assert_cmpstr(path, ==, "/sys/devices/dev1");

    /* nonexisting */
    g_assert(realpath("/sys/devices/xxnoexist", pathbuf) == NULL);
    g_assert_cmpint(errno, ==, ENOENT);

    /* dir with dynamic allocation */
    path = realpath("/sys/devices/dev1", NULL);
    g_assert_cmpstr(path, ==, "/sys/devices/dev1");
    g_free(path);

#ifdef __GLIBC__
    /* canonicalize_file_name */

    /* dir */
    path = canonicalize_file_name("/sys/devices/dev1");
    g_assert_cmpstr(path, ==, "/sys/devices/dev1");
    g_free(path);

    /* link */
    path = canonicalize_file_name("/sys/bus/pci/devices/dev1");
    g_assert_cmpstr(path, ==, "/sys/devices/dev1");
    g_free(path);

    /* nonexisting */
    g_assert(canonicalize_file_name("/sys/devices/xxnoexist") == NULL);
    g_assert_cmpint(errno, ==, ENOENT);
#endif

    /* openat */

    int dirfd_root = open("/", O_RDONLY | O_DIRECTORY);
    g_assert_cmpint(dirfd_root, >=, 0);
    int dirfd_bin = open("/bin", O_RDONLY | O_DIRECTORY);
    g_assert_cmpint(dirfd_bin, >=, 0);
    int dirfd_dev = open("/dev", O_RDONLY | O_DIRECTORY);
    int dirfd_sys = open("/sys", O_RDONLY | O_DIRECTORY);

    /* sys/ in root dir should be trapped */
    fd = openat(dirfd_root, "sys/devices/dev1/simple_attr", O_RDONLY);
    if (fd < 0)
        perror("openat");
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    /* dirfd == sys/ should be trapped */
    fd = openat(dirfd_sys, "devices/dev1/simple_attr", O_RDONLY);
    if (fd < 0)
        perror("openat");
    g_assert_cmpint(fd, >=, 0);
    close(fd);

#ifdef HAVE_OPENAT64
    fd = openat64(dirfd_root, "sys/devices/dev1/simple_attr", O_RDONLY);
    if (fd < 0)
        perror("openat64");
    g_assert_cmpint(fd, >=, 0);
    close(fd);
#endif

    /* dev/ in root dir should be trapped */
    fd = openat(dirfd_root, "dev/pd1", O_RDONLY);
    if (fd < 0)
        perror("openat");
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    /* dirfd == dev/ should be trapped */
    fd = openat(dirfd_dev, "pd1", O_RDONLY);
    if (fd < 0)
        perror("openat");
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    /* sys/ in other dir should not be trapped */
    errno = 0;
    g_assert_cmpint(openat(dirfd_bin, "sys", O_RDONLY), <, 0);
    g_assert_cmpint(errno, ==, ENOENT);

    /* dev/ in other dir should not be trapped */
    errno = 0;
    g_assert_cmpint(openat(dirfd_bin, "pd1", O_RDONLY), <, 0);
    g_assert_cmpint(errno, ==, ENOENT);

#ifdef HAVE_OPENAT64
    g_assert_cmpint(openat64(dirfd_bin, "sys", O_RDONLY), <, 0);

    errno = 0;
    g_assert_cmpint(openat(AT_FDCWD, "sys/devices", O_RDONLY), <, 0);
    g_assert_cmpint(errno, ==, ENOENT);
    g_assert_cmpint(openat64(AT_FDCWD, "sys/devices", O_RDONLY), <, 0);
#endif

#if defined(__GLIBC__) && defined(HAVE_OPEN_TREE)
    /* open_tree */

    /* sys/ in root dir should be trapped */
    fd = open_tree(dirfd_root, "sys/devices/dev1/simple_attr", O_RDONLY);
    if (fd < 0 && errno == ENOSYS) {
        /* can happen under valgrind or old kernels */
        g_printf("SKIP: open_tree() not available\n");
    } else {
        if (fd < 0)
            perror("openat");
        g_assert_cmpint(fd, >=, 0);
        close(fd);

        /* dev/ in root dir should be trapped */
        fd = open_tree(dirfd_root, "dev/pd1", O_RDONLY);
        if (fd < 0)
            perror("openat");
        g_assert_cmpint(fd, >=, 0);
        close(fd);

        /* sys/ in other dir should not be trapped */
        errno = 0;
        g_assert_cmpint(open_tree(dirfd_bin, "sys", O_RDONLY), <, 0);
        g_assert_cmpint(errno, ==, ENOENT);
    }
#endif

    close(dirfd_root);
    close(dirfd_bin);
    close(dirfd_dev);
    close(dirfd_sys);

    /* stat */
    g_assert_cmpint(stat ("/sys/bus/pci/devices", &st), ==, 0);
    g_assert_cmpint(st.st_nlink, >=, 1);
    g_assert_cmpint(st.st_nlink, <=, 2);
    g_assert_cmpuint(st.st_uid, ==, uid);
    g_assert(S_ISDIR(st.st_mode));

    g_assert_cmpint(lstat ("/sys/bus/pci/devices/dev1", &st), ==, 0);
    g_assert_cmpuint(st.st_uid, ==, uid);
    g_assert(S_ISLNK(st.st_mode));

    g_assert_cmpint(stat ("/sys/bus/pci/devices/dev1", &st), ==, 0);
    g_assert_cmpuint(st.st_uid, ==, uid);
    g_assert(S_ISDIR(st.st_mode));

    g_assert_cmpint(fstatat (AT_FDCWD, "/sys/bus/pci/devices", &st, 0), ==, 0);
    g_assert_cmpuint(st.st_uid, ==, uid);
    g_assert(S_ISDIR(st.st_mode));

    g_assert_cmpint(fstatat (AT_FDCWD, "/sys/bus/pci/devices/dev1", &st, AT_SYMLINK_NOFOLLOW), ==, 0);
    g_assert_cmpuint(st.st_uid, ==, uid);
    g_assert(S_ISLNK(st.st_mode));

    g_assert_cmpint(fstatat (AT_FDCWD, "/sys/bus/pci/devices/dev1", &st, 0), ==, 0);
    g_assert_cmpuint(st.st_uid, ==, uid);
    g_assert(S_ISDIR(st.st_mode));

#ifdef __GLIBC__
    /* statx */
    struct statx stx;
    g_assert_cmpint(statx (AT_FDCWD, "/sys/bus/pci/devices", 0, STATX_TYPE|STATX_NLINK|STATX_UID, &stx), ==, 0);
    g_assert_cmpint(stx.stx_nlink, >=, 1);
    g_assert_cmpint(stx.stx_nlink, <=, 2);
    g_assert_cmpuint(stx.stx_uid, ==, uid);
    g_assert(S_ISDIR(stx.stx_mode));

    g_assert_cmpint(statx (AT_FDCWD, "/sys/bus/pci/devices/dev1", AT_SYMLINK_NOFOLLOW, STATX_TYPE|STATX_NLINK|STATX_UID, &stx), ==, 0);
    g_assert_cmpuint(stx.stx_uid, ==, uid);
    g_assert(S_ISLNK(stx.stx_mode));

    g_assert_cmpint(statx (AT_FDCWD, "/sys/bus/pci/devices/dev1", 0, STATX_TYPE|STATX_NLINK|STATX_UID, &stx), ==, 0);
    g_assert_cmpuint(stx.stx_uid, ==, uid);
    g_assert(S_ISDIR(stx.stx_mode));

    struct statfs buf;
    int dirfd = open("/sys", O_RDONLY | O_DIRECTORY);
    g_assert_cmpint(dirfd, >=, 0);
    g_assert_cmpint(fstatfs(dirfd, &buf), ==, 0);
    g_assert_cmpint(buf.f_type, ==, SYSFS_MAGIC);
    close (dirfd);
    memset(&buf, 0, sizeof buf);

    dirfd = open("/sys/bus/pci/devices/dev1", O_RDONLY | O_DIRECTORY);
    g_assert_cmpint(dirfd, >=, 0);
    g_assert_cmpint(fstatfs(dirfd, &buf), ==, 0);
    g_assert_cmpint(buf.f_type, ==, SYSFS_MAGIC);
    close (dirfd);
    memset(&buf, 0, sizeof buf);

    dirfd = open("/dev", O_RDONLY | O_DIRECTORY);
    g_assert_cmpint(dirfd, >=, 0);
    g_assert_cmpint(fstatfs(dirfd, &buf), ==, 0);
    g_assert_cmpint(buf.f_type, !=, SYSFS_MAGIC);
    close (dirfd);
    memset(&buf, 0, sizeof buf);

    g_assert_cmpint(statfs("/sys", &buf), ==, 0);
    g_assert_cmpint(buf.f_type, ==, SYSFS_MAGIC);
    memset(&buf, 0, sizeof buf);

    g_assert_cmpint(statfs("/sys/bus/pci/devices/dev1", &buf), ==, 0);
    g_assert_cmpint(buf.f_type, ==, SYSFS_MAGIC);
    memset(&buf, 0, sizeof buf);

    g_assert_cmpint(statfs("/dev", &buf), ==, 0);
    g_assert_cmpint(buf.f_type, !=, SYSFS_MAGIC);
    memset(&buf, 0, sizeof buf);
#endif
}

static void
t_testbed_usb_lsusb(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    g_autofree gchar *out = NULL, *err = NULL;
    int exit_status;
    GError *error = NULL;
    g_autofree gchar *lsusb_path = g_find_program_in_path("lsusb");
    gchar *argv[] = { lsusb_path, "-v", NULL };

    if (lsusb_path == NULL) {
	g_printf("SKIP: lsusb not installed. ");
	return;
    }

    g_autofree gchar *syspath = umockdev_testbed_add_device(
            fixture->testbed, "usb", "usb1", NULL,
            /* attributes */
            "busnum", "1\n", "devnum", "1\n", "speed", "480",
            "bConfigurationValue", "1", NULL,
            /* properties */
            "DEVTYPE", "usb_device", "DEVNAME", "/dev/bus/usb/usb1", NULL);
    g_assert(syspath);
    /* descriptor from a Canon PowerShot SX200 */
    umockdev_testbed_set_attribute_binary(fixture->testbed, syspath, "descriptors", (guint8 *)
					  "\x12\x01\x00\x02\x00\x00\x00\x40\xa9\x04\xc0\x31\x02\x00\x01\x02"
					  "\x03\x01\x09\x02\x27\x00\x01\x01\x00\xc0\x01\x09\x04\x00\x00\x03"
					  "\x06\x01\x01\x00\x07\x05\x81\x02\x00\x02\x00\x07\x05\x02\x02\x00"
					  "\x02\x00\x07\x05\x83\x03\x08\x00\x09", 57);

    /* ensure that /dev/bus/usb/ exists, lsusb insists on it */
    g_autofree gchar *dir = g_build_filename(fixture->root_dir, "dev", "bus", "usb", "002", NULL);
    g_mkdir_with_parents(dir, 0755);

    g_assert(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &exit_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(exit_status, ==, 0);

    /* g_printf("------ out: -------\n%s\n------ err: ------\n%s\n-----\n", out, err); */
    /* FIXME: In NixOS sandbox lsusb errors with "Couldn't open device, some information will be missing", and bus/device are 0 */
    /* https://github.com/martinpitt/umockdev/issues/115 */
    if (g_getenv("NIX_BUILD_TOP"))
        g_assert(strstr(out, ": ID 04a9:31c0"));
    else
        g_assert(g_str_has_prefix(out, "\nBus 001 Device 001: ID 04a9:31c0"));
    g_assert(strstr(out, "idVendor           0x04a9"));

    /* Alpine's lsusb doesn't read usb.ids, and our test container does not ship it */
    if (g_file_test("/var/lib/usbutils/usb.ids", G_FILE_TEST_EXISTS))
        g_assert(strstr(out, "Canon, Inc. PowerShot SX200 IS"));
}

#define assert_dev_zero_stat(st) \
{ \
    g_assert_cmpint((st)->st_uid, ==, uid); \
    g_assert(S_ISCHR((st)->st_mode)); \
    g_assert_cmpint((st)->st_rdev, ==, 0); \
}

#define assert_dev_zero_fstat(fd) \
{ \
    GStatBuf st_macro; \
    g_assert_cmpint((fd), >, 0); \
    g_assert_cmpint(fstat((fd), &st_macro), ==, 0); /* we don't set any dev= */ \
    close(fd); \
    assert_dev_zero_stat(&st_macro); \
}

static void
t_testbed_dev_access(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GStatBuf st;
    gchar *devpath;
    int fd;
    FILE* f;
    char buf[100];
    uid_t uid = getuid();

    /* no mocked devices */
    g_assert_cmpint(g_open("/dev/wishyouwerehere", O_RDONLY, 0), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    g_assert_cmpint(g_stat("/dev/zero", &st), ==, 0);
    g_assert(S_ISCHR(st.st_mode));
    fd = g_open("/dev/zero", O_RDONLY, 0);
    g_assert_cmpint(fd, >, 0);
    g_assert(!isatty(fd));
    g_assert_cmpint(read(fd, buf, 20), ==, 20);
    close(fd);
    g_assert_cmpint(buf[0], ==, 0);
    g_assert_cmpint(buf[1], ==, 0);
    g_assert_cmpint(buf[9], ==, 0);

    /* create a mock /dev/zero */
    g_autofree gchar *devdir = g_build_filename(fixture->root_dir, "dev", NULL);
    devpath = g_build_filename(devdir, "zero", NULL);
    g_mkdir(devdir, 0755);
    g_assert(g_file_set_contents(devpath, "zerozerozero", -1, NULL));
    g_free(devpath);

    /* now /dev/zero should be the mocked one */
    g_assert_cmpint(g_open("/dev/wishyouwerehere", O_RDONLY, 0), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);

    g_assert_cmpint(g_stat("/dev/zero", &st), ==, 0);
    assert_dev_zero_stat(&st);

    memset(&st, 42, sizeof st);
    g_assert_cmpint(fstatat(AT_FDCWD, "/dev/zero", &st, 0), ==, 0);
    assert_dev_zero_stat(&st);

#ifdef __GLIBC__
    struct statx stx;
    g_assert_cmpint(statx (AT_FDCWD, "/dev/zero", 0, STATX_TYPE|STATX_UID, &stx), ==, 0);
    g_assert_cmpuint(stx.stx_uid, ==, uid);
    g_assert(S_ISCHR(stx.stx_mode));
#endif

    fd = g_open("/dev/zero", O_RDONLY, 0);
    g_assert_cmpint(fd, >, 0);
    g_assert(!isatty(fd));
    g_assert_cmpint(read(fd, buf, 20), ==, 12);
    close(fd);
    g_assert_cmpint(buf[0], ==, 'z');
    g_assert_cmpint(buf[1], ==, 'e');
    g_assert_cmpint(buf[11], ==, 'o');
    g_assert_cmpint(buf[12], ==, 0);
    memset(buf, 0, sizeof(buf));

    /* /dev/zero with fopen() */
    g_assert(fopen("/dev/wishyouwerehere", "r") == NULL);
    g_assert_cmpint(errno, ==, ENOENT);
    f = fopen("/dev/zero", "rb");
    g_assert(f != NULL);
    g_assert(!isatty(fileno(f)));
    g_assert_cmpint(fread(buf, 1, 100, f), ==, 12);
    fclose(f);
    g_assert_cmpint(buf[0], ==, 'z');
    g_assert_cmpint(buf[1], ==, 'e');
    g_assert_cmpint(buf[11], ==, 'o');
    g_assert_cmpint(buf[12], ==, 0);
    memset(buf, 0, sizeof(buf));

    /* symlinks should also work */
    devpath = g_build_filename(devdir, "wishyouwerehere", NULL);
    g_assert_cmpint(symlink("zero", devpath), ==, 0);
    g_free(devpath);
    g_assert_cmpint(g_lstat("/dev/wishyouwerehere", &st), ==, 0);
    g_assert(S_ISLNK(st.st_mode));
    fd = g_open("/dev/wishyouwerehere", O_RDONLY, 0);
    g_assert_cmpint(fd, >, 0);
    g_assert_cmpint(read(fd, buf, 20), ==, 12);
    close(fd);
    g_assert_cmpint(buf[0], ==, 'z');
    memset(buf, 0, sizeof(buf));

    /* directories should appear as actual directories */
    devpath = g_build_filename(devdir, "usb", NULL);
    g_assert_cmpint(g_mkdir(devpath, 0755), ==, 0);
    g_free(devpath);
    g_assert_cmpint(g_lstat("/dev/usb", &st), ==, 0);
    g_assert(S_ISDIR(st.st_mode));

    /* real TTY devices */
    fd = g_open("/dev/tty", O_RDONLY, 0);
    if (fd > 0) {
        /* we might not always have a tty, like in package builds */
        g_assert(isatty(fd));
        close(fd);
    }

    /* open() with O_TMPFILE; this hasn't been supported in Linux for very long
     * (>= 3.11), so check that it works in the testbed only if it also works
     * in the "normal" file system. */
    fd = g_open(g_get_tmp_dir(), O_TMPFILE|O_RDWR, 0644);
    if (fd >= 0) {
        close(fd);
        errno = 0;
        fd = g_open("/dev", O_TMPFILE|O_RDWR, 0644);
        g_assert_cmpint(errno, ==, 0);
        g_assert_cmpint(fd, >, 0);
        g_assert_cmpint(write(fd, "hello", 5), ==, 5);
        close(fd);
    } else {
        g_printf("(Skipping O_TMPFILE test, not supported on this kernel: %m) ");
    }

    /* fstat() via standard open() */
    fd = g_open("/dev/zero", O_RDONLY, 0);
    assert_dev_zero_fstat(fd);

    /* fstat() via openat() */
    int dirfd = g_open("/dev", O_RDONLY, 0);
    g_assert_cmpint(dirfd, >, 0);
    fd = openat(dirfd, "zero", O_RDONLY, 0);
    close(dirfd);
    assert_dev_zero_fstat(fd);

    /* fstat() on duplicated fd */
    int fd1 = g_open("/dev/zero", O_RDONLY, 0);
    int fd2 = dup(fd1);
    g_assert_cmpint(fd1, !=, fd2);
    assert_dev_zero_fstat(fd1);
    assert_dev_zero_fstat(fd2);
}

static void
t_testbed_add_from_string_dev_char(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GError *error = NULL;
    gchar *contents, *target;
    gsize length;
    GStatBuf st;

    /* N: without value should create an empty dev */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/empty\nN: empty\n"
					      "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/empty\n"
					      "A: dev=1:3\n", &error));
    g_assert_no_error(error);

    g_assert_cmpint(umockdev_testbed_get_dev_fd(fixture->testbed, "/dev/empty"), >, 0);
    g_assert_cmpint(g_stat("/dev/empty", &st), ==, 0);
    g_assert(S_ISCHR(st.st_mode));
    g_assert_cmpint(st.st_rdev, ==, makedev(1, 3));

    /* fstat() should also work */
    int fd = g_open("/dev/empty", O_RDONLY, 0);
    g_assert_cmpint(fd, >, 0);
    g_assert_cmpint(fstat(fd, &st), ==, 0);
    close(fd);
    g_assert(S_ISCHR(st.st_mode));
    g_assert_cmpint(st.st_rdev, ==, makedev(1, 3));

    /* N: with S: should create a symlink to DEVNAME */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/target\nN: target\nS: link\n"
					      "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/target\n"
					      "A: dev=1:4\n", &error));
    g_assert_no_error(error);

    contents = g_file_read_link("/dev/link", &error);
    g_assert_no_error(error);
    target = g_build_filename(fixture->root_dir, "dev/target", NULL);
    g_assert_cmpstr(contents, ==, target);
    g_free(target);
    g_free(contents);

    /* N: another N without value whose name looks like hex */
    umockdev_testbed_add_from_string(fixture->testbed,
				     "P: /devices/001\nN: bus/usb/001\n"
				     "E: BUSNUM=001\nE: SUBSYSTEM=foo\nE: DEVNAME=/dev/bus/usb/001\n"
				     "A: dev=189:1\n", &error);
    g_assert_no_error(error);

    g_assert_cmpint(umockdev_testbed_get_dev_fd(fixture->testbed, "/dev/bus/usb/001"), >, 0);

    /* N: with value should set that contents */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/preset\nN: bus/usb/preset=00FF614100\n"
					      "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/bus/usb/preset\n"
					      "A: dev=189:2\n", &error));
    g_assert_no_error(error);

    g_assert(g_file_get_contents("/dev/bus/usb/preset", &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpint(length, ==, 5);
    g_assert_cmpint(memcmp(contents, "\x00\377aA\x00", 2), ==, 0);
    g_free(contents);
    g_assert_cmpint(g_stat("/dev/bus/usb/preset", &st), ==, 0);
    g_assert(S_ISCHR(st.st_mode));
    g_assert_cmpint(st.st_rdev, ==, makedev(189, 2));
}

static void
t_testbed_add_from_string_dev_block(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GError *error = NULL;
    gchar *contents;
    gsize length;
    GStatBuf st;

    /* N: without value should create an empty dev */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/empty\n"
					      "A: dev=5:4\n"
					      "N: empty\n"
					      "E: SUBSYSTEM=block\n"
					      "E: DEVNAME=/dev/empty\n", &error));
    g_assert_no_error(error);

    g_assert(g_file_get_contents("/dev/empty", &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpint(length, ==, 0);
    g_assert_cmpstr(contents, ==, "");
    g_free(contents);
    g_assert_cmpint(g_stat("/dev/empty", &st), ==, 0);
    g_assert(S_ISBLK(st.st_mode));

    memset(&st, 42, sizeof st);
    g_assert_cmpint(fstatat(AT_FDCWD, "/dev/empty", &st, 0), ==, 0);
    g_assert_cmpuint(st.st_uid, ==, getuid());
    g_assert(S_ISBLK(st.st_mode));

    /* fstat() should also work */
    int fd = g_open("/dev/empty", O_RDONLY, 0);
    g_assert_cmpint(fd, >, 0);
    g_assert_cmpint(fstat(fd, &st), ==, 0);
    close(fd);
    g_assert(S_ISBLK(st.st_mode));
    g_assert(st.st_rdev == makedev(5, 4));

#ifdef __GLIBC__
    struct statx stx;
    g_assert_cmpint(statx (AT_FDCWD, "/dev/empty", 0, STATX_TYPE|STATX_UID, &stx), ==, 0);
    g_assert_cmpuint(stx.stx_uid, ==, getuid());
    g_assert(S_ISBLK(stx.stx_mode));
#endif

    /* N: with value should set that contents */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/block/filled\n"
					      "N: sdf=00FF614100\n"
					      "E: SUBSYSTEM=block\n" "E: DEVNAME=/dev/sdf\n", &error));
    g_assert_no_error(error);

    g_assert(g_file_get_contents("/dev/sdf", &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpint(length, ==, 5);
    g_assert_cmpint(memcmp(contents, "\x00\377aA\x00", 2), ==, 0);
    g_free(contents);
    g_assert_cmpint(g_stat("/dev/sdf", &st), ==, 0);
    g_assert(S_ISBLK(st.st_mode));

#ifdef __GLIBC__
    g_assert_cmpint(statx (AT_FDCWD, "/dev/sdf", 0, STATX_TYPE|STATX_UID, &stx), ==, 0);
    g_assert_cmpuint(stx.stx_uid, ==, getuid());
    g_assert(S_ISBLK(stx.stx_mode));
#endif

    /* listxattr() */
    char xattr_buf[1024];
    ssize_t res = listxattr("/dev/empty", xattr_buf, sizeof(xattr_buf));
    g_assert_cmpint(res, >=, 0);

    res = listxattr("/dev/sdf", xattr_buf, sizeof(xattr_buf));
    g_assert_cmpint(res, >=, 0);

    res = listxattr("/nonexisting", xattr_buf, sizeof(xattr_buf));
    g_assert_cmpint(res, ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);

    /* llistxattr() */
    res = llistxattr("/dev/empty", xattr_buf, sizeof(xattr_buf));
    g_assert_cmpint(res, >=, 0);

    res = llistxattr("/dev/sdf", xattr_buf, sizeof(xattr_buf));
    g_assert_cmpint(res, >=, 0);

    res = listxattr("/nonexisting", xattr_buf, sizeof(xattr_buf));
    g_assert_cmpint(res, ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
}

static void
t_testbed_dev_query_gudev(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GError *error = NULL;
    GUdevDevice *device;
    g_autoptr (GUdevClient) client = g_udev_client_new(NULL);

    /* add fake char and block device */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/stream\nN: stream\n"
					      "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/stream\n"
					      "E: MAJOR=4\nE: MINOR=1\n"
					      "A: dev=4:1\n", &error));
    g_assert_no_error(error);

    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/block/disk\nN: disk\n"
					      "E: SUBSYSTEM=block\nE: DEVNAME=/dev/disk\n"
					      "E: MAJOR=8\nE: MINOR=1\n"
					      "A: dev=8:1\n", &error));
    g_assert_no_error(error);

    g_assert(client);

    device = g_udev_client_query_by_device_file(client, "/dev/stream");
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_device_file(device), ==, "/dev/stream");
    g_assert(g_str_has_suffix(g_udev_device_get_sysfs_path(device), "/devices/stream"));
    g_object_unref(device);

    device = g_udev_client_query_by_device_file(client, "/dev/disk");
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_device_file(device), ==, "/dev/disk");
    g_assert(g_str_has_suffix(g_udev_device_get_sysfs_path(device), "/block/disk"));
    g_object_unref(device);
}

#define ASSERT_EOF \
  g_assert_cmpint(read(fd, buf, 5), ==, -1);    \
  g_assert_cmpint(errno, ==, EAGAIN);           \
  errno = 0;

static void
t_testbed_script_replay_evdev_event_framing(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  g_autofree char *tmppath = NULL;
  int fd;
  char buf[1024];

#if __BITS_PER_LONG != 32 || !defined(__USE_TIME_BITS64)
  struct timeval dummy = {0};
#else
  __kernel_ulong_t dummy = 0;
#endif

  /* Simple evdev stream - x coordinate followed by SYN, times 2 */
  struct input_event dummy_events[] = {
      {dummy, 0003, 0000, 2534},
      {dummy, 0000, 0000, 0000},
      {dummy, 0003, 0000, 2200},
      {dummy, 0000, 0000, 0000}
  };

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/event1\nN: input/event1\n"
          "A: dev=13:65\n"
          "E: DEVNAME=/dev/input/event1\nE: SUBSYSTEM=input\n", &error);
  g_assert_no_error(error);

  /* write script into temporary file */
  fd = g_file_open_tmp("test_script_simple.XXXXXX", &tmppath, &error);
  g_assert_no_error(error);
  g_assert_cmpint(write(fd, "r 0 ", strlen("r 0 ")), ==, strlen("r 0 "));
  for (int i = 0; i < sizeof dummy_events; ++i) {
    char cur = ((char *)dummy_events)[i];
    if (cur < 32) {
      g_assert_cmpint(write(fd, "^", 1), ==, 1);
      cur += 64;
    } else if (cur == '^') {
      g_assert_cmpint(write(fd, "^", 1), ==, 1);
      cur = '`';
    }
    g_assert_cmpint(write(fd, &cur, 1), ==, 1);
  }
  close(fd);

  /* load it */
  success = umockdev_testbed_load_script(fixture->testbed, "/dev/input/event1", tmppath, &error);
  g_assert_no_error(error);
  g_assert(success);
  g_unlink (tmppath);

  /* start communication */
  fd = g_open("/dev/input/event1", O_RDWR | O_NONBLOCK, 0);
  g_assert_cmpint(fd, >=, 0);

  /* don't pretend that this is a tty, even though the mocked one is */
  g_assert(!isatty(fd));

  /* Evdev guarantees that we only read whole events
   * We should do the same
   */
  ssize_t bytes_read = 0;
  while (bytes_read < sizeof dummy_events) {
    ssize_t tmp = read(fd, buf, sizeof buf);
    if (tmp < 0) {
      g_assert_cmpint(errno, ==, EAGAIN);
      errno = 0;
      continue;
    }
    bytes_read += tmp;
    g_assert_cmpint(bytes_read % sizeof(struct input_event), ==, 0);
  }
  g_assert_cmpint(memcmp(buf, dummy_events, sizeof dummy_events), ==, 0);

  /* end of script */
  ASSERT_EOF;

  close(fd);
}

static void
t_testbed_script_replay_simple(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  g_autofree char *tmppath = NULL;
  int fd;
  char buf[1024];

  static const char* test_script = "r 200 ready\n\
w 0 abc\n\
w 2 defgh\n\
r 10 response^I1^J\n\
\n\
# pretty unicode\n\
r 2  hello world ☺\n\
w 10 A\n\
w 10 T\n\
w 10 I\n\
w 10 ^M\n\
r 20 Bogus Device\n\
w 10 split write\n\
\n\
r 10 ACK\n\
# some corner cases in encoding\n\
w 0 ^@^^^`^@a\n\
r 0 ^@^^^`^@a\n";

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/greeter\nN: greeter\n"
          "E: DEVNAME=/dev/greeter\nE: SUBSYSTEM=tty\nA: dev=4:64\n", &error);
  g_assert_no_error(error);

  /* write script into temporary file */
  fd = g_file_open_tmp("test_script_simple.XXXXXX", &tmppath, &error);
  g_assert_no_error(error);
  g_assert_cmpint(write(fd, test_script, strlen(test_script)), >, 10);
  close(fd);

  /* load it */
  success = umockdev_testbed_load_script(fixture->testbed, "/dev/greeter", tmppath, &error);
  g_assert_no_error(error);
  g_assert(success);
  g_unlink (tmppath);

  /* start communication */
  fd = g_open("/dev/greeter", O_RDWR | O_NONBLOCK, 0);
  g_assert_cmpint(fd, >=, 0);
  g_assert(isatty(fd));

  /* should get initial greeting after 200 ms */
  ASSERT_EOF;
  t_testbed_read_buf_delay(220000, fd, buf, 5);
  g_assert(strncmp(buf, "ready", 5) == 0);
  g_assert_cmpint(errno, ==, 0);

  /* do the two write blocks in two matching calls; check that nothing is
   * readable before */
  ASSERT_EOF;
  g_assert_cmpint(write(fd, "abc", 3), ==, 3);
  ASSERT_EOF;
  g_assert_cmpint(write(fd, "defgh", 5), ==, 5);

  /* now we should get the response after 10 ms */
  ASSERT_EOF;
  t_testbed_read_buf_delay(20000, fd, buf, 11);
  g_assert(strncmp(buf, "response\t1\n", 11) == 0);
  g_assert_cmpint(errno, ==, 0);
  t_testbed_read_buf_delay(5000, fd, buf, 15);
  g_assert(strncmp(buf, "hello world ☺\n", 15) == 0);
  g_assert_cmpint(errno, ==, 0);
  ASSERT_EOF;

  /* joined write: write the ATI^M in one block;
     wait 2.5 times the original total write delay */
  usleep(100000);
  g_assert_cmpint(write(fd, "ATI\r", 4), ==, 4);

  /* response after 20 ms */
  t_testbed_read_buf_delay(30000, fd, buf, 12);
  g_assert(strncmp(buf, "Bogus Device", 12) == 0);
  ASSERT_EOF;

  /* split write: write original block in two blocks */
  usleep(5000);
  g_assert_cmpint(write(fd, "split ", 6), ==, 6);
  usleep(5000);
  g_assert_cmpint(write(fd, "write", 5), ==, 5);

  /* response after 10 ms */
  ASSERT_EOF;
  t_testbed_read_buf_delay(20000, fd, buf, 3);
  g_assert(strncmp(buf, "ACK", 3) == 0);

  /* corner cases in encoding */
  g_assert_cmpint(write(fd, "\0\x1E^\0a", 5), ==, 5);
  t_testbed_read_buf_delay(10000, fd, buf, 5);
  g_assert_cmpint(buf[0], ==, 0);
  g_assert_cmpint(buf[1], ==, '\x1E');
  g_assert_cmpint(buf[2], ==, '^');
  g_assert_cmpint(buf[3], ==, 0);
  g_assert_cmpint(buf[4], ==, 'a');

  /* end of script */
  ASSERT_EOF;

  /* waiting is a no-op */
  success = umockdev_testbed_wait_script(fixture->testbed, "/dev/greeter", &error);
  g_assert(success);
  g_assert_no_error(error);

  close(fd);
}

static void
t_testbed_script_replay_default_device(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  g_autofree char *tmppath = NULL;
  int fd;
  char buf[1024];

  static const char* test_script = "d 0 /dev/greeter\n\
r 0 OK\n";

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/greeter\nN: greeter\n"
          "E: DEVNAME=/dev/greeter\nE: SUBSYSTEM=tty\nA: dev=4:64\n", &error);
  g_assert_no_error(error);

  /* write script into temporary file */
  fd = g_file_open_tmp("test_script_simple.XXXXXX", &tmppath, &error);
  g_assert_no_error(error);
  g_assert_cmpint(write(fd, test_script, strlen(test_script)), >, 10);
  close(fd);

  /* load it */
  success = umockdev_testbed_load_script(fixture->testbed, NULL, tmppath, &error);
  g_assert_no_error(error);
  g_assert(success);
  g_unlink (tmppath);

  /* start communication */
  fd = g_open("/dev/greeter", O_RDWR, 0);
  g_assert_cmpint(fd, >=, 0);

  /* check that we've got the right thing on the other end */
  g_assert_cmpint(read(fd, buf, 2), ==, 2);
  g_assert_cmpint(memcmp(buf, "OK", 2), ==, 0);

  close(fd);
}

static void
t_testbed_script_replay_override_default_device(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  g_autofree char *tmppath = NULL;
  int fd;
  char buf[1024];

  static const char* test_script = "d 0 /dev/bananas\n\
r 0 OK\n";

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/greeter\nN: greeter\n"
          "E: DEVNAME=/dev/greeter\nE: SUBSYSTEM=tty\nA: dev=4:64\n", &error);
  g_assert_no_error(error);

  /* write script into temporary file */
  fd = g_file_open_tmp("test_script_simple.XXXXXX", &tmppath, &error);
  g_assert_no_error(error);
  g_assert_cmpint(write(fd, test_script, strlen(test_script)), >, 10);
  close(fd);

  /* load it */
  success = umockdev_testbed_load_script(fixture->testbed, "/dev/greeter", tmppath, &error);
  g_assert_no_error(error);
  g_assert(success);
  g_unlink (tmppath);

  /* start communication */
  fd = g_open("/dev/greeter", O_RDWR, 0);
  g_assert_cmpint(fd, >=, 0);

  /* check that we've got the right thing on the other end */
  g_assert_cmpint(read(fd, buf, 2), ==, 2);
  g_assert_cmpint(memcmp(buf, "OK", 2), ==, 0);

  close(fd);
}

static void
t_testbed_script_replay_wait(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  char buf[1024];

  static const char* test_script = "r 100 Hello \n\
r 100 world\n";

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/greeter\nN: greeter\n"
          "E: DEVNAME=/dev/greeter\nE: SUBSYSTEM=tty\nA: dev=4:64\n", &error);
  g_assert_no_error(error);

  /* load it */
  success = umockdev_testbed_load_script_from_string(fixture->testbed, "/dev/greeter", test_script, &error);
  g_assert_no_error(error);
  g_assert(success);

  /* wait for it; this writes the output into the pipe buffer, and theoretically may block
   * take the risk for the unit test, it's small enough */
  success = umockdev_testbed_wait_script(fixture->testbed, "/dev/greeter", &error);
  g_assert(success);
  g_assert_no_error(error);

  /* start communication */
  int fd = g_open("/dev/greeter", O_RDWR, 0);
  g_assert_cmpint(fd, >=, 0);

  /* we get the full message in a single read */
  g_assert_cmpint(read(fd, buf, sizeof buf), ==, 11);
  g_assert_cmpint(memcmp(buf, "Hello world", 11), ==, 0);

  close(fd);

  /* device has no script any more */
  success = umockdev_testbed_wait_script(fixture->testbed, "/dev/greeter", &error);
  g_assert_false(success);
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_clear_error(&error);

  /* can load a new script into the same device after waiting */
  success = umockdev_testbed_load_script_from_string(fixture->testbed, "/dev/greeter", "r 50 Again\n", &error);
  g_assert_no_error(error);
  g_assert(success);
  success = umockdev_testbed_wait_script(fixture->testbed, "/dev/greeter", &error);
  g_assert(success);
  g_assert_no_error(error);
  fd = g_open("/dev/greeter", O_RDWR, 0);
  g_assert_cmpint(fd, >=, 0);
  g_assert_cmpint(read(fd, buf, sizeof buf), ==, 5);
  g_assert_cmpint(memcmp(buf, "Again", 5), ==, 0);
  close(fd);

  /* invalid device */
  success = umockdev_testbed_wait_script(fixture->testbed, "/dev/invalid", &error);
  g_assert_false(success);
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_clear_error(&error);
}

static void
t_testbed_script_replay_socket_stream(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  g_autofree char *tmppath = NULL, *sockpath = NULL;
  int fd;
  struct sockaddr_un saddr;
  char buf[1024];

  static const char* test_script = "r 200 ready\n\
w 0 abc\n\
w 2 ^@defgh\n\
r 10 ^@response\n";

  /* write script into temporary file */
  fd = g_file_open_tmp("test_script_socket.XXXXXX", &tmppath, &error);
  g_assert_no_error(error);
  g_assert_cmpint(write(fd, test_script, strlen(test_script)), >, 10);
  close(fd);

  /* load it */
  success = umockdev_testbed_load_socket_script(fixture->testbed, "/dev/socket/chatter",
                                                SOCK_STREAM, tmppath, &error);
  g_assert_no_error(error);
  g_assert(success);

  sockpath = g_build_filename(fixture->root_dir, "dev/socket/chatter", NULL);
  g_assert(g_file_test(sockpath, G_FILE_TEST_EXISTS));

  /* start communication */
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  g_assert_cmpint(fd, >=, 0);
  saddr.sun_family = AF_UNIX;
  snprintf(saddr.sun_path, sizeof(saddr.sun_path), "/dev/socket/chatter");
  if (connect(fd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
      perror("t_testbed_script_replay_socket_stream() connect");
      abort();
  }

  /* should get initial greeting after 200 ms */
  ASSERT_EOF;
  /* some slow architectures (HPPA) often don't manage in 200 ms */
  t_testbed_read_buf_delay(350000, fd, buf, 5);
  g_assert(strncmp(buf, "ready", 5) == 0);
  g_assert_cmpint(errno, ==, 0);

  /* do the two write blocks in two matching calls; check that nothing is
   * readable before */
  ASSERT_EOF;
  g_assert_cmpint(write(fd, "abc", 3), ==, 3);
  ASSERT_EOF;
  g_assert_cmpint(write(fd, "\0defgh", 6), ==, 6);

  /* now we should get the response after 10 ms */
  ASSERT_EOF;
  t_testbed_read_buf_delay(15000, fd, buf, 9);
  g_assert_cmpint(buf[0], ==, 0);
  g_assert(strncmp(buf + 1, "response", 8) == 0);
  g_assert_cmpint(errno, ==, 0);
  ASSERT_EOF;

  close(fd);
  g_unlink (tmppath);
}

static void
t_testbed_script_replay_fuzz(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  char buf[1024];

  static const char* test_script = "f 20 -\n\
w 0 aaaaaaaaaa\n\
w 0 bbbbbbbbbb\n\
r 0 OK\n";

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/fuzzy\nN: fuzzy\n"
          "E: DEVNAME=/dev/fuzzy\nE: SUBSYSTEM=tty\nA: dev=4:64\n", &error);
  g_assert_no_error(error);

  /* load it */
  success = umockdev_testbed_load_script_from_string(fixture->testbed, "/dev/fuzzy", test_script, &error);
  g_assert_no_error(error);
  g_assert(success);

  /* start communication */
  int fd = g_open("/dev/fuzzy", O_RDWR | O_NONBLOCK, 0);
  g_assert_cmpint(fd, >=, 0);
  errno = 0;

  /* one wrong character (10%) */
  g_assert_cmpint(write(fd, "axaaaaaaaa", 10), ==, 10);
  /* two wrong characters (20%), in two blocks */
  g_assert_cmpint(write(fd, "b1b", 3), ==, 3);
  g_assert_cmpint(write(fd, "bbbbb7b", 7), ==, 7);

  /* wait for final OK to make sure it survived */
  t_testbed_read_buf_delay(10000, fd, buf, 2);
  g_assert(strncmp(buf, "OK", 2) == 0);
  g_assert_cmpint(errno, ==, 0);

  /* end of script */
  ASSERT_EOF;

  close(fd);
}

/* assert that difference between first and second is "ms" milliseconds, ±50 ms */
static void
assert_delta_t(const struct timeval * first, const struct timeval * second, int ms)
{
    int delta = (second->tv_sec - first->tv_sec) * 1000;
    delta += (second->tv_usec - first->tv_usec) / 1000;
    g_assert_cmpint(delta, >=, ms - 50 * slow_testbed_factor);
    g_assert_cmpint(delta, <=, ms + 50 * slow_testbed_factor);
}

static void
t_testbed_replay_evemu_events(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  struct timeval tv_begin, tv_end;
  struct input_event ev;
  static const char* test_data = "E: 1234.500000 0000 0000 0\n"   /* SYN */
                                 "E: 1234.650000 0002 0000 5  # comment\n"   /* REL X */
                                 "# some comment\n"
                                 /* slightly go back in time here */
                                 "E: 1234.649000 0002 0001 -11	# comment #2\n" /* REL Y */
                                 "E: 1235.200000 0001 0174 1\n";  /* KEY_ZOOM */

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/event1\nN: input/event1\n"
          "E: DEVNAME=/dev/input/event1\nE: SUBSYSTEM=input\n", &error);
  g_assert_no_error(error);

  /* load it */
  success = umockdev_testbed_load_evemu_events_from_string(fixture->testbed, "/dev/input/event1", test_data, &error);
  g_assert_no_error(error);
  g_assert(success);

  /* start communication */
  int fd = g_open("/dev/input/event1", O_RDONLY, 0);
  g_assert_cmpint(fd, >=, 0);

  g_assert_cmpint(gettimeofday(&tv_begin, NULL), ==, 0);

  /* read SYN event; that should happen immediately */
  g_assert_cmpint(read(fd, &ev, sizeof(ev)), ==, sizeof(ev));
  g_assert_cmpint(gettimeofday(&tv_end, NULL), ==, 0);
  assert_delta_t(&tv_begin, &tv_end, 10);
  tv_begin = tv_end;
  g_assert_cmpint(ev.input_event_sec, ==, 1234);
  g_assert_cmpint(ev.input_event_usec, ==, 500000);
  g_assert_cmpint(ev.type, ==, 0);
  g_assert_cmpint(ev.code, ==, 0);
  g_assert_cmpint(ev.value, ==, 0);

  /* read RELX event, should happen after 150 ms */
  g_assert_cmpint(read(fd, &ev, sizeof(ev)), ==, sizeof(ev));
  g_assert_cmpint(gettimeofday(&tv_end, NULL), ==, 0);
  g_assert_cmpint(ev.input_event_sec, ==, 1234);
  g_assert_cmpint(ev.input_event_usec, ==, 650000);
  g_assert_cmpint(ev.type, ==, 2);
  g_assert_cmpint(ev.code, ==, 0);
  g_assert_cmpint(ev.value, ==, 5);
  assert_delta_t(&tv_begin, &tv_end, 150);
  tv_begin = tv_end;

  /* read RELY event, should happen immediately after */
  g_assert_cmpint(read(fd, &ev, sizeof(ev)), ==, sizeof(ev));
  g_assert_cmpint(gettimeofday(&tv_end, NULL), ==, 0);
  g_assert_cmpint(ev.input_event_sec, ==, 1234);
  g_assert_cmpint(ev.input_event_usec, ==, 649000);
  g_assert_cmpint(ev.type, ==, 2);
  g_assert_cmpint(ev.code, ==, 1);
  g_assert_cmpint(ev.value, ==, -11);
  assert_delta_t(&tv_begin, &tv_end, 0);
  tv_begin = tv_end;

  /* read KEY_ZOOM event, should happen after 550 ms */
  g_assert_cmpint(read(fd, &ev, sizeof(ev)), ==, sizeof(ev));
  g_assert_cmpint(gettimeofday(&tv_end, NULL), ==, 0);
  g_assert_cmpint(ev.input_event_sec, ==, 1235);
  g_assert_cmpint(ev.input_event_usec, ==, 200000);
  g_assert_cmpint(ev.type, ==, 1);
  g_assert_cmpint(ev.code, ==, 0x174);
  g_assert_cmpint(ev.value, ==, 1);
  assert_delta_t(&tv_begin, &tv_end, 550);

  /* "flush" the script by waiting for it */
  success = umockdev_testbed_wait_script(fixture->testbed, "/dev/input/event1", &error);
  g_assert(success);
  g_assert_no_error(error);

  /* can load a new script now */
  success = umockdev_testbed_load_evemu_events_from_string(fixture->testbed, "/dev/input/event1",
                                                           "E: 1239.200000 0001 0123 1\n", &error);
  g_assert_no_error(error);
  g_assert(success);

  g_assert_cmpint(read(fd, &ev, sizeof(ev)), ==, sizeof(ev));
  g_assert_cmpint(ev.input_event_sec, ==, 1239);
  g_assert_cmpint(ev.input_event_usec, ==, 200000);
  g_assert_cmpint(ev.type, ==, 1);
  g_assert_cmpint(ev.code, ==, 0x123);
  g_assert_cmpint(ev.value, ==, 1);

  close(fd);
}

static void
t_testbed_replay_evemu_events_default_device(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
  gboolean success;
  GError *error = NULL;
  int fd;
  g_autofree char *tmppath = NULL;
  struct input_event ev;
  static const char* test_data = "# EVEMU 1.2\n"
                                 "# device /dev/input/event1\n"
                                 "# blabla\n"
                                 "E: 1234.500000 0000 0000 0\n";  /* SYN */

  umockdev_testbed_add_from_string(fixture->testbed,
          "P: /devices/event1\nN: input/event1\n"
          "E: DEVNAME=/dev/input/event1\nE: SUBSYSTEM=input\n", &error);
  g_assert_no_error(error);

  /* write evemu events file */
  fd = g_file_open_tmp("test_evemu.XXXXXX", &tmppath, &error);
  g_assert_no_error(error);
  g_assert_cmpint(write(fd, test_data, strlen(test_data)), ==, strlen(test_data));
  close(fd);

  /* load it */
  success = umockdev_testbed_load_evemu_events(fixture->testbed, NULL, tmppath, &error);
  g_assert_no_error(error);
  g_assert(success);
  g_unlink (tmppath);

  /* start communication */
  fd = g_open("/dev/input/event1", O_RDONLY, 0);
  g_assert_cmpint(fd, >=, 0);

  /* read SYN event; that should happen immediately */
  g_assert_cmpint(read(fd, &ev, sizeof(ev)), ==, sizeof(ev));
  g_assert_cmpint(ev.input_event_sec, ==, 1234);
  g_assert_cmpint(ev.input_event_usec, ==, 500000);
  g_assert_cmpint(ev.type, ==, 0);
  g_assert_cmpint(ev.code, ==, 0);
  g_assert_cmpint(ev.value, ==, 0);

  close(fd);
}

static void
t_testbed_clear(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GError *error = NULL;
    gchar *dev_path, *sysdev_path;

    /* create a /dev and a /sys entry */
    umockdev_testbed_add_from_string(fixture->testbed,
				     "P: /devices/moo\n"
				     "N: bus/usb/moo=00FF00\n"
				     "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/bus/usb/moo\n", &error);
    g_assert_no_error(error);

    g_assert(g_file_test(fixture->sys_dir, G_FILE_TEST_EXISTS));
    dev_path = g_build_filename(fixture->root_dir, "dev", NULL);
    sysdev_path = g_build_filename(fixture->sys_dir, "devices", NULL);
    g_assert(g_file_test(dev_path, G_FILE_TEST_EXISTS));
    g_assert(g_file_test(sysdev_path, G_FILE_TEST_EXISTS));

    /* clear */
    umockdev_testbed_clear(fixture->testbed);

    /* we only want to keep <root>/sys, but nothing else */
    g_assert(g_file_test(fixture->sys_dir, G_FILE_TEST_EXISTS));
    g_assert(!g_file_test(dev_path, G_FILE_TEST_EXISTS));
    g_assert(!g_file_test(sysdev_path, G_FILE_TEST_EXISTS));

    g_free(dev_path);
    g_free(sysdev_path);
}

static void
t_testbed_disable(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    if (!has_real_sysfs) {
	g_printf("SKIP: no real /sys on this system. ");
	return;
    }

    g_free (umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", NULL, NULL, NULL));

    /* only our test device */
    g_assert_cmpuint(num_udev_devices(), ==, 1);

    /* disable testbed */
    umockdev_testbed_disable(fixture->testbed);
    /* we should now have some real devices */
    g_assert_cmpuint(num_udev_devices(), >, 1);

    /* disable() is idempotent */
    umockdev_testbed_disable(fixture->testbed);
    g_assert_cmpuint(num_udev_devices(), >, 1);

    /* turn it back on */
    umockdev_testbed_enable(fixture->testbed);
    g_assert_cmpuint(num_udev_devices(), ==, 1);

    /* enable() is idempotent */
    umockdev_testbed_enable(fixture->testbed);
    g_assert_cmpuint(num_udev_devices(), ==, 1);
}

static gboolean
file_in_testbed(UMockdevTestbedFixture * fixture, const char *path)
{
    gboolean res;
    char *p = g_build_filename(fixture->root_dir,
                               path, NULL);
    res = g_file_test(p, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK);
    g_free(p);
    return res;
}

static void
t_testbed_remove(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    GError *error = NULL;

    /* create two /dev and /sys entries */
    umockdev_testbed_add_from_string(fixture->testbed,
				     "P: /devices/moo\n"
				     "N: bus/usb/moo\n"
				     "E: SUBSYSTEM=tty\nE: DEVNAME=/dev/bus/usb/moo\n"
                                     "A: dev=1:2\n", &error);
    g_assert_no_error(error);

    umockdev_testbed_add_from_string(fixture->testbed,
				     "P: /devices/precious\n"
				     "N: bus/tty/precious\n"
				     "E: SUBSYSTEM=tty\nE: DEVNAME=/dev/bus/tty/precious\n"
                                     "A: dev=1:3\n", &error);
    g_assert_no_error(error);

    /* static device node */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/static\nN: static=00FF614100\n"
					      "E: SUBSYSTEM=usb\nE: DEVNAME=/dev/static\n"
					      "A: dev=189:2\n", &error));
    g_assert_no_error(error);

    /* simplest possible device */
    g_autofree gchar *syspath = umockdev_testbed_add_device(fixture->testbed, "pci", "simple", NULL, NULL, NULL);
    g_assert(syspath);

    /* remove moo */
    umockdev_testbed_remove_device(fixture->testbed, "/sys/devices/moo");

    /* moo is gone */
    g_assert(!file_in_testbed(fixture, "sys/devices/moo"));
    g_assert(!file_in_testbed(fixture, "sys/dev/char/1:2"));
    g_assert(!file_in_testbed(fixture, "sys/class/tty/moo"));
    g_assert(!file_in_testbed(fixture, "dev/bus/usb/moo"));
    g_assert(!file_in_testbed(fixture, "dev/.node/bus_usb_moo"));

    /* but precious still exists */
    g_assert(file_in_testbed(fixture, "sys/devices/precious"));
    g_assert(file_in_testbed(fixture, "sys/dev/char/1:3"));
    g_assert(file_in_testbed(fixture, "sys/class/tty/precious"));
    g_assert(file_in_testbed(fixture, "dev/bus/tty/precious"));
    g_assert(file_in_testbed(fixture, "dev/.node/bus_tty_precious"));

    /* now remove precious as well; this should also clean parent dirs */
    umockdev_testbed_remove_device(fixture->testbed, "/sys/devices/precious");
    g_assert(!file_in_testbed(fixture, "sys/devices/precious"));
    g_assert(!file_in_testbed(fixture, "sys/dev/char/1:2"));
    g_assert(!file_in_testbed(fixture, "sys/class/tty"));
    g_assert(!file_in_testbed(fixture, "dev/bus/tty"));

    /* remove the other two, should not cause errors */
    umockdev_testbed_remove_device(fixture->testbed, "/sys/devices/static");
    g_assert(!file_in_testbed(fixture, "dev/static"));
    umockdev_testbed_remove_device(fixture->testbed, "/sys/devices/simple");

    /* should clean parent dirs */
    g_assert(!file_in_testbed(fixture, "dev/bus/tty"));
    g_assert(!file_in_testbed(fixture, "dev/bus/usb"));
    g_assert(!file_in_testbed(fixture, "sys/class/usb"));
    g_assert(!file_in_testbed(fixture, "sys/class/tty"));
    g_assert(!file_in_testbed(fixture, "sys/bus/usb"));
}

static void
t_testbed_proc(UMockdevTestbedFixture * fixture, UNUSED_DATA)
{
    gchar *contents;
    gchar *procdir, *path;
    const gchar *f;
    GDir *dir;
    gboolean found;

    /* should be the real file */
    g_assert(g_file_get_contents("/proc/meminfo", &contents, NULL, NULL));
    if (!strstr(contents, "MemTotal:")) {
        g_printerr("'MemTotal:' not found in /proc/meminfo: ----------\n%s\n----------", contents);
        g_test_fail();
    }
    g_free(contents);

    /* file not existing in real /proc */
    g_assert(!g_file_test("/proc/frobber", G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_open("/proc/frobber", O_RDONLY), <, 0);
    g_assert_cmpint(errno, ==, ENOENT);

    /* should be the real dir */
    dir = g_dir_open("/proc/1", 0, NULL);
    g_assert(dir != NULL);
    found = FALSE;
    while ((f = g_dir_read_name(dir)) != NULL) {
	if (strcmp(f, "exe") == 0) {
	    found = TRUE;
	    break;
	}
    }
    g_dir_close(dir);
    g_assert(found);

    /* mock existing /proc/cpuinfo */
    procdir = g_build_filename(fixture->root_dir, "proc", NULL);
    g_assert(g_mkdir(procdir, 0755) == 0);
    path = g_build_filename (procdir, "cpuinfo", NULL);
    g_assert(g_file_set_contents(path, "Ze Calculator", -1, NULL));
    g_free(path);

    g_assert(g_file_get_contents("/proc/cpuinfo", &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "Ze Calculator");
    g_free(contents);

    /* mock new file */
    path = g_build_filename (procdir, "frobber", NULL);
    g_assert(g_file_set_contents(path, "42stuff", -1, NULL));
    g_free(path);

    g_assert(g_file_test("/proc/frobber", G_FILE_TEST_IS_REGULAR));
    g_assert(g_file_get_contents("/proc/frobber", &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "42stuff");
    g_free(contents);

    /* mock file in directory */
    path = g_build_filename (procdir, "1", NULL);
    g_mkdir(path, 0755);
    g_free(path);
    path = g_build_filename (procdir, "1", "canary", NULL);
    g_assert(g_file_set_contents(path, "1", -1, NULL));
    g_free(path);

    /* we don't magically merge dirs, so readdir() is just our mock */
    dir = g_dir_open("/proc/1", 0, NULL);
    g_assert(dir != NULL);
    g_assert_cmpstr(g_dir_read_name(dir), ==, "canary");
    g_assert_cmpstr(g_dir_read_name(dir), ==, NULL);
    g_dir_close(dir);

    /* but we can see through the mock with full paths */
    g_assert(g_file_test("/proc/1/exe", G_FILE_TEST_IS_SYMLINK));

    g_free(procdir);
}

int
main(int argc, char **argv)
{
    const gchar *f = g_getenv("SLOW_TESTBED_FACTOR");

    if (f != NULL && atoi(f) > 0)
            slow_testbed_factor = atoi(f);

#if !defined(GLIB_VERSION_2_36)
    g_type_init();
#endif
    g_test_init(&argc, &argv, NULL);

    /* do we have a real /sys on this test? */
    has_real_sysfs = g_file_test("/sys/devices", G_FILE_TEST_IS_DIR);

    /* tests for mocking /sys */
    g_test_add("/umockdev-testbed/empty", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_empty, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_devicev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_devicev, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_device, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_block_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_block_device, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_device_errors", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_device_errors, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/child_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_child_device, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/set_attribute", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_set_attribute, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/set_property", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_set_property, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_from_string", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_from_string, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_from_string_errors",
	       UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_from_string_errors, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_from_file", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_from_file, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/libc", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_libc, t_testbed_fixture_teardown);

    /* tests for mocking uevents */
    g_test_add("/umockdev-testbed/uevent/libudev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_libudev, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/libudev-filter", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_libudev_filter, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/gudev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_gudev, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/no_listener", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_no_listener, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/error", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_error, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/null_action", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_null_action, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/action_overflow", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_action_overflow, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/property_overflow", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_property_overflow, t_testbed_fixture_teardown);

    /* tests for mocking USB devices */
    g_test_add("/umockdev-testbed-usb/lsusb", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_usb_lsusb, t_testbed_fixture_teardown);

    /* tests for mocking /dev */
    g_test_add("/umockdev-testbed/dev_access", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_dev_access, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_from_string_dev_char", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_from_string_dev_char, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_from_string_dev_block", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_from_string_dev_block, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/dev_query_gudev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_dev_query_gudev, t_testbed_fixture_teardown);

    /* tests for script replay */
    g_test_add("/umockdev-testbed/script_replay_simple", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_script_replay_simple, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/script_replay_default_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_script_replay_default_device, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/script_replay_override_default_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_script_replay_override_default_device, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/script_replay_wait", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_script_replay_wait, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/script_replay_evdev_event_framing", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
               t_testbed_script_replay_evdev_event_framing, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/script_replay_socket_stream", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_script_replay_socket_stream, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/script_replay_fuzz", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_script_replay_fuzz, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/replay_evemu_events", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_replay_evemu_events, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/replay_evemu_events_default_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_replay_evemu_events_default_device, t_testbed_fixture_teardown);

    /* misc */
    g_test_add("/umockdev-testbed/clear", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_clear, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/disable", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_disable, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/remove", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_remove, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/proc", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_proc, t_testbed_fixture_teardown);
    return g_test_run();
}
