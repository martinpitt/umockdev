/* -*- Mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "umockdev.h"

#include "uevent_sender.h"

/**
 * UMockdevTestbed:
 *
 * The #UMockdevTestbed struct is opaque and should not be accessed directly.
 */
struct _UMockdevTestbed
{
  GObject              parent;

  /*< private >*/
  UMockdevTestbedPrivate *priv;
};

/**
 * UMockdevTestbedClass:
 *
 * Class structure for #UMockdevTestbed.
 */
struct _UMockdevTestbedClass
{
  GObjectClass   parent_class;

  /*< private >*/
  /* Padding for future expansion */
  void (*reserved1) (void);
  void (*reserved2) (void);
  void (*reserved3) (void);
  void (*reserved4) (void);
  void (*reserved5) (void);
  void (*reserved6) (void);
  void (*reserved7) (void);
  void (*reserved8) (void);
};

/**
 * SECTION:umockdev
 * @short_description: Build a test bed for testing software that handles Linux
 * hardware devices.
 *
 * The #UMockdevTestbed class builds a temporary sandbox for mock devices.
 * Right now this covers sysfs, but other aspects (uevents, /dev, ioctl, etc.)
 * will be added in the future. You can add a number of devices including
 * arbitrary sysfs attributes and udev properties, and then run your software
 * in that test bed that is independent of the actual hardware it is running
 * on.  With this you can simulate particular hardware in virtual environments
 * up to some degree, without needing any particular privileges or disturbing
 * the whole system.
 *
 * Instantiating a #UMockdevTestbed object creates a temporary directory with
 * an empty sysfs tree and sets the $UMOCKDEV_DIR environment variable so that
 * programs subsequently started under umockdev-wrapper will use the test bed
 * instead of the system's real sysfs.
 */

struct _UMockdevTestbedPrivate
{
  gchar *root_dir;
  gchar *sys_dir;
  uevent_sender *uevent_sender;
};

G_DEFINE_TYPE (UMockdevTestbed, umockdev_testbed, G_TYPE_OBJECT)

static void
umockdev_testbed_finalize (GObject *object)
{
  UMockdevTestbed *testbed = UMOCKDEV_TESTBED (object);

  /* TODO: rm -r root_dir */

  g_debug ("Removing test bed %s", testbed->priv->root_dir);
  g_unsetenv ("UMOCKDEV_DIR");

  g_free (testbed->priv->root_dir);
  g_free (testbed->priv->sys_dir);

  uevent_sender_close (testbed->priv->uevent_sender);

  if (G_OBJECT_CLASS (umockdev_testbed_parent_class)->finalize != NULL)
    (* G_OBJECT_CLASS (umockdev_testbed_parent_class)->finalize) (object);
}

static void
umockdev_testbed_class_init (UMockdevTestbedClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  gobject_class->finalize = umockdev_testbed_finalize;
  g_type_class_add_private (klass, sizeof (UMockdevTestbedPrivate));
}

static void
umockdev_testbed_init (UMockdevTestbed *testbed)
{
  GError *error = NULL;

  testbed->priv = G_TYPE_INSTANCE_GET_PRIVATE (testbed,
                                               UMOCKDEV_TYPE_TESTBED,
                                               UMockdevTestbedPrivate);

  testbed->priv->root_dir = g_dir_make_tmp ("umockdev.XXXXXX", &error);
  g_assert_no_error (error);

  testbed->priv->sys_dir = g_build_filename (testbed->priv->root_dir, "sys", NULL);
  g_assert (g_mkdir (testbed->priv->sys_dir, 0755) == 0);

  testbed->priv->uevent_sender = uevent_sender_open (testbed->priv->root_dir);
  g_assert (testbed->priv->uevent_sender != NULL);

  g_assert (g_setenv ("UMOCKDEV_DIR", testbed->priv->root_dir, TRUE));

  g_debug ("Created udev test bed %s", testbed->priv->root_dir);
}

/**
 * umockdev_testbed_new:
 *
 * Construct a #UMockdevTestbed object with no devices. Use
 * #umockdev_testbed_add_device to populate it. This automatically sets the
 * UMOCKDEV_DIR environment variable so that subsequently started gudev
 * clients will use the test bed.
 *
 * Returns: A new #UMockdevTestbed object. Free with g_object_unref().
 */
UMockdevTestbed *
umockdev_testbed_new (void)
{
  return UMOCKDEV_TESTBED (g_object_new (UMOCKDEV_TYPE_TESTBED, NULL));
}

/**
 * umockdev_testbed_get_root_dir:
 * @testbed: A #UMockdevTestbed.
 *
 * Gets the root directory for @testbed.
 *
 * Returns: (transfer none): The root directory for @testbed. Do not free or
 * modify.
 */
const gchar *
umockdev_testbed_get_root_dir (UMockdevTestbed *testbed)
{
  g_return_val_if_fail (UMOCKDEV_IS_TESTBED (testbed), NULL);
  return testbed->priv->root_dir;
}

/**
 * umockdev_testbed_get_sys_dir:
 * @testbed: A #UMockdevTestbed.
 *
 * Gets the sysfs directory for @testbed.
 *
 * Returns: (transfer none): The sysfs directory for @testbed. Do not free or
 * modify.
 */
const gchar *
umockdev_testbed_get_sys_dir (UMockdevTestbed *testbed)
{
  g_return_val_if_fail (UMOCKDEV_IS_TESTBED (testbed), NULL);
  return testbed->priv->sys_dir;
}

/**
 * uevent_from_property_list:
 *
 * Build the contents of an uevent file (with udev properties) from a property
 * list.
 */
static gchar*
uevent_from_property_list (const gchar** properties)
{
  GString *result;
  const gchar *key, *value;

  result = g_string_sized_new (1024);

  while (*properties != NULL)
    {
      key = *properties;
      ++properties;
      if (*properties == NULL)
        {
          g_warning ("uevent_from_property_list: Ignoring key '%s' without value", key);
          break;
        }
      value = *properties;
      ++properties;
      g_string_append (result, key);
      g_string_append_c (result, '=');
      g_string_append (result, value);
      g_string_append_c (result, '\n');
    }

  return g_string_free (result, FALSE);
}

/**
 * umockdev_testbed_add_devicev:
 * @testbed: A #UMockdevTestbed.
 * @subsystem: The subsystem name, e. g. "usb"
 * @name: The device name; arbitrary, but needs to be unique within the testbed
 * @attributes: (transfer none): A list of device sysfs attributes, alternating
 *              names and values, terminated with NULL:
 *              { "key1", "value1", "key2", "value2", ..., NULL }
 * @properties: (transfer none): A list of device udev properties; same format
 *              as @attributes
 *
 * This method is mostly meant for language bindings (where it is named
 * #umockdev_testbed_add_device). For C programs it is usually more convenient to
 * use #umockdev_testbed_add_device.
 *
 * Add a new device to the @testbed. A Linux kernel device always has a
 * subsystem (such as "usb" or "pci"), and a device name. The test bed only
 * builds a very simple sysfs structure without nested namespaces, so it
 * requires device names to be unique. Some gudev client programs might make
 * assumptions about the name (e. g. a SCSI disk block device should be called
 * sdaN). A device also has an arbitrary number of sysfs attributes and udev
 * properties; usually you should specify them upon creation, but it is also
 * possible to change them later on with #umockdev_testbed_set_attribute and
 * #umockdev_testbed_set_property.
 *
 * Returns: (transfer full): The sysfs path for the newly created device. Free
 *          with g_free().
 *
 * Rename to: umockdev_testbed_add_device
 */
gchar*
umockdev_testbed_add_devicev (UMockdevTestbed  *testbed,
                              const gchar      *subsystem,
                              const gchar      *name,
                              const gchar     **attributes,
                              const gchar     **properties)
{
  gchar *dev_path;
  gchar *dev_dir;
  gchar *class_dir;
  gchar *target, *link;
  const gchar *key, *value;
  gchar *prop_str;

  g_return_val_if_fail (UMOCKDEV_IS_TESTBED (testbed), NULL);

  dev_path = g_build_filename ("/sys/devices", name, NULL);
  dev_dir = g_build_filename (testbed->priv->root_dir, dev_path, NULL);

  /* must not exist yet */
  g_return_val_if_fail (!g_file_test (dev_dir, G_FILE_TEST_EXISTS), NULL);

  /* create device and corresponding subsystem dir */
  g_assert (g_mkdir_with_parents (dev_dir, 0755) == 0);
  class_dir = g_build_filename (testbed->priv->sys_dir, "class", subsystem, NULL);
  g_assert (g_mkdir_with_parents (class_dir, 0755) == 0);

  /* subsystem symlink */
  target = g_build_filename ("..", "..", "class", subsystem, NULL);
  link = g_build_filename (dev_dir, "subsystem", NULL);
  g_assert (symlink (target, link) == 0);
  g_free (target);
  g_free (link);

  /* device symlink from class/ */
  target = g_build_filename ("..", "..", "devices", name, NULL);
  link = g_build_filename (class_dir, name, NULL);
  g_assert (symlink (target, link) == 0);
  g_free (target);
  g_free (link);

  g_free (class_dir);
  g_free (dev_dir);

  /* attributes */
  while (*attributes != NULL)
    {
      key = *attributes;
      ++attributes;
      if (*attributes == NULL)
        {
          g_warning ("umockdev_testbed_add_devicev: Ignoring attribute key '%s' without value", key);
          break;
        }
      value = *attributes;
      ++attributes;
      umockdev_testbed_set_attribute (testbed, dev_path, key, value);
    }

  /* properties; they go into the "uevent" sysfs attribute */
  prop_str = uevent_from_property_list (properties);
  umockdev_testbed_set_attribute (testbed, dev_path, "uevent", prop_str);
  g_free (prop_str);

  /* we want to return a realistic device path, not one starting with the the
   * testbed prefix */
  return dev_path;
}

/**
 * umockdev_testbed_add_device: (skip)
 * @testbed: A #UMockdevTestbed.
 * @subsystem: The subsystem name, e. g. "usb"
 * @name: The device name; arbitrary, but needs to be unique within the testbed
 * @...: Arbitrarily many pairs of sysfs attributes (alternating names and
 *       values), terminated by NULL, followed by arbitrarily many pairs of udev
 *       properties, terminated by another NULL.
 *
 * Add a new device to the @testbed. A Linux kernel device always has a
 * subsystem (such as "usb" or "pci"), and a device name. The test bed only
 * builds a very simple sysfs structure without nested namespaces, so it
 * requires device names to be unique. Some gudev client programs might make
 * assumptions about the name (e. g. a SCSI disk block device should be called
 * sdaN). A device also has an arbitrary number of sysfs attributes and udev
 * properties; usually you should specify them upon creation, but it is also
 * possible to change them later on with #umockdev_testbed_set_attribute and
 * #umockdev_testbed_set_property.
 *
 * Example:
 *   |[
 *   umockdev_testbed_add_device (testbed, "usb", "dev1",
 *                              "idVendor", "0815", "idProduct", "AFFE", NULL,
 *                              "ID_MODEL", "KoolGadget", NULL);
 *   ]|
 *
 * Returns: (transfer full): The sysfs path for the newly created device. Free
 *          with g_free().
 */
gchar*
umockdev_testbed_add_device (UMockdevTestbed *testbed,
                             const gchar     *subsystem,
                             const gchar     *name,
                             ...)
{
  va_list args;
  int arg_set = 0; /* 0 -> attributes, 1 -> properties */
  gchar *syspath;
  const gchar *arg;
  GArray *attributes;
  GArray *properties;

  g_return_val_if_fail (UMOCKDEV_IS_TESTBED (testbed), NULL);

  attributes = g_array_new (TRUE, FALSE, sizeof (gchar*));
  properties = g_array_new (TRUE, FALSE, sizeof (gchar*));

  va_start (args, name);

  for (;;) {
    arg = va_arg (args, const gchar*);
    /* we iterate arguments until NULL twice; first for the attributes, then
     * for the properties */
    if (arg == NULL)
      {
        if (++arg_set > 1)
          break;
        else
          continue;
      }

    if (arg_set == 0)
      g_array_append_val (attributes, arg);
    else
      g_array_append_val (properties, arg);
  }

  syspath = umockdev_testbed_add_devicev (testbed,
                                        subsystem,
                                        name,
                                        (const gchar**) attributes->data,
                                        (const gchar**) properties->data);

  g_array_free (attributes, FALSE);
  g_array_free (properties, FALSE);

  va_end (args);

  return syspath;
}


/**
 * umockdev_testbed_set_attribute:
 * @testbed: A #UMockdevTestbed.
 * @devpath: The full device path, as returned by #umockdev_testbed_add_device
 * @name: The attribute name
 * @value: The attribute value
 *
 * Set a sysfs attribute of a device.
 */
void
umockdev_testbed_set_attribute (UMockdevTestbed *testbed,
                                const gchar     *devpath,
                                const gchar     *name,
                                const gchar     *value)
{
  gchar *attr_path;
  GError *error = NULL;

  g_return_if_fail (UMOCKDEV_IS_TESTBED (testbed));

  attr_path = g_build_filename (testbed->priv->root_dir, devpath, name, NULL);
  g_file_set_contents (attr_path, value, -1, &error);
  g_assert_no_error (error);
  g_free (attr_path);
}

/**
 * umockdev_testbed_set_property:
 * @testbed: A #UMockdevTestbed.
 * @devpath: The full device path, as returned by #umockdev_testbed_add_device
 * @name: The property name
 * @value: The property value
 *
 * Set an udev property of a device.
 */
void
umockdev_testbed_set_property (UMockdevTestbed *testbed,
                               const gchar     *devpath,
                               const gchar     *name,
                               const gchar     *value)
{
  size_t name_len;
  gchar *uevent_path;
  gboolean existing = FALSE;
  GString *props;
  FILE *f;
  char line[4096];

  g_return_if_fail (UMOCKDEV_IS_TESTBED (testbed));

  name_len = strlen (name);

  /* read current properties from the uevent file; if name is already set,
   * replace its value with the new one */
  uevent_path = g_build_filename (testbed->priv->root_dir, devpath, "uevent", NULL);
  f = fopen (uevent_path, "r");
  g_assert (f != NULL);

  props = g_string_sized_new (1024);
  while (fgets (line, sizeof (line), f) != NULL)
    {
      if (g_str_has_prefix (line, name) && line[name_len] == '=')
        {
          existing = TRUE;
          g_string_append (props, name);
          g_string_append_c (props, '=');
          g_string_append (props, value);
          g_string_append_c (props, '\n');
        }
      else
        {
          g_string_append (props, line);
        }
    }
  fclose (f);

  /* if property name does not yet exist, append it */
  if (!existing)
    {
      g_string_append (props, name);
      g_string_append_c (props, '=');
      g_string_append (props, value);
      g_string_append_c (props, '\n');
    }


  /* write it back */
  f = fopen (uevent_path, "w");
  g_assert (f != NULL);
  g_assert_cmpint (fwrite (props->str, sizeof (gchar), props->len, f), ==, props->len);
  fclose (f);

  g_string_free (props, TRUE);
  g_free (uevent_path);
}

/**
 * umockdev_testbed_uevent:
 * @testbed: A #UMockdevTestbed
 * @devpath: The full device path, as returned by #umockdev_testbed_add_device
 * @action: "add", "remove", or "change"
 *
 * Generate an uevent for a device.
 */
void
umockdev_testbed_uevent (UMockdevTestbed  *testbed,
                         const gchar      *devpath,
                         const gchar      *action)
{
  g_return_if_fail (UMOCKDEV_IS_TESTBED (testbed));

  g_debug ("umockdev_testbed_uevent: sending uevent %s for device %s", action, devpath);

  uevent_sender_send (testbed->priv->uevent_sender, devpath, action);
}
