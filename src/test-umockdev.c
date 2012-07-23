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
#include <string.h>

#include <gudev/gudev.h>

#include "umockdev.h"

typedef struct {
    UMockdevTestbed *testbed;
} UMockdevTestbedFixture;

static void
t_testbed_fixture_setup (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  fixture->testbed = umockdev_testbed_new();
  g_assert (fixture->testbed != NULL);
}

static void
t_testbed_fixture_teardown (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  g_object_unref (fixture->testbed);
}

/* Empty UMockdevTestbed without any devices */
static void
t_testbed_empty (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  GUdevClient *client;
  GUdevEnumerator *enumerator;
  GList *result;

  client = g_udev_client_new (NULL);
  g_assert (client);

  enumerator = g_udev_enumerator_new (client);
  g_assert (enumerator);
  result = g_udev_enumerator_execute (enumerator);
  g_assert_cmpuint (g_list_length (result), ==, 0);

  g_object_unref (enumerator);
  g_object_unref (client);
}

/* common checks for umockdev_testbed_add_device{,v}() */
static void
_t_testbed_check_extkeyboard1 (const gchar* syspath)
{
  GUdevClient *client;
  GUdevEnumerator *enumerator;
  GList *result;
  GUdevDevice *device;
  client = g_udev_client_new (NULL);
  g_assert (client);

  enumerator = g_udev_enumerator_new (client);
  g_assert (enumerator);
  result = g_udev_enumerator_execute (enumerator);
  g_assert_cmpuint (g_list_length (result), ==, 1);

  /* check that the entry matches what we put into our test bed */
  device = G_UDEV_DEVICE (result->data);
  g_assert (device);
  g_assert_cmpstr (g_udev_device_get_name (device), ==, "extkeyboard1");
  g_assert_cmpstr (g_udev_device_get_sysfs_path (device), ==, syspath);

  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "idVendor"), ==, "0815");
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "idProduct"), ==, "AFFE");
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "noSuchAttr"), ==, NULL);

  g_assert_cmpstr (g_udev_device_get_property (device, "DEVPATH"), ==, "/devices/extkeyboard1");
  g_assert_cmpstr (g_udev_device_get_property (device, "SUBSYSTEM"), ==, "usb");
  g_assert_cmpstr (g_udev_device_get_property (device, "ID_INPUT"), ==, "1");
  g_assert_cmpstr (g_udev_device_get_property (device, "ID_INPUT_KEYBOARD"), ==, "1");
  g_assert_cmpstr (g_udev_device_get_property (device, "NO_SUCH_PROP"), ==, NULL);

  g_list_free_full (result, g_object_unref);
  g_object_unref (enumerator);
  g_object_unref (client);
}

/* UMockdevTestbed add_devicev() with adding one device */
static void
t_testbed_add_devicev (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  gchar *syspath;
  const gchar *attributes[] = { "idVendor", "0815", "idProduct", "AFFE", NULL };
  const gchar *properties[] = { "ID_INPUT", "1", "ID_INPUT_KEYBOARD", "1", NULL };

  syspath = umockdev_testbed_add_devicev (fixture->testbed,
                                        "usb",
                                        "extkeyboard1",
                                        NULL,
                                        attributes,
                                        properties);
  g_assert (syspath);
  g_assert (g_str_has_suffix (syspath, "/sys/devices/extkeyboard1"));

  _t_testbed_check_extkeyboard1(syspath);
  g_free (syspath);
}

/* UMockdevTestbed add_device() with adding one device */
static void
t_testbed_add_device (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  gchar *syspath;

  syspath = umockdev_testbed_add_device (fixture->testbed,
                                       "usb",
                                       "extkeyboard1",
                                       NULL,
                                       /* attributes */
                                       "idVendor", "0815", "idProduct", "AFFE", NULL,
                                       /* properties */
                                       "ID_INPUT", "1", "ID_INPUT_KEYBOARD", "1", NULL);
  g_assert (syspath);
  g_assert (g_str_has_suffix (syspath, "/sys/devices/extkeyboard1"));

  _t_testbed_check_extkeyboard1(syspath);
  g_free (syspath);
}

/* UMockdevTestbed add_device() with adding a child device */
static void
t_testbed_child_device (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  gchar *parent, *child;
  GUdevClient *client;
  GUdevDevice *device, *device2;

  parent = umockdev_testbed_add_device (fixture->testbed,
                                       "usb",
                                       "usb1",
                                       NULL,
                                       /* attributes */
                                       "idVendor", "0815", "idProduct", "AFFE", NULL,
                                       /* properties */
                                       "INTERFACE", "3/1/1", NULL);
  g_assert (parent);
  g_assert_cmpstr (parent, ==, "/sys/devices/usb1");

  child = umockdev_testbed_add_device (fixture->testbed,
                                       "input",
                                       "kb1",
                                       parent,
                                       /* attributes */
                                       "name", "HID 123", NULL,
                                       /* properties */
                                       "ID_INPUT", "1", NULL);
  g_assert (child);
  g_assert_cmpstr (child, ==, "/sys/devices/usb1/kb1");

  client = g_udev_client_new (NULL);

  /* check parent device */
  device = g_udev_client_query_by_sysfs_path (client, parent);
  g_assert (device);
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "idVendor"), ==, "0815");
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "name"), ==, NULL);
  g_assert_cmpstr (g_udev_device_get_property (device, "INTERFACE"), ==, "3/1/1");
  g_assert_cmpstr (g_udev_device_get_property (device, "ID_INPUT"), ==, NULL);
  g_assert (g_udev_device_get_parent (device) == NULL);
  g_assert_cmpstr (g_udev_device_get_subsystem (device), ==, "usb");
  g_assert_cmpstr (g_udev_device_get_name (device), ==, "usb1");
  g_object_unref (device);

  /* check child device */
  device = g_udev_client_query_by_sysfs_path (client, child);
  g_assert (device);
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "idVendor"), ==, NULL);
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "name"), ==, "HID 123");
  g_assert_cmpstr (g_udev_device_get_property (device, "INTERFACE"), ==, NULL);
  g_assert_cmpstr (g_udev_device_get_property (device, "ID_INPUT"), ==, "1");
  g_assert_cmpstr (g_udev_device_get_subsystem (device), ==, "input");
  g_assert_cmpstr (g_udev_device_get_name (device), ==, "kb1");
  device2 = g_udev_device_get_parent (device);
  g_assert (device2 != NULL);
  g_assert_cmpstr (g_udev_device_get_sysfs_path (device2), ==, parent);
  g_object_unref (device);

  g_object_unref (client);
  g_free (parent);
  g_free (child);
}

static void
t_testbed_set_attribute (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  GUdevClient *client;
  GUdevDevice *device;
  gchar *syspath;

  client = g_udev_client_new (NULL);

  syspath = umockdev_testbed_add_device (fixture->testbed,
                                       "usb",
                                       "extkeyboard1",
                                       NULL,
                                       /* attributes */
                                       "idVendor", "0815", "idProduct", "AFFE", NULL,
                                       /* properties */
                                       NULL);

  /* change an existing attribute */
  umockdev_testbed_set_attribute (fixture->testbed, syspath, "idProduct", "BEEF");
  /* add a new one */
  umockdev_testbed_set_attribute (fixture->testbed, syspath, "color", "yellow");

  device = g_udev_client_query_by_sysfs_path (client, syspath);
  g_assert (device);
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "idVendor"), ==, "0815");
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "idProduct"), ==, "BEEF");
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "color"), ==, "yellow");
  g_object_unref (device);

  g_object_unref (client);
  g_free (syspath);
}

static void
t_testbed_set_property (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  GUdevClient *client;
  GUdevDevice *device;
  gchar *syspath;

  client = g_udev_client_new (NULL);

  syspath = umockdev_testbed_add_device (fixture->testbed,
                                       "usb",
                                       "extkeyboard1",
                                       NULL,
                                       /* attributes */
                                       NULL,
                                       /* properties */
                                       "ID_INPUT", "1", NULL);

  /* change an existing property */
  umockdev_testbed_set_property (fixture->testbed, syspath, "ID_INPUT", "0");
  /* add a new one */
  umockdev_testbed_set_property (fixture->testbed, syspath, "ID_COLOR", "green");

  device = g_udev_client_query_by_sysfs_path (client, syspath);
  g_assert (device);
  g_assert_cmpstr (g_udev_device_get_property (device, "ID_INPUT"), ==, "0");
  g_assert_cmpstr (g_udev_device_get_property (device, "ID_COLOR"), ==, "green");
  g_object_unref (device);

  g_object_unref (client);
  g_free (syspath);
}

struct event_counter {
    unsigned add;
    unsigned remove;
    unsigned change;
    gchar last_device[1024];
};

static void
on_uevent (GUdevClient  *client,
           const gchar  *action,
           GUdevDevice  *device,
           gpointer      user_data)
{
  struct event_counter *counter = (struct event_counter *) user_data;

  g_debug ("on_uevent action %s device %s", action, g_udev_device_get_sysfs_path (device));

  if (strcmp (action, "add") == 0)
    counter->add++;
  else if (strcmp (action, "remove") == 0)
    counter->remove++;
  else if (strcmp (action, "change") == 0)
    counter->change++;
  else
    g_assert_not_reached ();

  strncpy (counter->last_device,
           g_udev_device_get_sysfs_path (device),
           sizeof (counter->last_device) - 1);
}

static gboolean
on_timeout (gpointer user_data)
{
  GMainLoop *mainloop = (GMainLoop *) user_data;
  g_main_loop_quit (mainloop);
  return FALSE;
}

static void
t_testbed_uevent (UMockdevTestbedFixture *fixture, gconstpointer data)
{
  GUdevClient *client;
  GUdevDevice *device;
  gchar *syspath;
  GMainLoop *mainloop;
  struct event_counter counter = {0, 0, 0};
  const gchar *subsystems[] = {"pci", NULL};

  syspath = umockdev_testbed_add_device (fixture->testbed,
                                         "pci",
                                         "mydev",
                                         NULL,
                                         /* attributes */
                                         "idVendor", "0815", NULL,
                                         /* properties */
                                         "ID_INPUT", "1", NULL);
  g_assert (syspath);

  /* set up listener for uevent signal */
  client = g_udev_client_new (subsystems);
  g_signal_connect (client, "uevent", G_CALLBACK (on_uevent), &counter);

  mainloop = g_main_loop_new (NULL, FALSE);

  /* send a signal and run main loop for 0.5 seconds to catch it */
  umockdev_testbed_uevent (fixture->testbed, syspath, "add");
  g_timeout_add (500, on_timeout, mainloop);
  g_main_loop_run (mainloop);
  g_assert_cmpuint (counter.add, ==, 1);
  g_assert_cmpuint (counter.remove, ==, 0);
  g_assert_cmpuint (counter.change, ==, 0);
  g_assert_cmpstr (counter.last_device, ==, "/sys/devices/mydev");

  counter.add = 0;
  umockdev_testbed_uevent (fixture->testbed, syspath, "change");
  g_timeout_add (500, on_timeout, mainloop);
  g_main_loop_run (mainloop);
  g_assert_cmpuint (counter.add, ==, 0);
  g_assert_cmpuint (counter.remove, ==, 0);
  g_assert_cmpuint (counter.change, ==, 1);
  g_assert_cmpstr (counter.last_device, ==, "/sys/devices/mydev");

  g_main_loop_unref (mainloop);

  /* ensure that properties and attributes are still intact */
  device = g_udev_client_query_by_sysfs_path (client, syspath);
  g_assert (device);
  g_assert_cmpstr (g_udev_device_get_sysfs_attr (device, "idVendor"), ==, "0815");
  g_assert_cmpstr (g_udev_device_get_property (device, "ID_INPUT"), ==, "1");

  g_object_unref (client);
  g_free (syspath);
}

int
main (int argc, char **argv)
{
  g_type_init ();
  g_test_init (&argc, &argv, NULL);

  g_test_add ("/umockdev-testbed/empty", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
              t_testbed_empty, t_testbed_fixture_teardown);
  g_test_add ("/umockdev-testbed/add_devicev", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
              t_testbed_add_devicev, t_testbed_fixture_teardown);
  g_test_add ("/umockdev-testbed/add_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
              t_testbed_add_device, t_testbed_fixture_teardown);
  g_test_add ("/umockdev-testbed/child_device", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
              t_testbed_child_device, t_testbed_fixture_teardown);
  g_test_add ("/umockdev-testbed/set_attribute", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
              t_testbed_set_attribute, t_testbed_fixture_teardown);
  g_test_add ("/umockdev-testbed/set_property", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
              t_testbed_set_property, t_testbed_fixture_teardown);
  g_test_add ("/umockdev-testbed/uevent", UMockdevTestbedFixture, NULL, t_testbed_fixture_setup,
              t_testbed_uevent, t_testbed_fixture_teardown);

  return g_test_run ();
}
