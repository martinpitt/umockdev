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

static void
tb_add_from_string (UMockdev.Testbed tb, string s)
{
    try {
        assert (tb.add_from_string (s));
    } catch (Error e) {
        stderr.printf ("Failed to call Testbed.add_from_string(): %s\n", e.message);
        Process.abort ();
    }
}
void
t_testbed_empty ()
{
  var tb = new UMockdev.Testbed ();
  assert (tb != null);
  var enumerator = new GUdev.Enumerator (new GUdev.Client(null));
  var devices = enumerator.execute ();

  assert_cmpuint (devices.length(), Op.EQ, 0);
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
  assert_cmpstr (syspath, Op.EQ, "/sys/devices/extkeyboard1");

  var enumerator = new GUdev.Enumerator (new GUdev.Client(null));
  var devices = enumerator.execute ();
  assert_cmpuint (devices.length(), Op.EQ, 1);

  GUdev.Device device = devices.nth_data(0);
  assert_cmpstr (device.get_name (), Op.EQ, "extkeyboard1");
  assert_cmpstr (device.get_sysfs_path (), Op.EQ, "/sys/devices/extkeyboard1");
  assert_cmpstr (device.get_subsystem (), Op.EQ, "usb");
  assert_cmpstr (device.get_sysfs_attr ("idVendor"), Op.EQ, "0815");
  assert_cmpstr (device.get_sysfs_attr ("idProduct"), Op.EQ, "AFFE");
  assert_cmpstr (device.get_sysfs_attr ("noSuchAttr"), Op.EQ, null);
  assert_cmpstr (device.get_property ("DEVPATH"), Op.EQ, "/devices/extkeyboard1");
  assert_cmpstr (device.get_property ("SUBSYSTEM"), Op.EQ, "usb");
  assert_cmpstr (device.get_property ("ID_INPUT"), Op.EQ, "1");
  assert_cmpstr (device.get_property ("ID_INPUT_KEYBOARD"), Op.EQ, "1");
  assert_cmpstr (device.get_property ("NO_SUCH_PROP"), Op.EQ, null);
}

void
t_testbed_gudev_query_list ()
{
  var tb = new UMockdev.Testbed ();

  tb_add_from_string (tb, """P: /devices/myusbhub/cam
N: bus/usb/001/002
E: SUBSYSTEM=usb
E: DEVTYPE=usb_device
E: DEVNAME=/dev/bus/usb/001/002

P: /devices/myusbhub
N: bus/usb/001/001
E: SUBSYSTEM=usb
E: DEVTYPE=usb_device
E: DEVNAME=/dev/bus/usb/001/001
""");

  var client = new GUdev.Client (null);
  var devices = client.query_by_subsystem (null);

  assert_cmpuint (devices.length (), Op.EQ, 2);
  foreach (var dev in devices) {
      assert_cmpstr (dev.get_subsystem(), Op.EQ, "usb");
      if (dev.get_sysfs_path () == "/sys/devices/myusbhub") {
          assert_cmpstr (dev.get_name(), Op.EQ, "myusbhub");
          assert_cmpstr (dev.get_device_file(), Op.EQ, "/dev/bus/usb/001/001");
      } else {
          assert_cmpstr (dev.get_sysfs_path (), Op.EQ, "/sys/devices/myusbhub/cam");
          assert_cmpstr (dev.get_name(), Op.EQ, "cam");
          assert_cmpstr (dev.get_device_file(), Op.EQ, "/dev/bus/usb/001/002");
      }
  }

}

void
t_usbfs_ioctl_static ()
{
  var tb = new UMockdev.Testbed ();

  tb_add_from_string (tb, """P: /devices/mycam
N: 001
E: SUBSYSTEM=usb
""");

  int fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, Op.GE, 0);

  int i = 1;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CLAIMINTERFACE, ref i), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_GETDRIVER, ref i), Op.EQ, -1);
  assert_cmpint (Posix.errno, Op.EQ, Posix.ENODATA);
  Posix.errno = 0;

  /* no ioctl tree loaded */
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, -1);
  // usually ENOTTY, but seem to be EINVAL
  assert_cmpint (Posix.errno, Op.GE, 22);
  errno = 0;

  Posix.close (fd);
}

void
t_usbfs_ioctl_tree ()
{
  var tb = new UMockdev.Testbed ();
  tb_add_from_string (tb, """P: /devices/mycam
N: 001
E: SUBSYSTEM=usb
""");

  // add simple ioctl tree
  string test_tree;
  if (BYTE_ORDER == ByteOrder.LITTLE_ENDIAN)
      test_tree = """# little-endian test ioctls
USBDEVFS_CONNECTINFO 0 0B00000000000000
USBDEVFS_REAPURB 0 1 129 -1 0 4 4 0 9902AAFF

# another connect info
USBDEVFS_CONNECTINFO 42 0C00000001000000
""";
  else
      test_tree = """# big-endian test ioctls
USBDEVFS_CONNECTINFO 0 0000000B00000000
USBDEVFS_REAPURB 0 1 129 -1 0 4 4 0 9902AAFF

# another connect info
USBDEVFS_CONNECTINFO 42 0000000C01000000
""";

  string tmppath;
  int fd;
  try {
      fd  = FileUtils.open_tmp ("test_ioctl_tree.XXXXXX", out tmppath);
  } catch (Error e) { Process.abort (); }
  assert_cmpint ((int) Posix.write (fd, test_tree, test_tree.length), Op.GT, 20);

  // ioctl emulation does not get in the way of non-/dev fds
  int i = 1;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CLAIMINTERFACE, ref i), Op.EQ, -1);
  // usually ENOTTY, but seem to be EINVAL
  assert_cmpint (Posix.errno, Op.GE, 22);

  Posix.close (fd);
  try {
      tb.load_ioctl ("/dev/001", tmppath);
  } catch (Error e) {
      stderr.printf ("Cannot load ioctls: %s\n", e.message);
      Process.abort ();
  }
  FileUtils.unlink (tmppath);

  fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, Op.GE, 0);

  // static ioctl
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CLAIMINTERFACE, ref i), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);

  // loaded ioctl
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 11);
  assert_cmpuint (ci.slow, Op.EQ, 0);

  /* loaded ioctl: URB */
  var urb_buffer = new uint8[4];
  Ioctl.usbdevfs_urb urb = {1, 129, 0, 0, urb_buffer, 4, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (urb.status, Op.EQ, 0);
  assert_cmpint (urb_buffer[0], Op.EQ, 0);

  Ioctl.usbdevfs_urb* urb_reap = null;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert (urb_reap == &urb);
  assert_cmpint (urb.status, Op.EQ, -1);
  assert_cmpuint (urb.buffer[0], Op.EQ, 0x99);
  assert_cmpuint (urb.buffer[1], Op.EQ, 0x02);
  assert_cmpuint (urb.buffer[2], Op.EQ, 0xAA);
  assert_cmpuint (urb.buffer[3], Op.EQ, 0xFF);

  // open the device a second time
  int fd2 = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd2, Op.GE, 0);

  // exercise ioctl on fd2, should iterate from beginning
  ci.devnum = 99;
  ci.slow = 99;
  assert_cmpint (Posix.ioctl (fd2, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 11);
  assert_cmpuint (ci.slow, Op.EQ, 0);

  // should still work on first fd, and continue with original tree state
  ci.devnum = 99;
  ci.slow = 99;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 42);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 12);
  assert_cmpuint (ci.slow, Op.EQ, 1);

  // should work after closing first fd, advancing position
  Posix.close (fd);
  ci.devnum = 99;
  ci.slow = 99;
  assert_cmpint (Posix.ioctl (fd2, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 42);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 12);
  assert_cmpuint (ci.slow, Op.EQ, 1);
  Posix.close (fd2);
}

void
t_usbfs_ioctl_tree_with_default_device ()
{
  var tb = new UMockdev.Testbed ();
  tb_add_from_string (tb, """P: /devices/mycam
N: 001
E: SUBSYSTEM=usb
""");

  // add simple ioctl tree
  string test_tree;
  if (BYTE_ORDER == ByteOrder.LITTLE_ENDIAN)
      test_tree = """# little-endian test ioctls
@DEV /dev/001
USBDEVFS_CONNECTINFO 0 0B00000000000000
""";
  else
      test_tree = """# big-endian test ioctls
@DEV /dev/001
USBDEVFS_CONNECTINFO 0 0000000B00000000
""";

  string tmppath;
  int fd;
  try {
      fd  = FileUtils.open_tmp ("test_ioctl_tree.XXXXXX", out tmppath);
  } catch (Error e) { Process.abort (); }
  assert_cmpint ((int) Posix.write (fd, test_tree, test_tree.length), Op.GT, 20);

  Posix.close (fd);

  try {
      tb.load_ioctl (null, tmppath);
  } catch (Error e) {
      stderr.printf ("Cannot load ioctls: %s\n", e.message);
      Process.abort ();
  }
  FileUtils.unlink (tmppath);

  fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, Op.GE, 0);

  // loaded ioctl
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 11);
  assert_cmpuint (ci.slow, Op.EQ, 0);
}

void
t_usbfs_ioctl_tree_override_default_device ()
{
  var tb = new UMockdev.Testbed ();
  tb_add_from_string (tb, """P: /devices/mycam
N: 002
E: SUBSYSTEM=usb
""");

  // add simple ioctl tree
  string test_tree;
  if (BYTE_ORDER == ByteOrder.LITTLE_ENDIAN)
      test_tree = """# little-endian test ioctls
@DEV /dev/001
USBDEVFS_CONNECTINFO 0 0B00000000000000
""";
  else
      test_tree = """# big-endian test ioctls
@DEV /dev/001
USBDEVFS_CONNECTINFO 0 0000000B00000000
""";

  string tmppath;
  int fd;
  try {
      fd  = FileUtils.open_tmp ("test_ioctl_tree.XXXXXX", out tmppath);
  } catch (Error e) { Process.abort (); }
  assert_cmpint ((int) Posix.write (fd, test_tree, test_tree.length), Op.GT, 20);

  Posix.close (fd);

  try {
      tb.load_ioctl ("/dev/002", tmppath);
  } catch (Error e) {
      stderr.printf ("Cannot load ioctls: %s\n", e.message);
      Process.abort ();
  }
  FileUtils.unlink (tmppath);

  fd = Posix.open ("/dev/002", Posix.O_RDWR, 0);
  assert_cmpint (fd, Op.GE, 0);

  // loaded ioctl
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 11);
  assert_cmpuint (ci.slow, Op.EQ, 0);
}


void
t_usbfs_ioctl_tree_xz ()
{
  var tb = new UMockdev.Testbed ();
  tb_add_from_string (tb, """P: /devices/mycam
N: 001
E: SUBSYSTEM=usb
""");

  // add simple ioctl tree
  string test_tree;
  if (BYTE_ORDER == ByteOrder.LITTLE_ENDIAN)
      test_tree = """USBDEVFS_CONNECTINFO 0 0B00000000000000
USBDEVFS_REAPURB 0 1 129 -1 0 4 4 0 9902AAFF
USBDEVFS_CONNECTINFO 42 0C00000001000000
""";
  else
      test_tree = """USBDEVFS_CONNECTINFO 0 0000000B00000000
USBDEVFS_REAPURB 0 1 129 -1 0 4 4 0 9902AAFF
USBDEVFS_CONNECTINFO 42 0000000C01000000
""";

  string tmppath;
  try {
      Posix.close (FileUtils.open_tmp ("test_ioctl_tree.XXXXXX.xz", out tmppath));
  } catch (Error e) { Process.abort (); }

  int exit;
  try {
      Process.spawn_command_line_sync (
            "sh -c 'echo \"" + test_tree + "\" | xz -9c > " + tmppath + "; sync'",
            null, null, out exit);
  } catch (SpawnError e) {
      stderr.printf ("Cannot call xz: %s\n", e.message);
      Process.abort ();
  }
  assert_cmpint (exit, Op.EQ, 0);
  try {
      tb.load_ioctl ("/dev/001", tmppath);
  } catch (Error e) {
      stderr.printf ("Cannot load ioctls: %s\n", e.message);
      Process.abort ();
  }
  FileUtils.unlink (tmppath);

  int fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, Op.GE, 0);

  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 0);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 11);
  assert_cmpuint (ci.slow, Op.EQ, 0);

  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), Op.EQ, 42);
  assert_cmpint (Posix.errno, Op.EQ, 0);
  assert_cmpuint (ci.devnum, Op.EQ, 12);
  assert_cmpuint (ci.slow, Op.EQ, 1);

  Posix.close (fd);
}

void
t_tty_stty ()
{
  var tb = new UMockdev.Testbed ();
  tb_add_from_string (tb, """P: /devices/usb/tty/ttyUSB1
N: ttyUSB1
E: DEVNAME=/dev/ttyUSB1
E: SUBSYSTEM=tty
A: dev=188:1
""");

  // appears as a proper char device
  Posix.Stat st;
  assert_cmpint (Posix.lstat ("/dev/ttyUSB1", out st), Op.EQ, 0);
  assert (Posix.S_ISCHR (st.st_mode));
  assert_cmpuint (Posix.major (st.st_rdev), Op.EQ, 188);
  assert_cmpuint (Posix.minor (st.st_rdev), Op.EQ, 1);

  // stty issues an ioctl; verify that it recognizes the fake device as a real tty
  string pout, perr;
  int pexit;
  try {
      Process.spawn_command_line_sync ("stty -F /dev/ttyUSB1", out pout, out perr, out pexit);
  } catch (SpawnError e) {
      stderr.printf ("Cannot call stty: %s\n", e.message);
      Process.abort ();
  }
  assert_cmpstr (perr, Op.EQ, "");
  assert_cmpint (pexit, Op.EQ, 0);
  assert (pout.contains ("speed 38400 baud"));
}

void
t_tty_data ()
{
  var tb = new UMockdev.Testbed ();
  tb_add_from_string (tb, """P: /devices/serial/ttyS10
N: ttyS10
E: DEVNAME=/dev/ttyS10
E: SUBSYSTEM=tty
A: dev=4:74
""");

  var client_fd = Posix.open ("/dev/ttyS10", Posix.O_RDWR, 0);
  assert_cmpint (client_fd, Op.GE, 0);

  var master_fd = tb.get_dev_fd ("/dev/ttyS10");
  assert_cmpint (master_fd, Op.GE, 0);

  char[] buf = new char[100];

  /* client -> master */
  assert_cmpint ((int) Posix.write (client_fd, "hello\n", 6), Op.EQ, 6);
  assert_cmpint ((int) Posix.read (master_fd, buf, 100), Op.EQ, 6);
  assert_cmpstr ((string) buf, Op.EQ, "hello\n");

  /* master -> client */
  buf = new char[100];
  assert_cmpint ((int) Posix.write (master_fd, "world\n", 6), Op.EQ, 6);
  assert_cmpint ((int) Posix.read (client_fd, buf, 100), Op.EQ, 6);
  assert_cmpstr ((string) buf, Op.EQ, "world\n");

  Posix.close (client_fd);
}

int
main (string[] args)
{
  Test.init (ref args);
  /* tests for mocking /sys */
  Test.add_func ("/umockdev-testbed-vala/empty", t_testbed_empty);
  Test.add_func ("/umockdev-testbed-vala/add_devicev", t_testbed_add_device);
  Test.add_func ("/umockdev-testbed-vala/gudev-query-list", t_testbed_gudev_query_list);

  /* tests for mocking ioctls */
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_static", t_usbfs_ioctl_static);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree", t_usbfs_ioctl_tree);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree_with_default_device", t_usbfs_ioctl_tree_with_default_device);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree_override_default_device", t_usbfs_ioctl_tree_override_default_device);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree_xz", t_usbfs_ioctl_tree_xz);

  /* tests for mocking TTYs */
  Test.add_func ("/umockdev-testbed-vala/tty_stty", t_tty_stty);
  Test.add_func ("/umockdev-testbed-vala/tty_data", t_tty_data);
  return Test.run();
}
