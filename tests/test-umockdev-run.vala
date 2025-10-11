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

using UMockdevUtils;
using Assertions;

#if HAVE_SELINUX
using Selinux;
#endif

const string umockdev_run_command = "env LC_ALL=C umockdev-run ";
const string umockdev_record_command = "env LC_ALL=C umockdev-record ";

string rootdir;
string tests_dir;

int slow_testbed_factor = 1;

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
checked_file_set_contents (string filename, string contents)
{
    try {
        FileUtils.set_contents (filename, contents);
    } catch (FileError e) {
        error ("Failed to set %s contents: %s", filename, e.message);
    }
}

static void
assert_in (string needle, string haystack)
{
    if (!haystack.contains (needle)) {
        error ("'%s' not found in '%s'", needle, haystack);
    }
}

static bool
have_program (string program)
{
    string sout;
    int exit;

    try {
        Process.spawn_command_line_sync ("sh -c 'type " + program + "'", out sout, null, out exit);
    } catch (SpawnError e) {
        error ("cannot call type %s: %s", program, e.message);
    }

    return exit == 0;
}

static bool
get_program_out (string program, string command, out string sout,
                 out string serr, out int exit)
{
    if (!have_program (program)) {
        stdout.printf ("[SKIP: %s not installed] ", program);
        sout = "";
        serr = "";
        exit = -1;
        return false;
    }

    try {
        Process.spawn_command_line_sync (command, out sout, out serr, out exit);
    } catch (SpawnError e) {
        error ("cannot call %s: %s", command, e.message);
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

    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpstr (sout, CompareOperator.EQ, expected_out);
    assert_cmpint (exit, CompareOperator.EQ, 0);
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

    assert_cmpint (exit, CompareOperator.NE, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "");
}

static void
t_run_exit_code ()
{
    string sout, serr;
    int exit;

    // normal exit, zero
    check_program_out ("true", "true", "");

    // normal exit, nonzero
    get_program_out ("ls", umockdev_run_command + "ls /nonexisting", out sout, out serr, out exit);
    assert (Process.if_exited (exit));
    assert_cmpint (Process.exit_status (exit), CompareOperator.GT, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert_cmpstr (serr, CompareOperator.NE, "");

    // signal exit
    get_program_out ("sh", umockdev_run_command + "-- sh -c 'kill -SEGV $$'", out sout, out serr, out exit);
    assert (Process.if_signaled (exit));
    assert_cmpint (Process.term_sig (exit), CompareOperator.EQ, ProcessSignal.SEGV);
    assert_cmpstr (sout, CompareOperator.EQ, "");
}

static void
t_run_version ()
{
    // missing program to run
    check_program_out ("true", "--version", Config.VERSION + "\n");
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

    assert_cmpstr (sout, CompareOperator.EQ, "hello\n");
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
}

static void
t_run_udevadm_block ()
{
    string umockdev_file;

    Posix.close (checked_open_tmp ("loop23.XXXXXX.umockdev", out umockdev_file));

    checked_file_set_contents (umockdev_file, """P: /devices/virtual/block/loop23
N: loop23
E: DEVNAME=/dev/loop23
E: __DEVCONTEXT=system_u:object_r:fixed_disk_device_t:s0
E: DEVTYPE=disk
E: MAJOR=7
E: MINOR=23
E: SUBSYSTEM=block
A: dev=7:23\n
A: size=1048576\n
""");

    string sout;
    string serr;
    int exit;

    // unfortunately the udevadm output between distros is not entirely constant
    assert (get_program_out (
            "udevadm",
            umockdev_run_command + "-d " + umockdev_file + " -- udevadm info --query=all --name=/dev/loop23",
            out sout, out serr, out exit));

    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert (sout.contains ("P: /devices/virtual/block/loop23\n"));
    assert (sout.contains ("P: /devices/virtual/block/loop23\n"));
    assert (sout.contains ("E: DEVPATH=/devices/virtual/block/loop23"));
    assert (sout.contains ("E: DEVNAME=/dev/loop23"));
    assert (sout.contains ("E: MAJOR=7"));
    assert (sout.contains ("E: MINOR=23"));

    // correct type wihtout the internally used sticky bit
    check_program_out("true", "-d " + umockdev_file + " -- stat -c %A /dev/loop23",
                        "brw-r--r--\n");

#if HAVE_SELINUX
    // we may run on a system without SELinux
    try {
        Process.spawn_command_line_sync ("command -v selinuxenabled", null, null, out exit);
    } catch (SpawnError e) {
        exit = 1;
    }
    if (exit == 0) {
        check_program_out("true", "-d " + umockdev_file + " -- stat -c %C /dev/loop23",
                          "system_u:object_r:fixed_disk_device_t:s0\n");
    } else {
        stdout.printf ("[SKIP selinux context check: SELinux not active] ");
    }
#else
        stdout.printf ("[SKIP selinux context check: not built with SELinux support] ");
#endif

    checked_remove (umockdev_file);
}

static void
t_run_invalid_args ()
{
    // missing program to run
    check_program_error ("true", "", "--help");

    // unknown option
    check_program_error ("true", "--foobarize", "--help");
}

static void
t_run_invalid_device ()
{
    // nonexisting device file
    check_program_error ("true", "-d non.existing", "Cannot open non.existing:");

    // invalid device file
    string umockdev_file;
    Posix.close (checked_open_tmp ("ttyS0.XXXXXX.umockdev", out umockdev_file));
    checked_file_set_contents (umockdev_file, "P: /devices/foo\n");

    check_program_error ("true", "-d " + umockdev_file, "Invalid record file " +
                            umockdev_file + ": missing SUBSYSTEM");
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
t_run_invalid_script ()
{
    // wrongly formatted option
    check_program_error ("true", "-d " + rootdir +
        "/devices/cameras/canon-powershot-sx200.umockdev -s " +
        "/dev/bus/usb/001/011 -- true",
        "--script argument must be");

    // unsuitable device for scripts
    check_program_error ("true", "-d " + rootdir +
        "/devices/cameras/canon-powershot-sx200.umockdev -s " +
        "/dev/bus/usb/001/011=/etc/passwd -- true",
        "not a device suitable for scripts");

    // nonexisting script
    check_program_error ("true", "-d " + rootdir +
        "/devices/input/usbkbd.umockdev -s " +
        "/dev/input/event5=/non/existing -- true",
        "Cannot install /non/existing for device /dev/input/event5:");

    // wrongly formatted -u option
    check_program_error ("true", "-u /dev/mysock -- true",
        "--unix-stream argument must be");

    // invalid socket name
    /* FIXME: Fails on Debian sparc buildd
    check_program_error ("true",
        "-u ../../../../../../../null/mysock=/nosuch.script -- true",
        "annot create");
    */
}

static void
t_run_invalid_program ()
{
    check_program_error ("true", "no.such.prog",
        "Cannot run no.such.prog: Failed to execute");
}

static void
t_run_record_null ()
{
    string umockdev_file;
    string sout;
    string serr;
    int exit;

    if (!FileUtils.test("/sys/dev/char/1:3", FileTest.EXISTS)) {
        stdout.printf ("[SKIP: no real /sys on this system] ");
        stdout.flush ();
        return;
    }

    // stat or other programs segfault under Gentoo's sandbox in umockdev
    if (Environ.get_variable(Environ.get(), "SANDBOX_ON") == "1") {
        stdout.printf ("[SKIP: crashes in Gentoo's sandbox] ");
        stdout.flush ();
        return;
    }

    Posix.close (checked_open_tmp ("null.XXXXXX.umockdev", out umockdev_file));
    assert (get_program_out ("true", umockdev_record_command + "/dev/null", out sout, out serr, out exit));
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    checked_file_set_contents (umockdev_file, sout);

    check_program_out("true", "-d " + umockdev_file + " -- stat -c '%n %F %t %T' /dev/null",
                      "/dev/null character special file 1 3\n");

#if HAVE_SELINUX
    // we may run on a system without SELinux
    try {
        Process.spawn_command_line_sync ("command -v selinuxenabled", null, null, out exit);
    } catch (SpawnError e) {
        exit = 1;
    }
    if (exit == 0) {
        string orig_context;
        assert_cmpint (Selinux.lgetfilecon ("/dev/null", out orig_context), CompareOperator.GT, 0);
        check_program_out("true", "-d " + umockdev_file + " -- stat -c %C /dev/null", orig_context + "\n");
    } else {
        stdout.printf ("[SKIP selinux context check: SELinux not active] ");
    }
#else
        stdout.printf ("[SKIP selinux context check: not built with SELinux support] ");
#endif

    checked_remove (umockdev_file);
}

static void
t_run_script_chatter ()
{
    string umockdev_file, script_file;

    // create umockdev and script files
    Posix.close (checked_open_tmp ("ttyS0.XXXXXX.umockdev", out umockdev_file));
    Posix.close (checked_open_tmp ("chatter.XXXXXX.script", out script_file));

    checked_file_set_contents (umockdev_file, """P: /devices/platform/serial8250/tty/ttyS0
N: ttyS0
E: DEVNAME=/dev/ttyS0
E: SUBSYSTEM=tty
A: dev=4:64""");

    checked_file_set_contents (script_file, """w 0 Hello world!^JWhat is your name?^J
r 300 Joe Tester^J
w 0 I ♥ Joe Tester^Ja^I tab and a^J   line break in one write^J
r 200 somejunk^J
w 0 bye!^J""");

    check_program_out ("true", "-d " + umockdev_file + " -s /dev/ttyS0=" + script_file +
                       " -- " + tests_dir + "/chatter /dev/ttyS0",
                       "Got input: Joe Tester\nGot input: somejunk\n");

    checked_remove (umockdev_file);
    checked_remove (script_file);
}

static void
t_run_script_chatter_socket_stream ()
{
    string script_file;

    // create umockdev and script files
    Posix.close (checked_open_tmp ("chatter.XXXXXX.script", out script_file));

    checked_file_set_contents (script_file, """w 0 What is your name?^J
r 307 Joe Tester^J
w 0 hello Joe Tester^J
w 20 send()
r 30 somejunk""");

    check_program_out ("true", " -u /dev/socket/chatter=" + script_file +
                       " -- " + tests_dir + "/chatter-socket-stream /dev/socket/chatter",
                       "Got name: Joe Tester\n\nGot recv: somejunk\n");

    checked_remove (script_file);
}

static void
t_gphoto_detect ()
{
    check_program_out ("gphoto2",
        "-d " + rootdir + "/devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=" +
        rootdir + "/devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 --auto-detect",
        "Model                          Port            \n" +
        "----------------------------------------------------------\n" +
        "Canon PowerShot SX200 IS       usb:001,011     \n");
}

/*
   broken: URB structure apparently got more flexible a while ago; triggers assertion about submit_node != NULL

static bool
check_gphoto_version ()
{
    string sout;
    string serr;
    int exit;

    if (!get_program_out ("gphoto2", "gphoto2 --version", out sout, out serr, out exit))
        return false;
    string[] words = sout.split(" ", 3);
    if (words.length < 2)
        return false;

    if (double.parse (words[1]) < 2.5) {
        stdout.printf ("[SKIP: needs gphoto >= 2.5] ");
        return false;
    }

    return true;
}

static void
t_gphoto_folderlist ()
{
    if (!check_gphoto_version ())
        return;

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
    if (!check_gphoto_version ())
        return;

    check_program_out ("gphoto2",
        "-d " + rootdir + "/devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=" +
            rootdir + "/devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 -L",
        """There is no file in folder '/'.
There is no file in folder '/store_00010001'.
There is no file in folder '/store_00010001/DCIM'.
There are 2 files in folder '/store_00010001/DCIM/100CANON'.
#1     IMG_0001.JPG               rd    67 KB  640x480  image/jpeg
#2     IMG_0002.JPG               rd    88 KB  640x480  image/jpeg
""");
}

static void
t_gphoto_thumbs ()
{
    string sout;
    string serr;
    int exit;

    if (!check_gphoto_version ())
        return;

    get_program_out ("gphoto2", umockdev_run_command + "-d " + rootdir +
            "/devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=" +
            rootdir + "/devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 -T",
            out sout, out serr, out exit);

    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_in ("thumb_IMG_0001.jpg", sout);
    assert_in ("thumb_IMG_0002.jpg", sout);

    Posix.Stat st;
    assert (Posix.stat("thumb_IMG_0001.jpg", out st) == 0);
    assert_cmpuint ((uint) st.st_size, CompareOperator.GT, 500);
    assert (Posix.stat("thumb_IMG_0002.jpg", out st) == 0);
    assert_cmpuint ((uint) st.st_size, CompareOperator.GT, 500);

    checked_remove ("thumb_IMG_0001.jpg");
    checked_remove ("thumb_IMG_0002.jpg");
}
static void
t_gphoto_download ()
{
    string sout;
    string serr;
    int exit;

    if (!check_gphoto_version ())
        return;

    get_program_out ("gphoto2", umockdev_run_command + "-d " + rootdir +
            "/devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=" +
            rootdir + "/devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 -P",
            out sout, out serr, out exit);

    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_in ("IMG_0001.JPG", sout);
    assert_in ("IMG_0002.JPG", sout);

    Posix.Stat st;
    assert (Posix.stat("IMG_0001.JPG", out st) == 0);
    assert_cmpuint ((uint) st.st_size, CompareOperator.GT, 5000);
    assert (Posix.stat("IMG_0002.JPG", out st) == 0);
    assert_cmpuint ((uint) st.st_size, CompareOperator.GT, 5000);

    checked_remove ("IMG_0001.JPG");
    checked_remove ("IMG_0002.JPG");
}

*/

static void
t_input_touchpad ()
{
    if (BYTE_ORDER == ByteOrder.BIG_ENDIAN) {
        stdout.printf ("[SKIP: this test only works on little endian machines] ");
        return;
    }
    if (!have_program ("Xorg")) {
        stdout.printf ("[SKIP: Xorg not installed] ");
        return;
    }

    if (long.MAX != int64.MAX) {
        stdout.printf ("[SKIP: test only works on 64 bit architectures] ");
        return;
    }

    Pid xorg_pid;
    string logfile;
    Posix.close (checked_open_tmp ("Xorg.log.XXXXXX", out logfile));
    try {
        Process.spawn_async (null, {"umockdev-run",
            "-d", rootdir + "/devices/input/synaptics-touchpad.umockdev",
            "-i", "/dev/input/event12=" + rootdir + "/devices/input/synaptics-touchpad.ioctl",
            "--", "Xorg", "-config", rootdir + "/tests/xorg-dummy.conf", "-logfile", logfile, ":5"},
            null, SpawnFlags.SEARCH_PATH | SpawnFlags.STDERR_TO_DEV_NULL, null, out xorg_pid);
    } catch (SpawnError e) {
        error ("cannot call Xorg: %s", e.message);
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
        stderr.printf ("SKIP: Xorg failed to start up; please ensure you have the X.org dummy driver installed, and check the log file: %s\n", logfile);
        return;
    }

    /* call xinput */
    string xinput_out, xinput_err;
    int xinput_exit;
    get_program_out ("xinput", "env DISPLAY=:5 xinput", out xinput_out, out xinput_err, out xinput_exit);

    string props_out, props_err;
    int props_exit;
    get_program_out ("xinput", "env DISPLAY=:5 xinput --list-props 'SynPS/2 Synaptics TouchPad'",
            out props_out, out props_err, out props_exit);

    /* shut down X; this requires extra force due to https://launchpad.net/bugs/1853266 */
#if VALA_0_40
    Posix.kill (xorg_pid, Posix.Signal.TERM);
    Posix.kill (xorg_pid, Posix.Signal.QUIT);
    Posix.kill (xorg_pid, Posix.Signal.KILL);
#else
    Posix.kill (xorg_pid, Posix.SIGTERM);
    Posix.kill (xorg_pid, Posix.SIGQUIT);
    Posix.kill (xorg_pid, Posix.SIGKILL);
#endif
    try {
        Process.spawn_sync (null, {"pkill", "-9", "-f", "Xorg.*/tests/xorg-dummy.conf"}, null, SpawnFlags.SEARCH_PATH, null, null, null, null);
    } catch (Error e) {}

    int status;
    Posix.waitpid (xorg_pid, out status, 0);
    Process.close_pid (xorg_pid);
    checked_remove (logfile);
    checked_remove (logfile + ".old");
    // clean up lockfile after killed X server
    if (FileUtils.remove ("/tmp/.X5-lock") < 0)
        debug("failed to clean up /tmp/.X5-lock: %m");
    if (FileUtils.remove ("/tmp/.X11-unix/X5") < 0)
        debug("failed to clean up .X11-unix/X5: %m");

    assert_cmpstr (xinput_err, CompareOperator.EQ, "");
    assert_cmpint (xinput_exit, CompareOperator.EQ, 0);
    assert_in ("SynPS/2 Synaptics TouchPad", xinput_out);

    assert_cmpstr (props_err, CompareOperator.EQ, "");
    assert_cmpint (props_exit, CompareOperator.EQ, 0);
    assert_in ("Synaptics Two-Finger Scrolling", props_out);
    assert_in ("/dev/input/event12", props_out);
}

static void
t_input_evtest ()
{
    if (BYTE_ORDER == ByteOrder.BIG_ENDIAN) {
        stdout.printf ("[SKIP: this test only works on little endian machines] ");
        return;
    }

    if (!have_program ("evtest")) {
        stdout.printf ("[SKIP: evtest not installed] ");
        return;
    }

    unowned string? preload = Environment.get_variable ("LD_PRELOAD");
    if (preload != null && preload.contains ("vgpreload")) {
        stdout.printf ("[SKIP: this test does not work under valgrind] ");
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
        error ("cannot call evtest: %s", e.message);
    }

    // our script covers 1.4 seconds, give it some slack
    Posix.sleep (2 * slow_testbed_factor);
#if VALA_0_40
    Posix.kill (evtest_pid, Posix.Signal.TERM);
#else
    Posix.kill (evtest_pid, Posix.SIGTERM);
#endif
    var sout = new uint8[10000];
    var serr = new uint8[10000];
    ssize_t sout_len = Posix.read (outfd, sout, sout.length);
    ssize_t serr_len = Posix.read (errfd, serr, sout.length);
    int status;
    Posix.waitpid (evtest_pid, out status, 0);
    Process.close_pid (evtest_pid);

    if (serr_len > 0) {
        serr[serr_len] = 0;
        error ("evtest error: %s", (string) serr);
    }

    assert_cmpint ((int) sout_len, CompareOperator.GT, 10);
    sout[sout_len] = 0;
    string output = (string) sout;

    // check supported events
    assert_in ("Event", output);
    assert_in ("type 1 (EV_KEY)", output);
    assert_in ("code 42 (KEY_LEFTSHIFT)", output);

    // check 'A' key event
    assert_in ("type 4 (EV_MSC), code 4 (MSC_SCAN), value 70004", output);
    assert_in ("type 1 (EV_KEY), code 30 (KEY_A), value 1\n", output);
    assert_in ("type 1 (EV_KEY), code 30 (KEY_A), value 0\n", output);

    // check 'left shift' key event
    assert_in ("type 4 (EV_MSC), code 4 (MSC_SCAN), value 700e1", output);
    assert_in ("type 1 (EV_KEY), code 42 (KEY_LEFTSHIFT), value 1\n", output);
    assert_in ("type 1 (EV_KEY), code 42 (KEY_LEFTSHIFT), value 0\n", output);
}

static void
t_input_evtest_evemu ()
{
    if (BYTE_ORDER == ByteOrder.BIG_ENDIAN) {
        stdout.printf ("[SKIP: this test only works on little endian machines] ");
        return;
    }

    if (!have_program ("evtest")) {
        stdout.printf ("[SKIP: evtest not installed] ");
        return;
    }

    unowned string? preload = Environment.get_variable ("LD_PRELOAD");
    if (preload != null && preload.contains ("vgpreload")) {
        stdout.printf ("[SKIP: this test does not work under valgrind] ");
        return;
    }

    Pid evtest_pid;
    int outfd, errfd;

    // create evemu events file
    string evemu_file;
    Posix.close (checked_open_tmp ("evemu.XXXXXX.events", out evemu_file));
    checked_file_set_contents (evemu_file,
"""E: 0.000000 0000 0000 0000	# ------------ SYN_REPORT (0) ----------
E: 0.200000 0004 0004 458756	# EV_MSC / MSC_SCAN             458756
E: 0.200000 0001 001e 0001	# EV_KEY / KEY_A                1
E: 0.200000 0000 0000 0000	# ------------ SYN_REPORT (0) ----------
E: 0.500000 0004 0004 458756	# EV_MSC / MSC_SCAN             458756
E: 0.500000 0001 001e 0000	# EV_KEY / KEY_A                0
""");

    try {
        Process.spawn_async_with_pipes (null, {"umockdev-run",
            "-d", rootdir + "/devices/input/usbkbd.umockdev",
            "-i", "/dev/input/event5=" + rootdir + "/devices/input/usbkbd.evtest.ioctl",
            "-e", "/dev/input/event5=" + evemu_file,
            "evtest", "/dev/input/event5"},
            null, SpawnFlags.SEARCH_PATH, null,
            out evtest_pid, null, out outfd, out errfd);
    } catch (SpawnError e) {
        error ("cannot call evtest: %s", e.message);
    }

    // our script covers 0.5 seconds, give it some slack
    Posix.sleep (1 * slow_testbed_factor);
    checked_remove (evemu_file);
#if VALA_0_40
    Posix.kill (evtest_pid, Posix.Signal.TERM);
#else
    Posix.kill (evtest_pid, Posix.SIGTERM);
#endif
    var sout = new uint8[10000];
    var serr = new uint8[10000];
    ssize_t sout_len = Posix.read (outfd, sout, sout.length);
    ssize_t serr_len = Posix.read (errfd, serr, sout.length);
    int status;
    Posix.waitpid (evtest_pid, out status, 0);
    Process.close_pid (evtest_pid);

    if (serr_len > 0) {
        serr[serr_len] = 0;
        error ("evtest error: %s", (string) serr);
    }

    assert_cmpint ((int) sout_len, CompareOperator.GT, 10);
    sout[sout_len] = 0;
    string output = (string) sout;

    // this can be followed by SYN_REPORT or EV_SYN depending on the evtest
    // version
    assert_in ("Event: time 0.000000, -------------- ", output);
    assert_in ("""Event: time 0.200000, type 4 (EV_MSC), code 4 (MSC_SCAN), value 70004
Event: time 0.200000, type 1 (EV_KEY), code 30 (KEY_A), value 1
""", output);
    assert_in ("Event: time 0.200000, -------------- ", output);
    assert_in ("""Event: time 0.500000, type 4 (EV_MSC), code 4 (MSC_SCAN), value 70004
Event: time 0.500000, type 1 (EV_KEY), code 30 (KEY_A), value 0
""", output);
}

int
main (string[] args)
{
  Test.init (ref args);

  string? f = Environment.get_variable ("SLOW_TESTBED_FACTOR");
  if (f != null && int.parse(f) > 0)
    slow_testbed_factor = int.parse(f);

  string? top_srcdir = Environment.get_variable ("TOP_SRCDIR");
  if (top_srcdir != null)
      rootdir = top_srcdir;
  else
      rootdir = ".";
  tests_dir = Path.get_dirname (args[0]);
    if (tests_dir.has_suffix (".libs")) // libtool hack
        tests_dir = Path.get_dirname (tests_dir);

  // general operations
  Test.add_func ("/umockdev-run/exit_code", t_run_exit_code);
  Test.add_func ("/umockdev-run/version", t_run_version);
  Test.add_func ("/umockdev-run/pipes", t_run_pipes);

  // udevadm emulation
  Test.add_func ("/umockdev-run/udevadm-block", t_run_udevadm_block);

  // error conditions
  Test.add_func ("/umockdev-run/invalid-args", t_run_invalid_args);
  Test.add_func ("/umockdev-run/invalid-device", t_run_invalid_device);
  Test.add_func ("/umockdev-run/invalid-ioctl", t_run_invalid_ioctl);
  Test.add_func ("/umockdev-run/invalid-script", t_run_invalid_script);
  Test.add_func ("/umockdev-run/invalid-program", t_run_invalid_program);

  // udevadm-record interaction
  Test.add_func ("/umockdev-run/umockdev-record-null-roundtrip", t_run_record_null);

  // script replay
  Test.add_func ("/umockdev-run/script-chatter", t_run_script_chatter);
  Test.add_func ("/umockdev-run/script-chatter-socket-stream", t_run_script_chatter_socket_stream);

  // tests with gphoto2 program for PowerShot
  Test.add_func ("/umockdev-run/integration/gphoto-detect", t_gphoto_detect);
  /*
  Test.add_func ("/umockdev-run/integration/gphoto-folderlist", t_gphoto_folderlist);
  Test.add_func ("/umockdev-run/integration/gphoto-filelist", t_gphoto_filelist);
  Test.add_func ("/umockdev-run/integration/gphoto-thumbs", t_gphoto_thumbs);
  Test.add_func ("/umockdev-run/integration/gphoto-download", t_gphoto_download);
  */

  // input devices
  Test.add_func ("/umockdev-run/integration/input-touchpad", t_input_touchpad);
  Test.add_func ("/umockdev-run/integration/input-evtest", t_input_evtest);
  Test.add_func ("/umockdev-run/integration/input-evtest-evemu", t_input_evtest_evemu);

  return Test.run();
}
