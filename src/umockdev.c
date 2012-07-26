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
#include <errno.h>
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
 * You can either add devices by specifying individual device paths,
 * properties, and attributes, or use the umockdump tool to create a human
 * readable/editable dump from a real device (and all its parents) and load
 * that into the testbed with umockdev_testbed_add_from_string().
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
  GRegex        *re_dump_val;
  GRegex        *re_dump_keyval;
};

G_DEFINE_TYPE (UMockdevTestbed, umockdev_testbed, G_TYPE_OBJECT)

/**
 * remove_dir:
 *
 * Recursively remove a directory and all its contents.
 */
static void
remove_dir (const gchar* path)
{
  if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
    {
      GError *error = NULL;
      GDir *d;
      const gchar *name;

      d = g_dir_open (path, 0, &error);
      if (d == NULL)
        {
          g_warning ("cannot open %s: %s", path, error->message);
          return;
        }
      while ((name = g_dir_read_name (d)) != NULL)
        {
          gchar *name_path;
          name_path = g_build_filename (path, name, NULL);
          remove_dir (name_path);
          g_free (name_path);
        }
      g_dir_close (d);

    }

  if (g_remove (path) < 0)
    g_warning ("cannot remove %s: %s", path, strerror (errno));
}

static void
umockdev_testbed_finalize (GObject *object)
{
  UMockdevTestbed *testbed = UMOCKDEV_TESTBED (object);

  remove_dir (testbed->priv->root_dir);

  g_debug ("Removing test bed %s", testbed->priv->root_dir);
  g_unsetenv ("UMOCKDEV_DIR");

  g_free (testbed->priv->root_dir);
  g_free (testbed->priv->sys_dir);

  uevent_sender_close (testbed->priv->uevent_sender);

  if (testbed->priv->re_dump_val != NULL)
    g_regex_unref (testbed->priv->re_dump_val);
  if (testbed->priv->re_dump_keyval != NULL)
    g_regex_unref (testbed->priv->re_dump_keyval);

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
 * umockdev_testbed_add_device() or umockdev_testbed_add_from_string() to populate
 * it. This automatically sets the UMOCKDEV_DIR environment variable so that
 * subsequently started gudev clients will use the test bed.
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

static const gchar*
make_dotdots (const gchar* devpath)
{
  static gchar dots[PATH_MAX];
  unsigned len = 0;
  unsigned count = 0;
  const gchar *offset;

  /* count slashes in devpath */
  for (offset = devpath; offset != NULL && offset[0]; offset = index (offset, '/'))
    {
      count++;
      offset++; /* advance beyond / */
    }

  /* we need one .. less than count */
  --count;

  for (dots[0] = '\0'; count > 0 && len < sizeof(dots); --count)
    {
      strcat (dots, "../");
      len += 3;
    }

  return dots;
}

/**
 * umockdev_testbed_add_devicev:
 * @testbed: A #UMockdevTestbed.
 * @subsystem: The subsystem name, e. g. "usb"
 * @name: The device name; arbitrary, but needs to be unique within the testbed
 * @parent: (allow-none): device path of the parent device. Use %NULL for a
 *          top-level device.
 * @attributes: (array zero-terminated=1) (transfer none): 
 *              A list of device sysfs attributes, alternating names and
 *              values, terminated with NULL:
 *              { "key1", "value1", "key2", "value2", ..., NULL }
 * @properties: (array zero-terminated=1) (transfer none): 
 *              A list of device udev properties; same format as @attributes
 *
 * This method is mostly meant for language bindings (where it is named
 * umockdev_testbed_add_device()). For C programs it is usually more convenient to
 * use umockdev_testbed_add_device().
 *
 * Add a new device to the @testbed. A Linux kernel device always has a
 * subsystem (such as "usb" or "pci"), and a device name. The test bed only
 * builds a very simple sysfs structure without nested namespaces, so it
 * requires device names to be unique. Some gudev client programs might make
 * assumptions about the name (e. g. a SCSI disk block device should be called
 * sdaN). A device also has an arbitrary number of sysfs attributes and udev
 * properties; usually you should specify them upon creation, but it is also
 * possible to change them later on with umockdev_testbed_set_attribute() and
 * umockdev_testbed_set_property().
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
                              const gchar      *parent,
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

  if (parent != NULL)
    {
      if (!g_file_test (parent, G_FILE_TEST_IS_DIR))
        {
          g_critical ("umockdev_testbed_add_devicev(): parent device %s does not exist", parent);
          return NULL;
        }
      dev_path = g_build_filename (parent, name, NULL);
    }
  else
    dev_path = g_build_filename ("/sys/devices", name, NULL);
  dev_dir = g_build_filename (testbed->priv->root_dir, dev_path, NULL);

  /* must not exist yet; do allow existing children, though */
  if (g_file_test (dev_dir, G_FILE_TEST_EXISTS))
    {
      gboolean uevent_exists;
      prop_str = g_build_filename (dev_dir, "uevent", NULL);
      uevent_exists = g_file_test (prop_str, G_FILE_TEST_EXISTS);
      g_free (prop_str);
      if (uevent_exists)
        g_error ("device %s already exists", dev_dir);
    }

  /* create device and corresponding subsystem dir */
  g_assert (g_mkdir_with_parents (dev_dir, 0755) == 0);
  class_dir = g_build_filename (testbed->priv->sys_dir, "class", subsystem, NULL);
  g_assert (g_mkdir_with_parents (class_dir, 0755) == 0);

  /* subsystem symlink */
  target = g_build_filename (make_dotdots(dev_path), "class", subsystem, NULL);
  link = g_build_filename (dev_dir, "subsystem", NULL);
  g_assert (symlink (target, link) == 0);
  g_free (target);
  g_free (link);

  /* device symlink from class/ */
  target = g_build_filename ("..", "..", strstr (dev_path, "/devices/"), NULL);
  /* skip directories in name; this happens when being called from
   * add_from_string() when the parent devices do not exist yet */
  link = g_build_filename (class_dir, g_path_get_basename (name), NULL);
  g_assert (symlink (target, link) == 0);
  g_free (target);
  g_free (link);

  g_free (class_dir);
  g_free (dev_dir);

  /* bus symlink */
  if (strcmp (subsystem, "usb") == 0 || strcmp (subsystem, "pci") == 0)
    {
      class_dir = g_build_filename (testbed->priv->sys_dir, "bus", subsystem, "devices", NULL);
      g_assert (g_mkdir_with_parents (class_dir, 0755) == 0);

      target = g_build_filename ("..", "..", "..", strstr (dev_path, "/devices/"), NULL);
      link = g_build_filename (class_dir, name, NULL);
      g_assert (symlink (target, link) == 0);
      g_free (link);
      g_free (class_dir);
      g_free (target);
    }

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
 * @parent: (allow-none): device path of the parent device. Use %NULL for a
 *          top-level device.
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
 * possible to change them later on with umockdev_testbed_set_attribute() and
 * umockdev_testbed_set_property().
 *
 * Example:
 *   |[
 *   umockdev_testbed_add_device (testbed, "usb", "dev1", NULL,
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
                             const gchar     *parent,
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

  va_start (args, parent);

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
                                        parent,
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
 * @devpath: The full device path, as returned by umockdev_testbed_add_device()
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
  umockdev_testbed_set_attribute_binary (testbed, devpath, name, value, -1);
}

void
umockdev_testbed_set_attribute_int (UMockdevTestbed *testbed,
                                    const gchar      *devpath,
                                    const gchar      *name,
                                    int               value)
{
  static gchar str[40];

  snprintf (str, sizeof (str), "%i", value);
  umockdev_testbed_set_attribute (testbed, devpath, name, str);
}

void
umockdev_testbed_set_attribute_hex (UMockdevTestbed *testbed,
                                    const gchar     *devpath,
                                    const gchar     *name,
                                    unsigned         value)
{
  static gchar str[40];

  snprintf (str, sizeof (str), "%x", value);
  umockdev_testbed_set_attribute (testbed, devpath, name, str);
}


/**
 * umockdev_testbed_set_attribute_binary:
 * @testbed: A #UMockdevTestbed.
 * @devpath: The full device path, as returned by umockdev_testbed_add_device()
 * @name: The attribute name
 * @value: (array length=value_len) (element-type guint8): The attribute value
 * @value_len: Length of @value in bytes
 *
 * Set a binary sysfs attribute of a device which can include null bytes.
 */
void
umockdev_testbed_set_attribute_binary (UMockdevTestbed *testbed,
                                       const gchar     *devpath,
                                       const gchar     *name,
                                       const gchar     *value,
                                       gssize           value_len)
{
  gchar *attr_path;
  GError *error = NULL;

  g_return_if_fail (UMOCKDEV_IS_TESTBED (testbed));

  attr_path = g_build_filename (testbed->priv->root_dir, devpath, name, NULL);
  g_file_set_contents (attr_path, value, value_len, &error);
  g_assert_no_error (error);
  g_free (attr_path);
}

/**
 * umockdev_testbed_set_property:
 * @testbed: A #UMockdevTestbed.
 * @devpath: The full device path, as returned by umockdev_testbed_add_device()
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

void
umockdev_testbed_set_property_int (UMockdevTestbed *testbed,
                                   const gchar      *devpath,
                                   const gchar      *name,
                                   int               value)
{
  static gchar str[40];

  snprintf (str, sizeof (str), "%i", value);
  umockdev_testbed_set_property (testbed, devpath, name, str);
}

void
umockdev_testbed_set_property_hex (UMockdevTestbed *testbed,
                                   const gchar     *devpath,
                                   const gchar     *name,
                                   unsigned         value)
{
  static gchar str[40];

  snprintf (str, sizeof (str), "%x", value);
  umockdev_testbed_set_property (testbed, devpath, name, str);
}

/**
 * umockdev_testbed_uevent:
 * @testbed: A #UMockdevTestbed
 * @devpath: The full device path, as returned by umockdev_testbed_add_device()
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

/**
 * umockdev_testbed_dump_parse_line:
 * @testbed: A #UMockdevTestbed
 * @data: String to parse
 * @type: (out): Pointer to a gchar which will get the line type (one of P, N,
 *        S, E, or H)
 * @key:  (out): Pointer to a string which will get the key name; this will be
 *        set to NULL for line types which do not have a key (P, N, S). You
 *        need to free this with g_free().
 * @value: (out): Pointer to a string which will get the value. You need to
 *         free this with g_free().
 *
 * Parse one line from a device dump.
 *
 * Returns: Pointer to the next line start in @data, or %NULL if the first line
 * is not valid.
 */
static const gchar*
umockdev_testbed_dump_parse_line (UMockdevTestbed *testbed,
                                  const gchar     *data,
                                  gchar           *type,
                                  gchar          **key,
                                  gchar          **value)
{
  GMatchInfo *match = NULL;
  gboolean is_match;
  gchar *type_str;
  int end_pos;

  g_assert (type != NULL);
  g_assert (key != NULL);
  g_assert (value != NULL);

  is_match = g_regex_match (testbed->priv->re_dump_val, data, 0, &match);
  if (is_match)
    {
      *key = NULL;
      *value = g_match_info_fetch (match, 2);
    }
  else
    {
      g_match_info_free (match);
      if (g_regex_match (testbed->priv->re_dump_keyval, data, 0, &match))
        {
          *key = g_match_info_fetch (match, 2);
          *value = g_match_info_fetch (match, 3);
        }
      else
        {
          g_match_info_free (match);
          *type = '\0';
          *key = NULL;
          *value = NULL;
          return NULL;
        }
    }

  g_assert (*value);

  type_str = g_match_info_fetch (match, 1);
  g_assert (type_str != NULL);
  *type = type_str[0];
  g_free (type_str);

  g_assert (g_match_info_fetch_pos (match, 0, NULL, &end_pos));
  g_match_info_free (match);

  return data + end_pos;
}

static inline char
hexdigit (char i)
{
  return (i > 'a') ? (i - 'a' + 10) :
         (i > 'A') ? (i - 'A' + 10) :
         (i - '0');
}

static gchar*
decode_hex (const gchar* hex)
{
  gchar *bin;
  size_t len, i;
 
  len = strlen (hex);
  /* hex digits must come in pairs */
  if (len % 2 != 0)
    return NULL;

  bin = g_new0 (char, len+1);

  for (i = 0; i < len/2; i++)
    bin[i] = (hexdigit (hex[i*2]) << 4) | hexdigit (hex[i*2+1]);

  return bin;
}

static const gchar*
umockdev_testbed_add_dev_from_string (UMockdevTestbed *testbed,
                                      const gchar      *data,
                                      GError          **error)
{
  gchar *devpath;
  gchar *syspath;
  gchar *subsystem = NULL;
  GPtrArray *attrs;
  GPtrArray *binattrs;
  GPtrArray *props;
  gchar  type;
  gchar *key;
  gchar *value;
  gchar *value_decoded;
  guint  i;

  /* the first line must be "P: devpath */
  data = umockdev_testbed_dump_parse_line (testbed, data, &type, &key, &value);

  if (data == NULL || type != 'P')
    {
      g_set_error_literal (error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE,
                           "device descriptions must start with a \"P: /devices/path/...\" line");
      return NULL;
    }
  devpath = value;

  if (!g_str_has_prefix (devpath, "/devices/"))
    {
      g_set_error (error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_VALUE,
                   "invalid device path '%s': must start with /devices/",
                   devpath);
      return NULL;
    }

  attrs = g_ptr_array_new_full (10, g_free);
  props = g_ptr_array_new_full (10, g_free);
  binattrs = g_ptr_array_sized_new (10);

  /* scan until we see an empty line */
  while (strlen (data) > 0 && data[0] != '\n')
    {
      data = umockdev_testbed_dump_parse_line (testbed, data, &type, &key, &value);
       
      if (data == NULL)
        {
          g_set_error (error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE,
                       "malformed attribute or property line in description of device %s",
                       devpath);
          data = NULL;
          goto out;
        }

      //g_debug ("umockdev_testbed_add_dev_from_string: type %c key %s val %s", type, key, value);
      switch (type)
        {
          case 'H':
            value_decoded = decode_hex (value);
            if (value_decoded == NULL)
              {
                g_set_error (error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE,
                             "malformed hexadecimal value: %s",
                             value);
                data = NULL;
                goto out;
              }
            g_ptr_array_add (binattrs, key);
            g_ptr_array_add (binattrs, value_decoded);
            /* append the length of the data, stored in a pointer */
            value_decoded = GSIZE_TO_POINTER (strlen (value) / 2);
            g_ptr_array_add (binattrs, value_decoded);

            g_free (value);
            break;

          case 'A':
            value_decoded = g_strcompress (value);
            g_free (value);
            g_ptr_array_add (attrs, key);
            g_ptr_array_add (attrs, value_decoded);
            break;

          case 'E':
            g_ptr_array_add (props, key);
            g_ptr_array_add (props, value);
            if (strcmp (key, "SUBSYSTEM") == 0)
              {
                if (subsystem != NULL)
                  {
                    g_set_error (error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_VALUE,
                                 "duplicate SUBSYSTEM property in description of device %s",
                                 devpath);
                    data = NULL;
                    goto out;
                  }
                subsystem = value;
              }
            break;

          case 'P':
            g_set_error (error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_PARSE,
                         "invalid P: line in description of device %s",
                         devpath);
            data = NULL;
            goto out;

          default:
            g_assert_not_reached ();
        }
    }

  if (subsystem == NULL)
    {
      g_set_error (error, UMOCKDEV_ERROR, UMOCKDEV_ERROR_VALUE,
                   "missing SUBSYSTEM property in description of device %s",
                   devpath);
      data = NULL;
      goto out;
    }

  /* NULL-terminate */
  g_ptr_array_add (attrs, NULL);
  g_ptr_array_add (props, NULL);

  /* create device */
  syspath = umockdev_testbed_add_devicev (testbed, subsystem,
                                          devpath + strlen ("/devices/"),
                                          NULL,
                                          (const gchar **) attrs->pdata,
                                          (const gchar **) props->pdata);
  g_assert (syspath != NULL);

  /* add binary attributes */
  g_assert (binattrs->len % 3 == 0);
  for (i = 0; i < binattrs->len; i += 3)
    {
      umockdev_testbed_set_attribute_binary (testbed,
                                             syspath,
                                             g_ptr_array_index (binattrs, i),
                                             g_ptr_array_index (binattrs, i+1),
                                             GPOINTER_TO_SIZE (g_ptr_array_index (binattrs, i+2)));
      g_free (g_ptr_array_index (binattrs, i));
      g_free (g_ptr_array_index (binattrs, i+1));
    }

  g_free (syspath);

  /* skip over multiple blank lines */
  while (data[0] != '\0' && data[0] == '\n')
      ++data;

out:
  g_free (devpath);
  g_ptr_array_free (props, TRUE);
  g_ptr_array_free (attrs, TRUE);
  g_ptr_array_free (binattrs, TRUE);

  return data;
}

/**
 * umockdev_testbed_add_from_string:
 * @testbed: A #UMockdevTestbed.
 * @data: Description of the device(s) as generated with umockdump
 * @error: (allow-none): return location for a #GError, or %NULL.
 *
 * Add a set of devices to the @testbed from a textual description. This reads
 * the format generated by the umockdump tool.
 *
 * Each paragraph defines one device. A line starts with a type tag (like 'E'),
 * followed by a colon, followed by either a value or a "key=value" assignment,
 * depending on the type tag. A device description must start with a 'P:' line.
 * Available type tags are: 
 * <itemizedlist>
 *   <listitem><type>P:</type> <emphasis>path</emphasis>: device path in sysfs, starting with
 *             <filename>/devices/</filename>; must occur exactly once at the
 *             start of device definition</listitem>
 *   <listitem><type>E:</type> <emphasis>key=value</emphasis>: udev property
 *             </listitem>
 *   <listitem><type>A:</type> <emphasis>key=value</emphasis>: ASCII sysfs
 *             attribute, with backslash-style escaping of \ (\\) and newlines
 *             (\n)</listitem>
 *   <listitem><type>H:</type> <emphasis>key=value</emphasis>: binary sysfs
 *             attribute, with the value being written as continuous hex string
 *             (e. g. 0081FE0A..)</listitem>
 * </itemizedlist>
 *
 * Returns: %TRUE on success, %FALSE if the data is invalid and an error
 *          occurred.
 *
 * Rename to: umockdev_testbed_add_device
 */
gboolean
umockdev_testbed_add_from_string (UMockdevTestbed *testbed,
                                  const gchar      *data,
                                  GError          **error)
{
  GError *error1 = NULL;

  /* lazily initialize the parsing regexps */
  if (testbed->priv->re_dump_val == NULL)
    {
      GError *e = NULL;
      testbed->priv->re_dump_val = g_regex_new ("^([PNS]): (.*)(?>\\R|$)",
                                                G_REGEX_OPTIMIZE,
                                                0, &e);
      g_assert_no_error (e);
      g_assert (testbed->priv->re_dump_val);
      testbed->priv->re_dump_keyval = g_regex_new ("^([EAH]): ([a-zA-Z0-9_:+-]+)=(.*)(?>\\R|$)",
                                                   G_REGEX_OPTIMIZE,
                                                   0, &e);
      g_assert_no_error (e);
      g_assert (testbed->priv->re_dump_keyval);
    }

  while (data[0] != '\0')
    {
      data = umockdev_testbed_add_dev_from_string (testbed, data, &error1);
      if (error1 != NULL)
        {
          g_propagate_error (error, error1);
          return FALSE;
        }
      g_assert (data != NULL);
    }
  
  return TRUE;
}

/**
 * SECTION:umockdeverror
 * @title: UMockdevError
 * @short_description: Possible errors that can be returned
 *
 * Error codes.
 */

GQuark
umockdev_error_quark ()
{
  static GQuark ret = 0;

  if (ret == 0)
    ret = g_quark_from_static_string ("umockdev_error");

  return ret;
}

#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
/**
 * umockdev_error_get_type:
 */
GType
umockdev_error_get_type ()
{
  static GType etype = 0;
  
  if (etype == 0)
    {
      static const GEnumValue values[] = {
              ENUM_ENTRY (UMOCKDEV_ERROR_PARSE, "UMockdevErrorParse"),
              ENUM_ENTRY (UMOCKDEV_ERROR_VALUE, "UMockdevErrorValue"),
              { 0, 0, 0 }
      };
      g_assert (UMOCKDEV_NUM_ERRORS == G_N_ELEMENTS (values) - 1);
      etype = g_enum_register_static ("UMockdevError", values);
    }
  return etype;
}
