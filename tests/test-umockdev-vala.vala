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

  // unknown ioctls don't work on an emulated device
  assert_cmpint (Posix.ioctl (fd, Ioctl.TIOCSBRK, 0), Op.EQ, -1);
  assert_cmpint (Posix.errno, Op.EQ, Posix.ENOTTY);
  Posix.errno = 0;

  // unknown ioctls do work on non-emulated devices
  int fd2 = Posix.open ("/dev/tty", Posix.O_RDWR, 0);
  if (fd2 > 0) {
      assert_cmpint (Posix.ioctl (fd2, Ioctl.TIOCSBRK, 0), Op.EQ, 0);
      assert_cmpint (Posix.errno, Op.EQ, 0);
      Posix.close (fd2);
  }

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

void
t_detects_running_in_testbed ()
{
    assert (UMockdev.in_mock_environment());
}

void
t_detects_not_running_in_testbed ()
{
    int pipefds[2];
    assert_cmpint (Posix.pipe(pipefds), Op.EQ, 0);

    Posix.pid_t pid = Posix.fork();
    assert_cmpint (pid, Op.NE, -1);

    if (pid == 0) {
        Posix.close(pipefds[0]);
        GLib.Environment.unset_variable("LD_PRELOAD");
        string[] argv = { "--test-outside-testbed", pipefds[1].to_string() };
        Posix.execv("/proc/self/exe", argv);
    }
    Posix.close(pipefds[1]);

    char buf = 'x';
    assert_cmpint ((int) Posix.read(pipefds[0], &buf, 1), Op.EQ, 1);
    assert_cmpint (buf, Op.EQ, '0');

    Posix.close(pipefds[0]);
}

int
is_test_inside_testbed (int pipefd)
{
    char buf[1];
    buf[0] = '0';
    if (UMockdev.in_mock_environment())
        buf[0] = '1';

    Posix.write(pipefd, buf, 1);
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
            try {
                FileUtils.get_contents (attr_path, out cur_value);
            } catch (FileError e) {
                error ("(count %u) failed to read %s: %s", this.count, attr_path, e.message);
            }
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
  try {
      FileUtils.get_contents(Path.build_filename(syspath, "c1"), out val);
      assert_cmpstr (val, Op.EQ, "100");
      FileUtils.get_contents(Path.build_filename(syspath, "c2"), out val);
      assert_cmpstr (val, Op.EQ, "100");
      FileUtils.get_contents(Path.build_filename(syspath, "c3"), out val);
      assert_cmpstr (val, Op.EQ, "100");
  } catch (FileError e) {
      error ("failed to read attribute: %s", e.message);
  }
}


void
t_mt_uevent ()
{
  var tb = new UMockdev.Testbed ();
  var gudev = new GUdev.Client ({"pci"});
  var ml = new MainLoop ();
  uint add_count = 0;
  uint change_count = 0;
  uint num_changes = 10;

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
          try {
              FileUtils.get_contents ("/sys/devices/dev1/a", out contents);
          } catch (FileError e) {
              error ("(#changes: %u) Error opening attribute file: %s", change_count, e.message);
          }
          assert_cmpstr (contents, Op.EQ, "1");
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

  assert_cmpuint (add_count, Op.EQ, 1);
  assert_cmpuint (change_count, Op.EQ, num_changes);
}


int
main (string[] args)
{
  for (int i = 0; i < args.length; i++) {
      if (args[i] == "--test-outside-testbed")
          return is_test_inside_testbed(int.parse(args[i+1]));
  }
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

  /* test for umockdev-preload detection */
  Test.add_func ("/umockdev-testbed-vala/detects_running_in_testbed", t_detects_running_in_testbed);
  Test.add_func ("/umockdev-testbed-vala/detects_running_outside_testbed", t_detects_not_running_in_testbed);

  /* tests for multi-thread safety */
  Test.add_func ("/umockdev-testbed-vala/mt_parallel_attr_distinct", t_mt_parallel_attr_distinct);
  Test.add_func ("/umockdev-testbed-vala/mt_uevent", t_mt_uevent);

  return Test.run();
}
