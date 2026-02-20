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

using UMockdevUtils;
using Assertions;
using GLibc;

#if HAVE_SELINUX
using Selinux;
#endif

string rootdir;

/* exception-handling wrappers */
static int
checked_open_tmp (string tmpl, out string name_used) {
    try {
        return FileUtils.open_tmp (tmpl, out name_used);
    } catch (Error e) {
        error ("Failed to open temporary file: %s", e.message);
    }
}

static void
checked_file_get_contents (string filename, out string contents)
{
    try {
        FileUtils.get_contents (filename, out contents);
    } catch (FileError e) {
        error ("Failed to read %s contents: %s", filename, e.message);
    }
}

static void
tb_add_from_string (UMockdev.Testbed tb, string s)
{
    try {
        assert (tb.add_from_string (s));
    } catch (Error e) {
        error ("Failed to call Testbed.add_from_string(): %s", e.message);
    }
}
void
t_testbed_empty ()
{
  var tb = new UMockdev.Testbed ();
  assert (tb != null);
  var enumerator = new GUdev.Enumerator (new GUdev.Client(null));
  var devices = enumerator.execute ();

  assert_cmpuint (devices.length(), CompareOperator.EQ, 0);
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
  assert_cmpstr (syspath, CompareOperator.EQ, "/sys/devices/extkeyboard1");

  var enumerator = new GUdev.Enumerator (new GUdev.Client(null));
  var devices = enumerator.execute ();
  assert_cmpuint (devices.length(), CompareOperator.EQ, 1);

  GUdev.Device device = devices.nth_data(0);
  assert_cmpstr (device.get_name (), CompareOperator.EQ, "extkeyboard1");
  assert_cmpstr (device.get_sysfs_path (), CompareOperator.EQ, "/sys/devices/extkeyboard1");
  assert_cmpstr (device.get_subsystem (), CompareOperator.EQ, "usb");
  assert_cmpstr (device.get_sysfs_attr ("idVendor"), CompareOperator.EQ, "0815");
  assert_cmpstr (device.get_sysfs_attr ("idProduct"), CompareOperator.EQ, "AFFE");
  assert_cmpstr (device.get_sysfs_attr ("noSuchAttr"), CompareOperator.EQ, null);
  assert_cmpstr (device.get_property ("DEVPATH"), CompareOperator.EQ, "/devices/extkeyboard1");
  assert_cmpstr (device.get_property ("SUBSYSTEM"), CompareOperator.EQ, "usb");
  assert_cmpstr (device.get_property ("ID_INPUT"), CompareOperator.EQ, "1");
  assert_cmpstr (device.get_property ("ID_INPUT_KEYBOARD"), CompareOperator.EQ, "1");
  assert_cmpstr (device.get_property ("NO_SUCH_PROP"), CompareOperator.EQ, null);
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

  assert_cmpuint (devices.length (), CompareOperator.EQ, 2);
  foreach (var dev in devices) {
      assert_cmpstr (dev.get_subsystem(), CompareOperator.EQ, "usb");
      if (dev.get_sysfs_path () == "/sys/devices/myusbhub") {
          assert_cmpstr (dev.get_name(), CompareOperator.EQ, "myusbhub");
          assert_cmpstr (dev.get_device_file(), CompareOperator.EQ, "/dev/bus/usb/001/001");
      } else {
          assert_cmpstr (dev.get_sysfs_path (), CompareOperator.EQ, "/sys/devices/myusbhub/cam");
          assert_cmpstr (dev.get_name(), CompareOperator.EQ, "cam");
          assert_cmpstr (dev.get_device_file(), CompareOperator.EQ, "/dev/bus/usb/001/002");
      }
  }

}

void
assert_listdir (string path, string[] entries)
{
  var files = new List<string>();

  try {
    var dir = Dir.open(path);
    string? entry;
    while ((entry = dir.read_name()) != null)
        files.append(entry);
  } catch (Error e) {
      error ("Failed to list dir: %s", e.message);
  }
  files.sort(strcmp);
  assert_cmpuint (files.length(), CompareOperator.EQ, entries.length);
  uint i = 0;
  foreach (var n in files)
      assert_cmpstr (n, CompareOperator.EQ, entries[i++]);
}

void
t_testbed_fs_ops ()
{
  var have_real_sys = FileUtils.test("/sys", FileTest.EXISTS);
  var tb = new UMockdev.Testbed ();
  var orig_cwd = Environment.get_current_dir ();

  var syspath = tb.add_devicev ("pci", "dev1", null, {"a", "1"}, {"DEVTYPE", "fancy"});
  assert_cmpstr (syspath, CompareOperator.EQ, "/sys/devices/dev1");

  // absolute paths
  assert_listdir ("/sys", {"bus", "class", "devices"});
  assert_listdir ("/sys/devices", {"dev1"});
  assert_listdir ("/sys/bus", {"pci"});
  assert_listdir ("/sys/devices/dev1", {"a", "subsystem", "uevent"});

  // change directory into trapped /sys
  assert_cmpint (Posix.chdir ("/sys"), CompareOperator.EQ, 0);
  assert_listdir (".", {"bus", "class", "devices"});
  assert_listdir ("bus", {"pci"});
  assert_cmpstr (Environment.get_current_dir (), CompareOperator.EQ, "/sys");

  assert_cmpint (Posix.chdir ("/sys/devices/dev1"), CompareOperator.EQ, 0);
  assert_listdir (".", {"a", "subsystem", "uevent"});
  assert_cmpstr (Environment.get_current_dir (), CompareOperator.EQ, "/sys/devices/dev1");

  // relative paths into trapped /sys; this only works if the real /sys exists, as otherwise realpath() fails in trap_path()
  if (!have_real_sys) {
      stdout.printf ("[SKIP relative paths: environment has no real /sys]\n");
      return;
  }

  assert_cmpint (Posix.chdir ("/"), CompareOperator.EQ, 0);
  assert_listdir ("sys", {"bus", "class", "devices"});
  assert_listdir ("sys/devices", {"dev1"});
  assert_listdir ("sys/bus", {"pci"});

  assert_cmpint (Posix.chdir ("/etc"), CompareOperator.EQ, 0);
  assert_listdir ("../sys", {"bus", "class", "devices"});
  assert_listdir ("../sys/devices", {"dev1"});
  assert_listdir ("../sys/bus", {"pci"});

  assert_cmpint (Posix.chdir (orig_cwd), CompareOperator.EQ, 0);
}

#if HAVE_SELINUX
void
t_testbed_selinux ()
{
  int exit;
  try {
      Process.spawn_command_line_sync ("command -v selinuxenabled", null, null, out exit);
  } catch (SpawnError e) {
      exit = 1;
  }
  if (exit != 0) {
      stdout.printf ("[SKIP SELinux not active]\n");
      return;
  }

  var tb = new UMockdev.Testbed ();

  // valid context
  tb_add_from_string (tb, """P: /devices/myusbhub/cam
N: bus/usb/001/002
E: SUBSYSTEM=usb
E: DEVTYPE=usb_device
E: DEVNAME=/dev/bus/usb/001/002
E: __DEVCONTEXT=system_u:object_r:device_t:s0
""");

  string context;
  assert_cmpint (Selinux.lgetfilecon ("/dev/bus/usb/001/002", out context), CompareOperator.GT, 0);
  assert_cmpstr (context, CompareOperator.EQ, "system_u:object_r:device_t:s0");

  // invalidly context
  tb_add_from_string (tb, """P: /devices/invalidcontext
N: invalidcontext
E: SUBSYSTEM=tty
E: DEVNAME=/dev/invalidcontext
E: __DEVCONTEXT=blah
""");

  assert (FileUtils.test("/dev/invalidcontext", FileTest.EXISTS));
  string root_context;
  assert_cmpint (Selinux.lgetfilecon (tb.get_root_dir(), out root_context), CompareOperator.GT, 0);
  assert_cmpint (Selinux.lgetfilecon ("/dev/invalidcontext", out context), CompareOperator.GT, 0);
  // has default context
  assert_cmpstr (context, CompareOperator.EQ, root_context);
}
#endif

void
t_usbfs_ioctl_static ()
{
  var tb = new UMockdev.Testbed ();

  tb_add_from_string (tb, """P: /devices/mycam
N: 001
E: SUBSYSTEM=usb
""");

  int fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  int i = 1;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CLAIMINTERFACE, ref i), CompareOperator.EQ, 0);
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_GETDRIVER, ref i), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.ENODATA);
  Posix.errno = 0;

  /* no ioctl tree loaded */
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.GE, Posix.EINVAL);
  Posix.errno = 0;

  // unknown ioctls don't work on an emulated device
  int argp;
  assert_cmpint (Posix.ioctl (fd, Ioctl.FIONREAD, &argp), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.ENOTTY);
  Posix.close (fd);
  Posix.errno = 0;

  // Test that termios ioctls work on an emulated TTY device (backed by a real PTY)
  tb_add_from_string (tb, """P: /devices/serial/ttyTest
N: ttyTest
E: DEVNAME=/dev/ttyTest
E: SUBSYSTEM=tty
""");

  fd = Posix.open ("/dev/ttyTest", Posix.O_RDWR | Posix.O_NONBLOCK, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);
  Posix.errno = 0;
  // Use TCGETS - a simple termios ioctl; struct termios is ~60 bytes
  uint8 tio_data[128] = {0};
  assert_cmpint (Posix.ioctl (fd, (int) IoctlTermios.get_tcgets_ioctl(), tio_data), CompareOperator.EQ, 0);

  // Verify errno is preserved on successful termios ioctl (POSIX behavior)
  Posix.errno = Posix.EINVAL;  // Set a non-zero errno
  assert_cmpint (Posix.ioctl (fd, (int) IoctlTermios.get_tcgets_ioctl(), tio_data), CompareOperator.EQ, 0);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.EINVAL);  // Should be preserved

  // TCGETS with NULL argument fails with expected error
  Posix.errno = 0;
  assert_cmpint (Posix.ioctl (fd, (int) IoctlTermios.get_tcgets_ioctl(), null), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.EFAULT);

  // Non-termios ioctl on TTY is not forwarded to underlying PTY
  Posix.errno = 0;
  int dummy_val = 0;
  assert_cmpint (Posix.ioctl (fd, Ioctl.FIONREAD, out dummy_val), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.ENOTTY);

  Posix.close (fd);
  Posix.errno = 0;

  // unknown ioctls do work on non-emulated devices
  fd = Posix.open ("/dev/stdout", Posix.O_WRONLY, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);
  assert_cmpint (Posix.errno, CompareOperator.EQ, 0);
  assert_cmpint (Posix.ioctl (fd, Ioctl.FIONREAD, out argp), CompareOperator.EQ, 0);
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
  int fd = checked_open_tmp ("test_ioctl_tree.XXXXXX", out tmppath);
  assert_cmpint ((int) Posix.write (fd, test_tree, test_tree.length), CompareOperator.GT, 20);

  // ioctl emulation does not get in the way of non-/dev fds
  int i = 1;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CLAIMINTERFACE, ref i), CompareOperator.EQ, -1);
  // usually ENOTTY, but seem to be EINVAL
  assert_cmpint (Posix.errno, CompareOperator.GE, 22);

  Posix.close (fd);
  try {
      tb.load_ioctl ("/dev/001", tmppath);
  } catch (Error e) {
      error ("Cannot load ioctls: %s", e.message);
  }
  checked_remove (tmppath);

  fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  // static ioctl
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CLAIMINTERFACE, ref i), CompareOperator.EQ, 0);

  // loaded ioctl
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 0);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 11);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 0);

  /* loaded ioctl: URB */
  var urb_buffer = new uint8[4];
  Ioctl.usbdevfs_urb urb = {1, 129, 0, 0, urb_buffer, 4, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb), CompareOperator.EQ, 0);
  assert_cmpuint (urb.status, CompareOperator.EQ, 0);
  assert_cmpint (urb_buffer[0], CompareOperator.EQ, 0);

  Ioctl.usbdevfs_urb* urb_reap = null;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, 0);
  assert (urb_reap == &urb);
  assert_cmpint (urb.status, CompareOperator.EQ, -1);
  assert_cmpuint (urb.buffer[0], CompareOperator.EQ, 0x99);
  assert_cmpuint (urb.buffer[1], CompareOperator.EQ, 0x02);
  assert_cmpuint (urb.buffer[2], CompareOperator.EQ, 0xAA);
  assert_cmpuint (urb.buffer[3], CompareOperator.EQ, 0xFF);

  // open the device a second time
  int fd2 = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd2, CompareOperator.GE, 0);

  // exercise ioctl on fd2, should iterate from beginning
  ci.devnum = 99;
  ci.slow = 99;
  assert_cmpint (Posix.ioctl (fd2, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 0);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 11);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 0);

  // should still work on first fd, and continue with original tree state
  ci.devnum = 99;
  ci.slow = 99;
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 42);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 12);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 1);

  // should work after closing first fd, advancing position
  Posix.close (fd);
  ci.devnum = 99;
  ci.slow = 99;
  assert_cmpint (Posix.ioctl (fd2, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 42);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 12);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 1);
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
  int fd = checked_open_tmp ("test_ioctl_tree.XXXXXX", out tmppath);
  assert_cmpint ((int) Posix.write (fd, test_tree, test_tree.length), CompareOperator.GT, 20);

  Posix.close (fd);

  try {
      tb.load_ioctl (null, tmppath);
  } catch (Error e) {
      error ("Cannot load ioctls: %s", e.message);
  }
  checked_remove (tmppath);

  fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  // loaded ioctl
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 0);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 11);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 0);
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
  int fd = checked_open_tmp ("test_ioctl_tree.XXXXXX", out tmppath);
  assert_cmpint ((int) Posix.write (fd, test_tree, test_tree.length), CompareOperator.GT, 20);

  Posix.close (fd);

  try {
      tb.load_ioctl ("/dev/002", tmppath);
  } catch (Error e) {
      error ("Cannot load ioctls: %s", e.message);
  }
  checked_remove (tmppath);

  fd = Posix.open ("/dev/002", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  // loaded ioctl
  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 0);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 11);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 0);
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
  Posix.close (checked_open_tmp ("test_ioctl_tree.XXXXXX.xz", out tmppath));

  int exit;
  try {
      Process.spawn_command_line_sync (
            "sh -c 'echo \"" + test_tree + "\" | xz -9c > " + tmppath + "; sync'",
            null, null, out exit);
  } catch (SpawnError e) {
      error ("Cannot call xz: %s", e.message);
  }
  assert_cmpint (exit, CompareOperator.EQ, 0);
  try {
      tb.load_ioctl ("/dev/001", tmppath);
  } catch (Error e) {
      error ("Cannot load ioctls: %s", e.message);
  }
  checked_remove (tmppath);

  int fd = Posix.open ("/dev/001", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  var ci = Ioctl.usbdevfs_connectinfo();
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 0);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 11);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 0);

  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_CONNECTINFO, ref ci), CompareOperator.EQ, 42);
  assert_cmpuint (ci.devnum, CompareOperator.EQ, 12);
  assert_cmpuint (ci.slow, CompareOperator.EQ, 1);

  Posix.close (fd);
}

void
t_usbfs_ioctl_pcap ()
{
  var tb = new UMockdev.Testbed ();
  string device;
  Ioctl.usbdevfs_urb* urb_reap = null;

  GLibc.Utsname utsbuf;
  GLibc.uname (out utsbuf);
  if (utsbuf.machine ==  "sparc64") {
      stdout.printf ("[SKIP pre-recorded pcap does not work on sparc64]\n");
      return;
  }

  /* NOTE: This test is a bit ugly. It wasn't the best idea to use a USB keyboard. */

  checked_file_get_contents (Path.build_filename(rootdir + "/devices/input/usbkbd.pcap.umockdev"), out device);
  tb_add_from_string (tb, device);

  try {
      tb.load_pcap ("/sys/devices/pci0000:00/0000:00:14.0/usb1/1-3", Path.build_filename(rootdir + "/devices/input/usbkbd.pcap.pcapng"));
  } catch (Error e) {
      error ("Cannot load pcap file: %s", e.message);
  }

  int fd = Posix.open ("/dev/bus/usb/001/011", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  /* We can submit this early, even if it comes later in the recording! */
  var urb_buffer_ep1 = new uint8[8];
  Ioctl.usbdevfs_urb urb_ep1 = {1, 0x81, 0, 0, urb_buffer_ep1, 8, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb_ep1), CompareOperator.EQ, 0);
  assert_cmpuint (urb_ep1.status, CompareOperator.EQ, 0);

  /* Not all control transfers are skipped in this case, we need to speak USBHID */
  /* SET_IDLE request */
  uint8 urb_buffer_setup_set_idle[8] = { 0x21, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  Ioctl.usbdevfs_urb urb_set_idle = {2, 0x00, 0, 0, urb_buffer_setup_set_idle, 8, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb_set_idle), CompareOperator.EQ, 0);

  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, 0);
  assert (urb_reap == &urb_set_idle);

  /* GET DESCIPTOR is skipped again. */

  /* SET_REPORT request */
  uint8 urb_buffer_setup_set_report[9] = { 0x21, 0x09, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x00};
  Ioctl.usbdevfs_urb urb_set_report = {2, 0x00, 0, 0, urb_buffer_setup_set_report, 9, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb_set_report), CompareOperator.EQ, 0);

  /* Now we'll receive the SET_REPORT response as EP 1 has been submitted already */
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, 0);
  assert (urb_reap == &urb_set_report);

  /* We cannot reap any urbs yet as we are waiting for SET_IDLE */
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.EAGAIN);

  /* Another SET_IDLE */
  urb_buffer_setup_set_idle = { 0x21, 0x0a, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
  urb_set_idle = {2, 0x00, 0, 0, urb_buffer_setup_set_idle, 8, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb_set_idle), CompareOperator.EQ, 0);

  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, 0);
  assert (urb_reap == &urb_set_idle);

  /* Another SET_REPORT request */
  urb_buffer_setup_set_report = { 0x21, 0x09, 0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x01};
  urb_set_report = {2, 0x00, 0, 0, urb_buffer_setup_set_report, 9, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb_set_report), CompareOperator.EQ, 0);

  /* We cannot reap any urbs yet, because we didn't make a request on EP 2 */
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.EAGAIN);

  /* Submit on EP 2 */
  var urb_buffer_ep2 = new uint8[4];
  Ioctl.usbdevfs_urb urb_ep2 = {1, 0x82, 0, 0, urb_buffer_ep2, 4, 0};
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb_ep2), CompareOperator.EQ, 0);
  assert_cmpuint (urb_ep2.status, CompareOperator.EQ, 0);

  /* Now we'll receive the SET_REPORT response as EP 1 has been submitted already */
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, 0);
  assert (urb_reap == &urb_set_report);

  /* The first report is: 00000c0000000000 (EP1) */
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, 0);
  assert (urb_reap == &urb_ep1);
  assert_cmpint (urb_ep1.status, CompareOperator.EQ, 0);
  assert_cmpuint (urb_ep1.buffer[0], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[1], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[2], CompareOperator.EQ, 0x0c);
  assert_cmpuint (urb_ep1.buffer[3], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[4], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[5], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[6], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[7], CompareOperator.EQ, 0x00);

  /* Resubmit URB to get next report */
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_SUBMITURB, ref urb_ep1), CompareOperator.EQ, 0);

  /* The second report is: 0000000000000000 (EP1) */
  assert_cmpint (Posix.ioctl (fd, Ioctl.USBDEVFS_REAPURB, ref urb_reap), CompareOperator.EQ, 0);
  assert (urb_reap == &urb_ep1);
  assert_cmpint (urb_ep1.status, CompareOperator.EQ, 0);
  assert_cmpuint (urb_ep1.buffer[0], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[1], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[2], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[3], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[4], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[5], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[6], CompareOperator.EQ, 0x00);
  assert_cmpuint (urb_ep1.buffer[7], CompareOperator.EQ, 0x00);

  Posix.close (fd);
}

void
t_spidev_ioctl ()
{
  // does not work on 32 bit big-endian (such as Debian hppa)
  if (sizeof(long) != sizeof(int64) && BYTE_ORDER == ByteOrder.BIG_ENDIAN) {
      stdout.printf ("[SKIP: SPI emulation does not work on 32 bit big-endian] ");
      return;
  }

  var tb = new UMockdev.Testbed ();

  string device;
  checked_file_get_contents (Path.build_filename(rootdir + "/devices/spi/elanfingerprint.umockdev"), out device);
  tb_add_from_string (tb, device);

  try {
      tb.load_ioctl ("/dev/spidev0.0", Path.build_filename(rootdir + "/devices/spi/elanfingerprint.ioctl"));
  } catch (Error e) {
      error ("Cannot load ioctl file: %s", e.message);
  }

  int fd = Posix.open ("/dev/spidev0.0", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  /* A buffer for the xfer structs because vala doesn't compile otherwise
   * See https://gitlab.gnome.org/GNOME/vala/-/issues/1082 */
  var xfer_buf = new uint8[sizeof(Ioctl.spi_ioc_transfer) * 2];
  var tx_buf = new uint8[2];
  var rx_buf = new uint8[1];
  Ioctl.spi_ioc_transfer *xfer = xfer_buf;

  /* First a two byte write and one byte read. */

  tx_buf[0] = 0x03;
  tx_buf[1] = 0xff;

  Posix.memset (xfer, 0, sizeof (Ioctl.spi_ioc_transfer) * 2);
  /* these casts are evil, bad, and wrong -- but that's how Linux defines them, even 32 bit platforms have u64 */
  xfer[0].tx_buf = (uint64) tx_buf;
  xfer[0].len = 2;
  xfer[1].rx_buf = (uint64) rx_buf;
  xfer[1].len = 1;

  /* Not sure if this should return 3 or 0. */
  assert_cmpint (Posix.ioctl (fd, Ioctl.SPI_IOC_MESSAGE (2), xfer), CompareOperator.GE, 0);
  assert_cmpint (rx_buf[0], CompareOperator.EQ, 0x81);


  /* One byte write, use write(). */
  tx_buf[0] = 0x31;
  assert_cmpint ((int) Posix.write (fd, tx_buf, 1), CompareOperator.EQ, 1);

  /* Two byte write and one byte read, but doing it as three transfers
   * and the last one even as a read(). */
  tx_buf[0] = 0x08;
  tx_buf[1] = 0xff;

  Posix.memset (xfer, 0, sizeof (Ioctl.spi_ioc_transfer) * 2);
  xfer[0].tx_buf = (uint64) tx_buf;
  xfer[0].len = 1;
  xfer[1].tx_buf = (uint64) &tx_buf[1];
  xfer[1].len = 1;
  xfer[1].cs_change = 1;

  assert_cmpint (Posix.ioctl (fd, Ioctl.SPI_IOC_MESSAGE (2), xfer), CompareOperator.GE, 0);

  assert_cmpint ((int) Posix.read (fd, rx_buf, 1), CompareOperator.EQ, 1);
  assert_cmpint (rx_buf[0], CompareOperator.EQ, 0x5f);

  Posix.close (fd);
}

void
t_hidraw_ioctl ()
{
  var tb = new UMockdev.Testbed ();

  string device;
  checked_file_get_contents (Path.build_filename(rootdir + "/devices/hidraw/fido2.umockdev"), out device);
  tb_add_from_string (tb, device);

  try {
      tb.load_ioctl ("/dev/hidraw5", Path.build_filename(rootdir + "/devices/hidraw/fido2.ioctl"));
  } catch (Error e) {
      error ("Cannot load ioctl file: %s", e.message);
  }

  int fd = Posix.open ("/dev/hidraw5", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  int i = 0;
  assert_cmpint (Posix.ioctl (fd, Ioctl.HIDIOCGRDESCSIZE, ref i), CompareOperator.EQ, 0);
  // HACK: This is broken on big-endian machines like s390x and ppc64, it is 0x22000000 there
  if (BYTE_ORDER == ByteOrder.LITTLE_ENDIAN)
      assert_cmpint (i, CompareOperator.EQ, 34);
  Ioctl.hidraw_report_descriptor desc = { 34, };
  assert_cmpint (Posix.ioctl (fd, Ioctl.HIDIOCGRDESC, ref desc), CompareOperator.EQ, 0);
  uint8 desc_value[] = {
	  0x06, 0xD0, 0xF1, 0x09, 0x01, 0xA1, 0x01, 0x09, 0x20, 0x15, 0x00, 0x26,
	  0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02, 0x09, 0x21, 0x15, 0x00,
	  0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02, 0xC0
  };
  assert_cmpint (Posix.memcmp(desc.value, desc_value, 34), CompareOperator.EQ, 0);
  Posix.close (fd);
}

void
t_cros_ec_ioctl ()
{
  var tb = new UMockdev.Testbed ();

  string device;
  checked_file_get_contents (Path.build_filename(rootdir + "/devices/cros_ec/crosfingerprint.umockdev"), out device);
  tb_add_from_string (tb, device);

  try {
      tb.load_ioctl ("/dev/cros_fp", Path.build_filename(rootdir + "/devices/cros_ec/crosfingerprint.ioctl"));
  } catch (Error e) {
      error ("Cannot load ioctl file: %s", e.message);
  }

  int fd = Posix.open ("/dev/cros_fp", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  Ioctl.cros_ec_command_v2 *s_cmd = malloc (sizeof (Ioctl.cros_ec_command_v2) + 4);
  assert_cmpint (Posix.ioctl (fd, Ioctl.CROS_EC_DEV_IOCXCMD_V2, s_cmd), CompareOperator.EQ, 4);
  uint8 fpmode_data[] = {
    0x80, 0x00, 0x00, 0x00
  };
  assert_cmpint (Posix.memcmp(s_cmd->data, fpmode_data, 4), CompareOperator.EQ, 0);

  s_cmd = realloc(s_cmd, sizeof (Ioctl.cros_ec_command_v2) + 48);
  assert_cmpint (Posix.ioctl (fd, Ioctl.CROS_EC_DEV_IOCXCMD_V2, s_cmd), CompareOperator.EQ, 48);
  uint8 fpinfo_data[] = {
    0x46, 0x50, 0x43, 0x20, 0x09, 0x00, 0x00, 0x00, 0x1B, 0x02, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x94, 0x66, 0x00, 0x00, 0x47, 0x52, 0x45, 0x59,
    0xA0, 0x00, 0xA0, 0x00, 0x08, 0x00, 0xFF, 0x03, 0x24, 0x14, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00
  };
  assert_cmpint (Posix.memcmp(s_cmd->data, fpinfo_data, 48), CompareOperator.EQ, 0);

  s_cmd = realloc(s_cmd, sizeof (Ioctl.cros_ec_command_v2) + 22);
  assert_cmpint (Posix.ioctl (fd, Ioctl.CROS_EC_DEV_IOCXCMD_V2, s_cmd), CompareOperator.EQ, 22);
  uint8 fpstats_data[] = {
    0x65, 0x63, 0x01, 0x00, 0xB4, 0x4A, 0x02, 0x00, 0x07, 0xB3, 0x03, 0x00,
    0xDC, 0x33, 0x50, 0x5A, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  assert_cmpint (Posix.memcmp(s_cmd->data, fpstats_data, 22), CompareOperator.EQ, 0);

  free (s_cmd);
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
  assert_cmpint (Posix.lstat ("/dev/ttyUSB1", out st), CompareOperator.EQ, 0);
  assert (Posix.S_ISCHR (st.st_mode));
  assert_cmpuint (Posix.major (st.st_rdev), CompareOperator.EQ, 188);
  assert_cmpuint (Posix.minor (st.st_rdev), CompareOperator.EQ, 1);

  // stty issues an ioctl; verify that it recognizes the fake device as a real tty
  string pout, perr;
  int pexit;
  try {
      Process.spawn_command_line_sync ("stty -F /dev/ttyUSB1", out pout, out perr, out pexit);
  } catch (SpawnError e) {
      error ("Cannot call stty: %s", e.message);
  }
  assert_cmpstr (perr, CompareOperator.EQ, "");
  assert_cmpint (pexit, CompareOperator.EQ, 0);
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
  assert_cmpint (client_fd, CompareOperator.GE, 0);

  var master_fd = tb.get_dev_fd ("/dev/ttyS10");
  assert_cmpint (master_fd, CompareOperator.GE, 0);

  char[] buf = new char[100];

  /* client -> master */
  assert_cmpint ((int) Posix.write (client_fd, "hello\n", 6), CompareOperator.EQ, 6);
  assert_cmpint ((int) Posix.read (master_fd, buf, 100), CompareOperator.EQ, 6);
  assert_cmpstr ((string) buf, CompareOperator.EQ, "hello\n");

  /* master -> client */
  buf = new char[100];
  assert_cmpint ((int) Posix.write (master_fd, "world\n", 6), CompareOperator.EQ, 6);
  assert_cmpint ((int) Posix.read (client_fd, buf, 100), CompareOperator.EQ, 6);
  assert_cmpstr ((string) buf, CompareOperator.EQ, "world\n");

  Posix.close (client_fd);
}

void
t_detects_running_in_testbed ()
{
    assert (UMockdev.in_mock_environment());
}

void
t_detects_not_running_in_testbed ()
{
    int pipefds[2];
    assert_cmpint (Posix.pipe(pipefds), CompareOperator.EQ, 0);

    Posix.pid_t pid = Posix.fork();
    assert_cmpint (pid, CompareOperator.NE, -1);

    if (pid == 0) {
        Posix.close(pipefds[0]);
        GLib.Environment.unset_variable("LD_PRELOAD");
        string myexe = Posix.realpath("/proc/self/exe");
        string[] argv = { myexe, "--test-outside-testbed", pipefds[1].to_string() };
        Posix.execv(myexe, argv);
        error ("execv /proc/self/exe = %s failed: %m", myexe);
    }
    Posix.close(pipefds[1]);

    char buf = 'x';
    assert_cmpint ((int) Posix.read(pipefds[0], &buf, 1), CompareOperator.EQ, 1);
    assert_cmpint (buf, CompareOperator.EQ, '0');

    Posix.close(pipefds[0]);
}

int
is_test_inside_testbed (int pipefd)
{
    char buf[1];
    buf[0] = '0';
    if (UMockdev.in_mock_environment())
        buf[0] = '1';

    assert_cmpint ((int) Posix.write(pipefd, buf, 1), CompareOperator.EQ, 1);
    return int.parse((string) buf);
}

class AttributeCounterThread {

    public AttributeCounterThread (UMockdev.Testbed tb, string syspath, string name, uint max) {
        this.tb = tb;
        this.syspath = syspath;
        this.name = name;
        this.count = max;
    }

    public void* run () {
        string attr_path = Path.build_filename (this.syspath, this.name);
        for (; this.count > 0; --this.count) {
            string cur_value;
            checked_file_get_contents (attr_path, out cur_value);
            tb.set_attribute_int (this.syspath, name, int.parse(cur_value) + 1);
        }

        return null;
    }

    private UMockdev.Testbed tb;
    private string name;
    private string syspath;
    private uint count;
}

/* set attributes in parallel; every thread handles a different
 * attribute, so no locking required */
void
t_mt_parallel_attr_distinct ()
{
  var tb = new UMockdev.Testbed ();

  string syspath = tb.add_devicev ("changelings", "rapid", null,
                                   { "c1", "0", "c2", "0", "c3", "0"},
                                   {});
  var t1d = new AttributeCounterThread (tb, syspath, "c1", 100);
  var t2d = new AttributeCounterThread (tb, syspath, "c2", 100);
  var t3d = new AttributeCounterThread (tb, syspath, "c3", 100);
  var t1 = new Thread<void*> ("t_c1", t1d.run);
  var t2 = new Thread<void*> ("t_c2", t2d.run);
  var t3 = new Thread<void*> ("t_c3", t3d.run);
  t1.join();
  t2.join();
  t3.join();

  string val;
  checked_file_get_contents(Path.build_filename(syspath, "c1"), out val);
  assert_cmpstr (val, CompareOperator.EQ, "100");
  checked_file_get_contents(Path.build_filename(syspath, "c2"), out val);
  assert_cmpstr (val, CompareOperator.EQ, "100");
  checked_file_get_contents(Path.build_filename(syspath, "c3"), out val);
  assert_cmpstr (val, CompareOperator.EQ, "100");
}


void
t_mt_uevent ()
{
  var tb = new UMockdev.Testbed ();
  var gudev = new GUdev.Client ({"pci"});
  var ml = new MainLoop ();
  uint add_count = 0;
  uint change_count = 0;
  uint num_changes = 100;

  gudev.uevent.connect((client, action, device) => {
      if (action == "add")
          add_count++;
      else {
          if (++change_count == num_changes)
              ml.quit ();
      }
    });

  // causes one "add" event
  var syspath = tb.add_devicev ("pci", "dev1", null, {"a", "1"}, {"DEVTYPE", "fancy"});

  // this thread is purely to create noise in the preload lib
  var t_noise = new Thread<void*> ("noise", () => {
      string contents;

      while (ml.is_running ()) {
          checked_file_get_contents ("/sys/devices/dev1/a", out contents);
          assert_cmpstr (contents, CompareOperator.EQ, "1");
          tb.set_property (syspath, "ID_FOO", "1");
      }
      return null;
  });

  // synthesize num_changes "change" events
  var t_emitter = new Thread<void*> ("emitter", () => {
      for (uint i = 0; i < num_changes; ++i)
          tb.uevent (syspath, "change");
      return null;
  });

  // fallback timeout
  Timeout.add(3000, () => { ml.quit(); return false; });
  ml.run ();
  t_emitter.join ();
  t_noise.join ();

  assert_cmpuint (add_count, CompareOperator.EQ, 1);
  assert_cmpuint (change_count, CompareOperator.EQ, num_changes);
}

static bool
ioctl_custom_handle_ioctl_cb(UMockdev.IoctlBase handler, UMockdev.IoctlClient client)
{
    if (client.request == 1) {
        client.complete(*(long*)client.arg.data, 0);
    } else if (client.request == 2) {
        client.complete(-1, Posix.ENOMEM);
    } else if (client.request == 3 ) {
        try {
            var data = client.arg.resolve(0, sizeof(int));

            *(int*) data.data = (int) 0xc00fffee;

            client.complete(0, 0);
        } catch (Error e) {
            error ("cannot resolve client arg: %s", e.message);
        }
    } else {
        client.complete(-5, 1);
    }
    return true;
}

static bool
ioctl_custom_handle_write_cb(UMockdev.IoctlBase handler, UMockdev.IoctlClient client)
{
    /* NOTE:
     *
     * This code uses g_object_{get,set}_data for simplicity. Any real code
     * should likely subclass IoctlBase, override the vfunc's and keep its
     * state in there (this only works if you do not need a per-client state).
     */
    client.set_data("written", client.arg);
    client.complete(client.arg.data.length, 0);
    return true;
}

static bool
ioctl_custom_handle_read_cb(UMockdev.IoctlBase handler, UMockdev.IoctlClient client)
{
    UMockdev.IoctlData written = client.steal_data("written");

    /* Return EAGAIN if nothing has been written. */
    if (written == null) {
        client.complete(-1, Posix.EAGAIN);
        return true;
    }

    /* Return the first n bytes of the buffer, then discard the rest. Real code
     * would likely need to track the offset.
     */
    ssize_t read_len = ssize_t.min(written.data.length, client.arg.data.length);

    Posix.memcpy(client.arg.data, written.data, read_len);

    client.complete(read_len, 0);

    return true;
}

void
t_ioctl_custom()
{
  var tb = new UMockdev.Testbed ();

  tb_add_from_string (tb, """P: /devices/test
N: test
E: SUBSYSTEM=test
""");

  var handler = new UMockdev.IoctlBase();
  var write_buf = new uint8[10] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
  var read_buf = new uint8[10] { 0 };
  int ioctl_target = 0;

  /* These are not native vala signals because of the accumulator. */
  handler.connect("signal::handle-ioctl", ioctl_custom_handle_ioctl_cb, null);
  handler.connect("signal::handle-read", ioctl_custom_handle_read_cb, null);
  handler.connect("signal::handle-write", ioctl_custom_handle_write_cb, null);

  try {
      tb.attach_ioctl("/dev/test", handler);
  } catch (Error e) {
      error ("Failed to attach ioctl: %s", e.message);
  }

  int fd = Posix.open ("/dev/test", Posix.O_RDWR, 0);
  assert_cmpint (fd, CompareOperator.GE, 0);

  int value = (int) 0xdeadbeef;
  assert_cmpint (Posix.ioctl (fd, 1, value), CompareOperator.EQ, value);

  assert_cmpint (Posix.ioctl (fd, 2, value), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.ENOMEM);

  assert_cmpint (Posix.ioctl (fd, 3, &ioctl_target), CompareOperator.EQ, 0);
  assert_cmpint (ioctl_target, CompareOperator.EQ, (int) 0xc00fffee);

  /* Test whether we can write and get the value mirrored back. */
  assert_cmpint ((int) Posix.write (fd, write_buf, 10), CompareOperator.EQ, 10);

  assert_cmpint ((int) Posix.read (fd, read_buf, 10), CompareOperator.EQ, 10);
  assert_cmpint (Posix.memcmp (read_buf, write_buf, 10), CompareOperator.EQ, 0);

  /* A further read returns EAGAIN */
  assert_cmpint ((int) Posix.read (fd, read_buf, 10), CompareOperator.EQ, -1);
  assert_cmpint (Posix.errno, CompareOperator.EQ, Posix.EAGAIN);


  Posix.close(fd);

  try {
      tb.detach_ioctl("/dev/test");
  } catch (Error e) {
      error ("Failed to detach ioctl: %s", e.message);
  }
}

int
main (string[] args)
{
  string? top_srcdir = Environment.get_variable ("TOP_SRCDIR");
  if (top_srcdir != null)
      rootdir = top_srcdir;
  else
      rootdir = ".";

  for (int i = 0; i < args.length; i++) {
      if (args[i] == "--test-outside-testbed")
          return is_test_inside_testbed(int.parse(args[i+1]));
  }
  Test.init (ref args);
  /* tests for mocking /sys */
  Test.add_func ("/umockdev-testbed-vala/empty", t_testbed_empty);
  Test.add_func ("/umockdev-testbed-vala/add_devicev", t_testbed_add_device);
  Test.add_func ("/umockdev-testbed-vala/gudev-query-list", t_testbed_gudev_query_list);
  Test.add_func ("/umockdev-testbed-vala/fs_ops", t_testbed_fs_ops);
#if HAVE_SELINUX
  Test.add_func ("/umockdev-testbed-vala/selinux", t_testbed_selinux);
#endif

  /* tests for mocking ioctls */
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_static", t_usbfs_ioctl_static);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree", t_usbfs_ioctl_tree);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree_with_default_device", t_usbfs_ioctl_tree_with_default_device);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree_override_default_device", t_usbfs_ioctl_tree_override_default_device);
  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_tree_xz", t_usbfs_ioctl_tree_xz);

  Test.add_func ("/umockdev-testbed-vala/usbfs_ioctl_pcap", t_usbfs_ioctl_pcap);

  Test.add_func ("/umockdev-testbed-vala/spidev_ioctl", t_spidev_ioctl);

  Test.add_func ("/umockdev-testbed-vala/hidraw_ioctl", t_hidraw_ioctl);

  Test.add_func ("/umockdev-testbed-vala/cros_ec_ioctl", t_cros_ec_ioctl);

  /* tests for mocking TTYs */
  Test.add_func ("/umockdev-testbed-vala/tty_stty", t_tty_stty);
  Test.add_func ("/umockdev-testbed-vala/tty_data", t_tty_data);

  /* test for umockdev-preload detection */
  Test.add_func ("/umockdev-testbed-vala/detects_running_in_testbed", t_detects_running_in_testbed);
  Test.add_func ("/umockdev-testbed-vala/detects_running_outside_testbed", t_detects_not_running_in_testbed);

  /* tests for multi-thread safety */
  Test.add_func ("/umockdev-testbed-vala/mt_parallel_attr_distinct", t_mt_parallel_attr_distinct);
  Test.add_func ("/umockdev-testbed-vala/mt_uevent", t_mt_uevent);

  /* test IoctlBase attachment and signals */
  Test.add_func ("/umockdev-testbed-vala/ioctl_custom", t_ioctl_custom);

  return Test.run();
}
