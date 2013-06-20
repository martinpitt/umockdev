/*
 * test-umockdev
 *
 * Copyright (C) 2012 Canonical Ltd.
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
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#include <libudev.h>
#include <gudev/gudev.h>

#include "umockdev.h"

static gboolean has_real_sysfs;

typedef struct {
    UMockdevTestbed *testbed;
} UMockdevTestbedFixture;

static void
t_testbed_fixture_setup(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    fixture->testbed = umockdev_testbed_new();
    g_assert(fixture->testbed != NULL);
}

static void
t_testbed_fixture_teardown(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    gchar *rootdir;
    rootdir = g_strdup(umockdev_testbed_get_root_dir(fixture->testbed));
    g_object_unref(fixture->testbed);

    /* verify that temp dir gets cleaned up properly */
    g_assert(!g_file_test(rootdir, G_FILE_TEST_EXISTS));

    g_free(rootdir);
}

/* Return number of devices that libudev can see */
static guint
num_udev_devices(void)
{
    GUdevClient *client;
    GUdevEnumerator *enumerator;
    GList *result;
    guint num;

    client = g_udev_client_new(NULL);
    g_assert(client);

    enumerator = g_udev_enumerator_new(client);
    g_assert(enumerator);
    result = g_udev_enumerator_execute(enumerator);
    num = g_list_length(result);

    g_list_free_full(result, g_object_unref);
    g_object_unref(enumerator);
    g_object_unref(client);

    return num;
}

/* Empty UMockdevTestbed without any devices */
static void
t_testbed_empty(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    g_assert_cmpuint(num_udev_devices(), ==, 0);
}

/* common checks for umockdev_testbed_add_device{,v}() */
static void
_t_testbed_check_extkeyboard1(const gchar * syspath)
{
    GUdevClient *client;
    GUdevEnumerator *enumerator;
    GList *result;
    GUdevDevice *device;
    client = g_udev_client_new(NULL);
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
    g_object_unref(client);
}

/* UMockdevTestbed add_devicev() with adding one device */
static void
t_testbed_add_devicev(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    gchar *syspath;
    gchar *attributes[] = { "idVendor", "0815", "idProduct", "AFFE", NULL };
    gchar *properties[] = { "ID_INPUT", "1", "ID_INPUT_KEYBOARD", "1", NULL };

    syspath = umockdev_testbed_add_devicev(fixture->testbed, "usb", "extkeyboard1", NULL, attributes, properties);
    g_assert_cmpstr(syspath, ==, "/sys/devices/extkeyboard1");

    _t_testbed_check_extkeyboard1(syspath);
    g_free(syspath);
}

/* UMockdevTestbed add_device() with adding one device */
static void
t_testbed_add_device(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    gchar *syspath;

    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "extkeyboard1", NULL,
					  /* attributes */
					  "idVendor", "0815", "idProduct", "AFFE", NULL,
					  /* properties */
					  "ID_INPUT", "1", "ID_INPUT_KEYBOARD", "1", NULL);
    g_assert(syspath);
    g_assert(g_str_has_suffix(syspath, "/sys/devices/extkeyboard1"));

    _t_testbed_check_extkeyboard1(syspath);
    g_free(syspath);
}

/* UMockdevTestbed add_device() with adding a child device */
static void
t_testbed_child_device(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    gchar *dev, *iface, *input;
    GUdevClient *client;
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

    client = g_udev_client_new(NULL);

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
    path = g_build_filename(dev, "subsystem", "usb1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(dev, "subsystem", "usb1", "idVendor", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);

    /* dev's bus symlinks */
    path = g_build_filename(umockdev_testbed_get_sys_dir(fixture->testbed), "bus", "usb", "devices", "usb1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(umockdev_testbed_get_sys_dir(fixture->testbed),
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
    path = g_build_filename(iface, "subsystem", "1-1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(iface, "subsystem", "1-1", "iClass", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_REGULAR));
    g_free(path);

    /* iface's bus symlinks */
    path = g_build_filename(umockdev_testbed_get_sys_dir(fixture->testbed), "bus", "usb", "devices", "1-1", NULL);
    g_assert(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
    g_free(path);
    path = g_build_filename(umockdev_testbed_get_sys_dir(fixture->testbed),
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

    g_object_unref(client);
    g_free(dev);
    g_free(iface);
    g_free(input);
}

struct TestbedErrorCatcherData {
    unsigned counter;
    GLogLevelFlags last_level;
    gchar *last_message;
};

static gboolean
t_testbed_error_catcher(const gchar * log_domain, GLogLevelFlags log_level, const gchar * message, gpointer user_data)
{
    struct TestbedErrorCatcherData *data = (struct TestbedErrorCatcherData *)user_data;

    data->counter++;
    data->last_level = log_level;
    if (data->last_message)
	g_free(data->last_message);
    data->last_message = g_strdup(message);
    return FALSE;
}

static void
ignore_log_handler (const gchar *log_domain, GLogLevelFlags log_level,
                    const gchar *message, gpointer user_data)
{
}

/* UMockdevTestbed add_device() error conditions */
static void
t_testbed_add_device_errors(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    gchar *syspath;
    struct TestbedErrorCatcherData errors = { 0, 0, NULL };
    guint log_handler;

    g_test_log_set_fatal_handler(t_testbed_error_catcher, &errors);

    /* invalid parent */
    log_handler = g_log_set_handler(NULL, G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_FATAL, ignore_log_handler, NULL);
    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", "/sys/nosuchdevice", NULL, NULL);
    g_log_remove_handler (NULL, log_handler);
    g_assert(syspath == NULL);
    g_assert_cmpint(errors.counter, ==, 1);
    g_assert_cmpint(errors.last_level, ==, G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_FATAL);
    g_assert(strstr(errors.last_message, "/sys/nosuchdevice") != NULL);

    /* key/values do not pair up */
    log_handler = g_log_set_handler(NULL, G_LOG_LEVEL_WARNING | G_LOG_FLAG_FATAL, ignore_log_handler, NULL);
    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", NULL,
					  /* attributes */
					  "idVendor", "0815", "idProduct", NULL, NULL);
    g_log_remove_handler (NULL, log_handler);
    g_assert(syspath);
    g_assert_cmpint(errors.counter, ==, 2);
    g_assert_cmpint(errors.last_level & G_LOG_LEVEL_WARNING, !=, 0);
    g_assert(strstr(errors.last_message, "idProduct") != NULL);

}

static void
t_testbed_set_attribute(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    GUdevClient *client;
    GUdevDevice *device;
    gchar *syspath;
    gchar *attrpath;
    gchar *contents = NULL;
    gsize length;

    client = g_udev_client_new(NULL);

    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "extkeyboard1", NULL,
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
    /* int attributes */
    umockdev_testbed_set_attribute_int(fixture->testbed, syspath, "count", 1000);
    umockdev_testbed_set_attribute_hex(fixture->testbed, syspath, "addr", 0x1a01);

    device = g_udev_client_query_by_sysfs_path(client, syspath);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idProduct"), ==, "BEEF");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "color"), ==, "yellow");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "count"), ==, "1000");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "addr"), ==, "1a01");
    g_object_unref(device);

    g_object_unref(client);

    /* validate binary attribute */
    attrpath = g_build_filename(syspath, "descriptor", NULL);
    g_assert(g_file_get_contents(attrpath, &contents, &length, NULL));
    g_assert_cmpint(length, ==, 7);
    g_assert_cmpint(memcmp(contents, "\x01\x00\xFF\x00\x05\x40\xA0", 7), ==, 0);
    g_free(contents);

    g_free(syspath);
}

static void
t_testbed_set_property(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    GUdevClient *client;
    GUdevDevice *device;
    gchar *syspath;

    client = g_udev_client_new(NULL);

    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "extkeyboard1", NULL,
					  /* attributes */
					  NULL,
					  /* properties */
					  "ID_INPUT", "1", NULL);

    /* change an existing property */
    umockdev_testbed_set_property(fixture->testbed, syspath, "ID_INPUT", "0");
    /* add a new one */
    umockdev_testbed_set_property(fixture->testbed, syspath, "ID_COLOR", "green");
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

    g_object_unref(client);
    g_free(syspath);
}

struct event_counter {
    unsigned add;
    unsigned remove;
    unsigned change;
    gchar last_device[1024];
};

static void
on_uevent(GUdevClient * client, const gchar * action, GUdevDevice * device, gpointer user_data)
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
t_testbed_uevent_libudev(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    gchar *syspath;
    struct udev *udev;
    struct udev_monitor *udev_mon, *kernel_mon;
    struct udev_device *device;

    syspath = umockdev_testbed_add_device(fixture->testbed, "pci", "mydev", NULL,
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
    udev_device_unref(device);

    device = udev_monitor_receive_device(kernel_mon);
    g_assert(device != NULL);
    g_assert_cmpstr(udev_device_get_syspath(device), ==, syspath);
    g_assert_cmpstr(udev_device_get_action(device), ==, "add");
    udev_device_unref(device);

    udev_monitor_unref(udev_mon);
    udev_monitor_unref(kernel_mon);
    udev_unref(udev);
}

static void
t_testbed_uevent_gudev(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    GUdevClient *client;
    GUdevDevice *device;
    gchar *syspath;
    GMainLoop *mainloop;
    struct event_counter counter = { 0, 0, 0 };
    const gchar *subsystems[] = { "pci", NULL };

    syspath = umockdev_testbed_add_device(fixture->testbed, "pci", "mydev", NULL,
					  /* attributes */
					  "idVendor", "0815", NULL,
					  /* properties */
					  "ID_INPUT", "1", NULL);
    g_assert(syspath);

    /* set up listener for uevent signal */
    client = g_udev_client_new(subsystems);
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
    device = g_udev_client_query_by_sysfs_path(client, syspath);
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "idVendor"), ==, "0815");
    g_assert_cmpstr(g_udev_device_get_property(device, "ID_INPUT"), ==, "1");

    g_object_unref(client);
    g_free(syspath);
}

static void
t_testbed_add_from_string(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    GUdevClient *client;
    GUdevDevice *device, *subdev;
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
					       "A: simple_attr=1\n", &error);
    g_assert_no_error(error);
    g_assert(success);

    client = g_udev_client_new(NULL);

    /* should have exactly one device */
    g_assert_cmpuint(num_udev_devices(), ==, 1);

    /* check properties and attributes */
    device = g_udev_client_query_by_sysfs_path(client, "/sys/devices/dev1");
    g_assert(device);
    g_assert_cmpstr(g_udev_device_get_subsystem(device), ==, "pci");
    g_assert(g_udev_device_get_parent(device) == NULL);
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "simple_attr"), ==, "1");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(device, "multiline_attr"), ==, "a\\b\nc\\d\nlast");
    g_assert_cmpstr(g_udev_device_get_property(device, "SIMPLE_PROP"), ==, "1");
    g_object_unref(device);

    g_assert(g_file_get_contents("/sys/devices/dev1/binary_attr", &contents, &length, NULL));
    g_assert_cmpint(length, ==, 7);
    g_assert_cmpint(memcmp(contents, "\x41\xA9\xFF\x00\x05\xFF\x00", 7), ==, 0);
    g_free(contents);

    /* class symlink created */
    g_assert(g_file_test("/sys/devices/dev1/subsystem", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev1/subsystem/dev1", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev1/subsystem/dev1/simple_attr", G_FILE_TEST_IS_REGULAR));

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
    g_assert(g_file_test("/sys/devices/dev2/subsystem/dev2", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev2/subsystem/dev2/dev2color", G_FILE_TEST_IS_REGULAR));

    /* check subdev1 */
    subdev = g_udev_client_query_by_sysfs_path(client, "/sys/devices/dev2/subdev1");
    g_assert(subdev);
    g_assert_cmpstr(g_udev_device_get_sysfs_path(g_udev_device_get_parent(subdev)), ==, "/sys/devices/dev2");
    g_assert_cmpstr(g_udev_device_get_subsystem(subdev), ==, "input");
    g_assert_cmpstr(g_udev_device_get_sysfs_attr(subdev, "subdev1color"), ==, "yellow");
    g_assert_cmpstr(g_udev_device_get_property(subdev, "SUBDEV1COLOR"), ==, "YELLOW");
    g_object_unref(subdev);
    g_assert(g_file_test("/sys/devices/dev2/subdev1/subsystem", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev2/subdev1/subsystem/subdev1", G_FILE_TEST_IS_SYMLINK));
    g_assert(g_file_test("/sys/devices/dev2/subdev1/subsystem/subdev1/subdev1color", G_FILE_TEST_IS_REGULAR));

    g_object_unref(device);
    g_object_unref(client);
}

static void
t_testbed_add_from_string_errors(UMockdevTestbedFixture * fixture, gconstpointer data)
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
t_testbed_usb_lsusb(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    gchar *syspath;
    gchar *out, *err, *dir;
    int exit_status;
    GError *error = NULL;
    gchar *argv[] = { "lsusb", "-v", NULL };

    if (g_find_program_in_path("lsusb") == NULL) {
	g_printf("SKIP: lsusb not installed. ");
	return;
    }

    syspath = umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", NULL,
					  /* attributes */
					  "busnum", "1", "devnum", "1", "speed", "480",
					  "bConfigurationValue", "1", NULL,
					  /* properties */
					  "ID_GPHOTO2", "1", NULL);
    g_assert(syspath);
    /* descriptor from a Canon PowerShot SX200 */
    umockdev_testbed_set_attribute_binary(fixture->testbed, syspath, "descriptors", (guint8 *)
					  "\x12\x01\x00\x02\x00\x00\x00\x40\xa9\x04\xc0\x31\x02\x00\x01\x02"
					  "\x03\x01\x09\x02\x27\x00\x01\x01\x00\xc0\x01\x09\x04\x00\x00\x03"
					  "\x06\x01\x01\x00\x07\x05\x81\x02\x00\x02\x00\x07\x05\x02\x02\x00"
					  "\x02\x00\x07\x05\x83\x03\x08\x00\x09", 57);

    /* ensure that /dev/bus/usb/ exists, lsusb insists on it */
    dir = g_build_filename(umockdev_testbed_get_root_dir(fixture->testbed), "dev", "bus", "usb", "002", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_free (dir);

    g_assert(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &exit_status, &error));
    g_assert_no_error(error);
    g_assert_cmpint(exit_status, ==, 0);

    /* g_printf("------ out: -------\n%s\n------ err: ------\n%s\n-----\n", out, err); */
    g_assert(g_str_has_prefix(out, "\nBus 001 Device 001: ID 04a9:31c0 Canon, Inc. PowerShot SX200 IS\n"));
    g_assert(strstr(out, "idVendor           0x04a9 Canon, Inc."));
}

static void
t_testbed_dev_access(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    GStatBuf st;
    gchar *devdir, *devpath;
    int fd;
    char buf[100];

    /* no mocked devices */
    g_assert_cmpint(g_open("/dev/wishyouwerehere", O_RDONLY, 0), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    g_assert_cmpint(g_stat("/dev/zero", &st), ==, 0);
    g_assert(S_ISCHR(st.st_mode));
    fd = g_open("/dev/zero", O_RDONLY, 0);
    g_assert_cmpint(fd, >, 0);
    g_assert_cmpint(read(fd, buf, 20), ==, 20);
    close(fd);
    g_assert_cmpint(buf[0], ==, 0);
    g_assert_cmpint(buf[1], ==, 0);
    g_assert_cmpint(buf[9], ==, 0);

    /* create a mock /dev/zero */
    devdir = g_build_filename(umockdev_testbed_get_root_dir(fixture->testbed), "dev", NULL);
    devpath = g_build_filename(devdir, "zero", NULL);
    g_mkdir(devdir, 0755);
    g_assert(g_file_set_contents(devpath, "zerozerozero", -1, NULL));
    g_free(devpath);

    /* now /dev/zero should be the mocked one */
    g_assert_cmpint(g_open("/dev/wishyouwerehere", O_RDONLY, 0), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    g_assert_cmpint(g_stat("/dev/zero", &st), ==, 0);
    g_assert(S_ISREG(st.st_mode));
    fd = g_open("/dev/zero", O_RDONLY, 0);
    g_assert_cmpint(fd, >, 0);
    g_assert_cmpint(read(fd, buf, 20), ==, 12);
    close(fd);
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

    g_free(devdir);
}

static void
t_testbed_add_from_string_dev(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    GError *error = NULL;
    gchar *contents;
    gsize length;

    /* N: without value should create an empty dev */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/empty\n"
					      "N: empty\n" "E: SUBSYSTEM=foo\n" "E: DEVNAME=/dev/empty\n", &error));
    g_assert_no_error(error);

    g_assert(g_file_get_contents("/dev/empty", &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpint(length, ==, 0);
    g_assert_cmpstr(contents, ==, "");
    g_free(contents);

    /* N: another N without value whose name looks like hex */
    umockdev_testbed_add_from_string(fixture->testbed,
				     "P: /devices/001\n"
				     "N: bus/usb/001\n"
				     "E: BUSNUM=001\n" "E: SUBSYSTEM=foo\n" "E: DEVNAME=/dev/bus/usb/001\n", &error);
    g_assert_no_error(error);

    g_assert(g_file_get_contents("/dev/bus/usb/001", &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpint(length, ==, 0);
    g_assert_cmpstr(contents, ==, "");
    g_free(contents);

    /* N: with value should set that contents */
    g_assert(umockdev_testbed_add_from_string(fixture->testbed,
					      "P: /devices/preset\n"
					      "N: bus/usb/preset=00FF614100\n"
					      "E: SUBSYSTEM=foo\n" "E: DEVNAME=/dev/bus/usb/preset\n", &error));
    g_assert_no_error(error);

    g_assert(g_file_get_contents("/dev/bus/usb/preset", &contents, &length, &error));
    g_assert_no_error(error);
    g_assert_cmpint(length, ==, 5);
    g_assert_cmpint(memcmp(contents, "\x00\377aA\x00", 2), ==, 0);
    g_free(contents);
}


static void
t_testbed_clear(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    GError *error = NULL;
    gchar *dev_path, *sysdev_path;

    /* create a /dev and a /sys entry */
    umockdev_testbed_add_from_string(fixture->testbed,
				     "P: /devices/moo\n"
				     "N: bus/usb/moo=00FF00\n"
				     "E: SUBSYSTEM=foo\nE: DEVNAME=/dev/bus/usb/moo\n", &error);
    g_assert_no_error(error);

    g_assert(g_file_test(umockdev_testbed_get_sys_dir(fixture->testbed), G_FILE_TEST_EXISTS));
    dev_path = g_build_filename(umockdev_testbed_get_root_dir(fixture->testbed), "dev", NULL);
    sysdev_path = g_build_filename(umockdev_testbed_get_sys_dir(fixture->testbed), "devices", NULL);
    g_assert(g_file_test(dev_path, G_FILE_TEST_EXISTS));
    g_assert(g_file_test(sysdev_path, G_FILE_TEST_EXISTS));

    /* clear */
    umockdev_testbed_clear(fixture->testbed);

    /* we only want to keep <root>/sys, but nothing else */
    g_assert(g_file_test(umockdev_testbed_get_sys_dir(fixture->testbed), G_FILE_TEST_EXISTS));
    g_assert(!g_file_test(dev_path, G_FILE_TEST_EXISTS));
    g_assert(!g_file_test(sysdev_path, G_FILE_TEST_EXISTS));

    g_free(dev_path);
    g_free(sysdev_path);
}

static void
t_testbed_disable(UMockdevTestbedFixture * fixture, gconstpointer data)
{
    if (!has_real_sysfs) {
	g_printf("SKIP: no real /sys on this system. ");
	return;
    }

    umockdev_testbed_add_device(fixture->testbed, "usb", "usb1", NULL, NULL, NULL);

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

int
main(int argc, char **argv)
{
#if !defined(GLIB_VERSION_2_36)
    g_type_init();
#endif
    g_test_init(&argc, &argv, NULL);

    /* do we have a real /sys on this test? */
    has_real_sysfs = g_file_test ("/sys/devices", G_FILE_TEST_IS_DIR);

    /* tests for mocking /sys */
    g_test_add("/umockdev-testbed/empty", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_empty, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_devicev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_devicev, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_device, t_testbed_fixture_teardown);
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

    /* tests for mocking uevents */
    g_test_add("/umockdev-testbed/uevent/libudev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_libudev, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/uevent/gudev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_uevent_gudev, t_testbed_fixture_teardown);

    /* tests for mocking USB devices */
    g_test_add("/umockdev-testbed-usb/lsusb", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_usb_lsusb, t_testbed_fixture_teardown);

    /* tests for mocking /dev */
    g_test_add("/umockdev-testbed/dev_access", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_dev_access, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/add_from_string_dev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_add_from_string_dev, t_testbed_fixture_teardown);

    /* misc */
    g_test_add("/umockdev-testbed/clear", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_clear, t_testbed_fixture_teardown);
    g_test_add("/umockdev-testbed/disable", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
	       t_testbed_disable, t_testbed_fixture_teardown);
    return g_test_run();
}
