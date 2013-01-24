/*
 * test-umockdev.vala
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

using Assertions;

void
t_testbed_empty ()
{
  var tb = new UMockdev.Testbed ();
  assert (tb != null);
  var enumerator = new GUdev.Enumerator (new GUdev.Client(null));
  var devices = enumerator.execute ();

  assert_cmpuint (devices.length(), OperatorType.EQUAL, 0);
}

void
t_testbed_add_device ()
{
  var tb = new UMockdev.Testbed ();

  string syspath = tb.add_devicev ("usb",
                                   "extkeyboard1",
                                   null,
                                   { "idVendor", "0815", "idProduct", "AFFE" },
                                   { "ID_INPUT", "1", "ID_INPUT_KEYBOARD", "1" });
  assert_cmpstr (syspath, OperatorType.EQUAL, "/sys/devices/extkeyboard1");

  var enumerator = new GUdev.Enumerator (new GUdev.Client(null));
  var devices = enumerator.execute ();
  assert_cmpuint (devices.length(), OperatorType.EQUAL, 1);

  GUdev.Device device = devices.nth_data(0);
  assert_cmpstr (device.get_name (), OperatorType.EQUAL, "extkeyboard1");
  assert_cmpstr (device.get_sysfs_path (), OperatorType.EQUAL, "/sys/devices/extkeyboard1");
  assert_cmpstr (device.get_subsystem (), OperatorType.EQUAL, "usb");
  assert_cmpstr (device.get_sysfs_attr ("idVendor"), OperatorType.EQUAL, "0815");
  assert_cmpstr (device.get_sysfs_attr ("idProduct"), OperatorType.EQUAL, "AFFE");
  assert_cmpstr (device.get_sysfs_attr ("noSuchAttr"), OperatorType.EQUAL, null);
  assert_cmpstr (device.get_property ("DEVPATH"), OperatorType.EQUAL, "/devices/extkeyboard1");
  assert_cmpstr (device.get_property ("SUBSYSTEM"), OperatorType.EQUAL, "usb");
  assert_cmpstr (device.get_property ("ID_INPUT"), OperatorType.EQUAL, "1");
  assert_cmpstr (device.get_property ("ID_INPUT_KEYBOARD"), OperatorType.EQUAL, "1");
  assert_cmpstr (device.get_property ("NO_SUCH_PROP"), OperatorType.EQUAL, null);
}


int
main (string[] args)
{
  Test.init (ref args);
  Test.add_func ("/umockdev-testbed-vala/empty", t_testbed_empty);
  Test.add_func ("/umockdev-testbed-vala/add_devicev", t_testbed_add_device);
  return Test.run();
}
