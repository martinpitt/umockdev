/*
 * test-umockdev-run.vala
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

using Assertions;

const string umockdev_run_command = "env LC_ALL=C umockdev-run ";

string rootdir;

static void
assert_in (string needle, string haystack)
{
    if (!haystack.contains (needle)) {
        stderr.printf ("'%s' not found in '%s'\n", needle, haystack);
        Process.abort();
    }
}

static bool
have_program (string program)
{
    string sout;
    int exit;

    try {
        Process.spawn_command_line_sync ("which " + program, out sout, null, out exit);
    } catch (SpawnError e) {
        stderr.printf ("cannot call which %s: %s\n", program, e.message);
        Process.abort();
    }

    return exit == 0;
}

static bool
get_program_out (string program, string command, out string sout,
                 out string serr, out int exit)
{
    if (!have_program (program)) {
        stderr.printf ("[SKIP: %s not installed] ", program);
        sout = "";
        serr = "";
        exit = -1;
        return false;
    }

    try {
        Process.spawn_command_line_sync (command, out sout, out serr, out exit);
    } catch (SpawnError e) {
        stderr.printf ("cannot call %s: %s\n", command, e.message);
        Process.abort();
    }

    return true;
}

static void
check_program_out (string program, string run_command, string expected_out)
{
    string sout;
    string serr;
    int exit;

    if (!get_program_out (program, umockdev_run_command + run_command, out sout, out serr, out exit))
        return;

    assert_cmpstr (sout, Op.EQ, expected_out);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
}

static void
check_program_error (string program, string run_command, string expected_err)
{
    string sout;
    string serr;
    int exit;

    if (!get_program_out (program, umockdev_run_command + run_command, out sout, out serr, out exit))
        return;

    assert_in (expected_err, serr);

    assert_cmpint (exit, Op.NE, 0);
    assert (Process.if_exited (exit));
    assert_cmpstr (sout, Op.EQ, "");
}

static void
t_run_exit_code ()
{
    string sout, serr;
    int exit;

    // normal exit, zero
    check_program_out ("true", umockdev_run_command + "true", "");

    // normal exit, nonzero
    get_program_out ("ls", umockdev_run_command + "ls /nonexisting", out sout, out serr, out exit);
    assert (Process.if_exited (exit));
    assert_cmpint (Process.exit_status (exit), Op.EQ, 2);
    assert_cmpstr (sout, Op.EQ, "");
    assert_cmpstr (serr, Op.NE, "");

    // signal exit
    get_program_out ("sh", umockdev_run_command + "-- sh -c 'kill -SEGV $$'", out sout, out serr, out exit);
    assert (Process.if_signaled (exit));
    assert_cmpint (Process.term_sig (exit), Op.EQ, ProcessSignal.SEGV);
    assert_cmpstr (sout, Op.EQ, "");
    assert_cmpstr (serr, Op.EQ, "");
}

static void
t_run_pipes ()
{
    string sout;
    string serr;
    int exit;

    // child program gets stdin, and we get proper stdout
    assert(get_program_out ("echo", "sh -c 'echo hello | " + umockdev_run_command + "cat'",
                            out sout, out serr, out exit));

    assert_cmpstr (sout, Op.EQ, "hello\n");
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
}

static void
t_run_invalid_args ()
{
    // missing program to run
    check_program_error ("true", "", "--help");
}

static void
t_run_invalid_ioctl ()
{
    // nonexisting ioctl file
    check_program_error ("gphoto2", "-d " + rootdir +
        "/devices/cameras/canon-powershot-sx200.umockdev -i " +
        "/dev/bus/usb/001/011=/non/existing.ioctl -- gphoto2 -l",
        "/non/existing.ioctl");

    // empty ioctl file
    check_program_error ("gphoto2", "-d " + rootdir +
        "/devices/cameras/canon-powershot-sx200.umockdev -i " +
        "/dev/bus/usb/001/011=/dev/null -- gphoto2 -l",
        "001/011");

    // invalid ioctl file
    check_program_error ("gphoto2", "-d " + rootdir +
        "/devices/cameras/canon-powershot-sx200.umockdev -i " +
        "/dev/bus/usb/001/011=" + rootdir + "/NEWS -- gphoto2 -l",
        "001/011");

    // unspecified ioctl file
    check_program_error ("gphoto2", "-d " + rootdir +
        "/devices/cameras/canon-powershot-sx200.umockdev -i " +
        "/dev/bus/usb/001/011 -- gphoto2 -l",
        "--ioctl");
}

static void
t_run_script_chatter ()
{
    string umockdev_file, script_file;

    // create umockdev and script files
    try {
        int fd = FileUtils.open_tmp ("ttyS0.XXXXXX.umockdev", out umockdev_file);
        Posix.close (fd);
        fd = FileUtils.open_tmp ("chatter.XXXXXX.script", out script_file);
        Posix.close (fd);

        FileUtils.set_contents (umockdev_file, """P: /devices/platform/serial8250/tty/ttyS0
N: ttyS0
E: DEVNAME=/dev/ttyS0
E: SUBSYSTEM=tty
A: dev=4:64""");

        FileUtils.set_contents (script_file, """w 0 Hello world!^JWhat is your name?^J
r 300 Joe Tester^J
w 0 I ♥ Joe Tester^Ja^I tab and a^J   line break in one write^J
r 200 somejunk^J
w 0 bye!^J""");
    } catch (FileError e) {
        stderr.printf ("cannot create temporary file: %s\n", e.message);
        Process.abort();
    }

    check_program_out ("true", "-d " + umockdev_file + " -s /dev/ttyS0=" + script_file +
                       " -- tests/chatter /dev/ttyS0",
                       "Got input: Joe Tester\nGot input: somejunk\n");

    FileUtils.remove (umockdev_file);
    FileUtils.remove (script_file);
}

static void
t_gphoto_detect ()
{
    check_program_out ("gphoto2",
        "-d " + rootdir + "/devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=" +
        rootdir + "/devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 --auto-detect",
        """Model                          Port            
----------------------------------------------------------
Canon PowerShot SX200 IS       usb:001,011     
""");
}

static void
t_gphoto_folderlist ()
{
    check_program_out ("gphoto2",
        "-d " + rootdir + "/devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=" +
            rootdir + "/devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 -l",
        """There is 1 folder in folder '/'.
 - store_00010001
There is 1 folder in folder '/store_00010001'.
 - DCIM
There is 1 folder in folder '/store_00010001/DCIM'.
 - 100CANON
There are 0 folders in folder '/store_00010001/DCIM/100CANON'.
""");
}

static void
t_gphoto_filelist ()
{
    check_program_out ("gphoto2",
        "-d " + rootdir + "/devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=" +
            rootdir + "/devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 -L",
        """There is no file in folder '/'.
There is no file in folder '/store_00010001'.
There is no file in folder '/store_00010001/DCIM'.
There are 9 files in folder '/store_00010001/DCIM/100CANON'.
#1     IMG_0095.JPG               rd  1640 KB 3264x2448 image/jpeg
#2     IMG_0096.JPG               rd  1669 KB 3264x2448 image/jpeg
#3     IMG_0097.JPG               rd  1741 KB 3264x2448 image/jpeg
#4     IMG_0098.JPG               rd  1328 KB 3264x2448 image/jpeg
#5     IMG_0099.JPG               rd  1290 KB 3264x2448 image/jpeg
#6     IMG_0100.JPG               rd  2340 KB 3264x2448 image/jpeg
#7     IMG_0101.JPG               rd  1916 KB 3264x2448 image/jpeg
#8     IMG_0102.JPG               rd  2026 KB 3264x2448 image/jpeg
#9     IMG_0103.JPG               rd  1810 KB 3264x2448 image/jpeg
""");
}

static void
t_input_touchpad ()
{
    if (BYTE_ORDER == ByteOrder.BIG_ENDIAN) {
        stderr.printf ("[SKIP: this test only works on little endian machines] ");
        return;
    }
    if (!have_program ("Xorg")) {
        stderr.printf ("[SKIP: Xorg not installed] ");
        return;
    }

    Pid xorg_pid;
    string logfile;
    try {
        int fd = FileUtils.open_tmp ("Xorg.log.XXXXXX", out logfile);
        Posix.close (fd);
    } catch (FileError e) {
        stderr.printf ("cannot create temporary file: %s\n", e.message);
        Process.abort();
    }
    try {
        Process.spawn_async (null, {"umockdev-run",
            "-d", rootdir + "/devices/input/synaptics-touchpad.umockdev",
            "-i", "/dev/input/event12=" + rootdir + "/devices/input/synaptics-touchpad.ioctl",
            "--", "Xorg", "-config", rootdir + "/tests/xorg-dummy.conf", "-logfile", logfile, ":5"},
            null, SpawnFlags.SEARCH_PATH | SpawnFlags.STDERR_TO_DEV_NULL, null, out xorg_pid);
    } catch (SpawnError e) {
        stderr.printf ("cannot call Xorg: %s\n", e.message);
        Process.abort();
    }

    /* wait until X socket is available */
    int timeout = 50;
    while (timeout > 0) {
        timeout -= 1;
        Posix.usleep (100000);
        if (FileUtils.test ("/tmp/.X11-unix/X5", FileTest.EXISTS))
            break;
    }
    if (timeout <= 0) {
        stderr.printf ("Xorg failed to start up; please ensure you have the X.org dummy driver installed, and check the log file: %s\n", logfile);
        Process.abort();
    }

    /* call xinput */
    string xinput_out, xinput_err;
    int xinput_exit;
    get_program_out ("xinput", "env DISPLAY=:5 xinput", out xinput_out, out xinput_err, out xinput_exit);

    string props_out, props_err;
    int props_exit;
    get_program_out ("xinput", "env DISPLAY=:5 xinput --list-props 'SynPS/2 Synaptics TouchPad'",
            out props_out, out props_err, out props_exit);

    /* shut down X */
    Posix.kill (xorg_pid, Posix.SIGTERM);
    int status;
    Posix.waitpid (xorg_pid, out status, 0);
    Process.close_pid (xorg_pid);
    FileUtils.remove (logfile);
    FileUtils.remove (logfile + ".old");

    assert_cmpstr (xinput_err, Op.EQ, "");
    assert_cmpint (xinput_exit, Op.EQ, 0);
    assert_in ("SynPS/2 Synaptics TouchPad", xinput_out);

    assert_cmpstr (props_err, Op.EQ, "");
    assert_cmpint (props_exit, Op.EQ, 0);
    assert_in ("Synaptics Two-Finger Scrolling", props_out);
    assert_in ("/dev/input/event12", props_out);
}

static void
t_input_evtest ()
{
    if (BYTE_ORDER == ByteOrder.BIG_ENDIAN) {
        stderr.printf ("[SKIP: this test only works on little endian machines] ");
        return;
    }

    if (!have_program ("evtest")) {
        stderr.printf ("[SKIP: evtest not installed] ");
        return;
    }

    Pid evtest_pid;
    int outfd, errfd;

    // FIXME: Is there a more elegant way?
    string script_arch;
    if (long.MAX == int64.MAX)
        script_arch = "64";
    else
        script_arch = "32";

    try {
        Process.spawn_async_with_pipes (null, {"umockdev-run",
            "-d", rootdir + "/devices/input/usbkbd.umockdev",
            "-i", "/dev/input/event5=" + rootdir + "/devices/input/usbkbd.evtest.ioctl",
            "-s", "/dev/input/event5=" + rootdir + "/devices/input/usbkbd.evtest.script." + script_arch,
            "evtest", "/dev/input/event5"},
            null, SpawnFlags.SEARCH_PATH, null,
            out evtest_pid, null, out outfd, out errfd);
    } catch (SpawnError e) {
        stderr.printf ("cannot call evtest: %s\n", e.message);
        Process.abort();
    }

    // our script covers 1.4 seconds, give it some slack
    Posix.sleep (2);
    Posix.kill (evtest_pid, Posix.SIGTERM);
    var sout = new uint8[10000];
    var serr = new uint8[10000];
    ssize_t sout_len = Posix.read (outfd, sout, sout.length);
    ssize_t serr_len = Posix.read (errfd, serr, sout.length);
    int status;
    Posix.waitpid (evtest_pid, out status, 0);
    Process.close_pid (evtest_pid);

    if (serr_len > 0) {
        serr[serr_len] = 0;
        stderr.printf ("evtest error: %s\n", (string) serr);
        Process.abort();
    }

    assert_cmpint ((int) sout_len, Op.GT, 10);
    sout[sout_len] = 0;
    string output = (string) sout;

    // check supported events
    assert_in ("Event type 1 (EV_KEY)", output);
    assert_in ("Event code 15 (KEY_TAB)", output);

    // check 'A' key event
    assert_in ("type 4 (EV_MSC), code 4 (MSC_SCAN), value 70004", output);
    assert_in ("type 1 (EV_KEY), code 30 (KEY_A), value 1\n", output);
    assert_in ("type 1 (EV_KEY), code 30 (KEY_A), value 0\n", output);

    // check 'left shift' key event
    assert_in ("type 4 (EV_MSC), code 4 (MSC_SCAN), value 700e1", output);
    assert_in ("type 1 (EV_KEY), code 42 (KEY_LEFTSHIFT), value 1\n", output);
    assert_in ("type 1 (EV_KEY), code 42 (KEY_LEFTSHIFT), value 0\n", output);
}

int
main (string[] args)
{
  Test.init (ref args);

  string? top_srcdir = Environment.get_variable ("TOP_SRCDIR");
  if (top_srcdir != null)
      rootdir = top_srcdir;
  else
      rootdir = ".";

  // general operations
  Test.add_func ("/umockdev-run/exit_code", t_run_exit_code);
  Test.add_func ("/umockdev-run/pipes", t_run_pipes);

  // boundary conditions
  Test.add_func ("/umockdev-run/invalid-args", t_run_invalid_args);
  Test.add_func ("/umockdev-run/invalid-ioctl", t_run_invalid_ioctl);

  // script replay
  Test.add_func ("/umockdev-run/script-chatter", t_run_script_chatter);

  // tests with gphoto2 program for PowerShot
  Test.add_func ("/umockdev-run/integration/gphoto-detect", t_gphoto_detect);
  Test.add_func ("/umockdev-run/integration/gphoto-folderlist", t_gphoto_folderlist);
  Test.add_func ("/umockdev-run/integration/gphoto-filelist", t_gphoto_filelist);

  // input devices
  Test.add_func ("/umockdev-run/integration/input-touchpad", t_input_touchpad);
  Test.add_func ("/umockdev-run/integration/input-evtest", t_input_evtest);

  return Test.run();
}
