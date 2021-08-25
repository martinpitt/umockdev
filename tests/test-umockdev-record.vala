/*
 * test-umockdev-record.vala
 *
 * Copyright (C) 2013 Canonical Ltd.
 * Copyright (C) 2018 Martin Pitt
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

string readbyte_path;
string tests_dir;

int slow_testbed_factor = 1;

// wrappers to avoid "unhandled error" warnings

static int
checked_open_tmp (string tmpl, out string name_used) {
    try {
        return FileUtils.open_tmp (tmpl, out name_used);
    } catch (Error e) {
        error ("Failed to open temporary file: %s", e.message);
    }
}

void
spawn (string command, out string sout, out string serr, out int exit)
{
    try {
        assert (Process.spawn_command_line_sync (command, out sout, out serr, out exit));
    } catch (SpawnError e) {
        error ("Cannot call '%s': %s", command, e.message);
    }
}

static void
assert_in (string needle, string haystack)
{
    if (!haystack.contains (needle)) {
        error ("'%s' not found in '%s'", needle, haystack);
    }
}

string
file_contents (string filename)
{
    string contents;
    try {
        assert (FileUtils.get_contents (filename, out contents));
    } catch (FileError e) {
        error ("Cannot get contents of %s: %s", filename, e.message);
    }
    return contents;
}

// --all on empty testbed
static void
t_testbed_all_empty ()
{
    string sout;
    string serr;
    int exit;

    var tb = new UMockdev.Testbed ();
    assert (tb != null);

    spawn ("umockdev-record" + " --all", out sout, out serr, out exit);

    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
}

// one device
static void
t_testbed_one ()
{
    string sout;
    string serr;
    int exit;

    var tb = new UMockdev.Testbed ();
    var syspath = tb.add_devicev ("pci", "dev1", null,
                                  {"simple_attr", "1",
                                   "multiline_attr", "a\\b\nc\\d\nlast",
                                   "knobs/red", "off"},
                                  {"SIMPLE_PROP", "1"});
    tb.set_attribute_binary (syspath, "binary_attr", {0x41, 0xFF, 0, 5, 0xFF, 0});
    tb.set_attribute_link (syspath, "driver", "../../drivers/foo");
    tb.set_attribute_link (syspath, "power/fiddle", "../some/where");

    spawn ("umockdev-record" + " --all", out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, """P: /devices/dev1
E: SIMPLE_PROP=1
E: SUBSYSTEM=pci
H: binary_attr=41FF0005FF00
L: driver=../../drivers/foo
A: knobs/red=off
A: multiline_attr=a\\b\nc\\d\nlast
L: power/fiddle=../some/where
A: simple_attr=1

""");
}

// multiple devices
static void
t_testbed_multiple ()
{
    string sout;
    string serr;
    int exit;

    var tb = new UMockdev.Testbed ();
    var dev1 = tb.add_devicev ("pci", "dev1", null,
                               {"dev1color", "green", "knobs/red", "off"},
                               {"DEV1COLOR", "GREEN"});
    var subdev1 = tb.add_devicev ("pci", "subdev1", dev1, {"subdev1color", "yellow"},
                                  {"SUBDEV1COLOR", "YELLOW"});
    tb.add_devicev ("pci", "dev2", null, {"dev2color", "brown"}, {"DEV2COLOR", "BROWN"});

    // should grab device and all parents
    spawn ("umockdev-record" + " " + subdev1, out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, """P: /devices/dev1/subdev1
E: SUBDEV1COLOR=YELLOW
E: SUBSYSTEM=pci
A: subdev1color=yellow

P: /devices/dev1
E: DEV1COLOR=GREEN
E: SUBSYSTEM=pci
A: dev1color=green
A: knobs/red=off

""");

    // only dev1
    spawn ("umockdev-record" + " " + dev1, out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, """P: /devices/dev1
E: DEV1COLOR=GREEN
E: SUBSYSTEM=pci
A: dev1color=green
A: knobs/red=off

""");

    // with --all it should have all three
    spawn ("umockdev-record" + " --all", out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert (sout.contains ("P: /devices/dev1/subdev1\n"));
    assert (sout.contains ("P: /devices/dev1\n"));
    assert (sout.contains ("P: /devices/dev2\n"));
}


static void
t_system_single ()
{
    string sout;
    string serr;
    int exit;

    if (!FileUtils.test("/sys/dev/char/1:3", FileTest.EXISTS)) {
        stdout.printf ("[SKIP: no real /sys on this system] ");
        stdout.flush ();
        return;
    }

    spawn ("umockdev-record" + " /dev/null /dev/zero", out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_in("E: DEVNAME=/dev/null", sout);
    assert_in("P: /devices/virtual/mem/null", sout);
    assert_in("E: DEVNAME=/dev/zero", sout);
}

// system /sys: umockdev-record --all works and result loads back
static void
t_system_all ()
{
    string sout;
    string serr;
    int exit;

    if (!FileUtils.test("/sys/dev/char", FileTest.EXISTS)) {
        stdout.printf ("[SKIP: no real /sys on this system] ");
        stdout.flush ();
        return;
    }
    if (Environment.get_variable ("INSTALLED_TEST") != null) {
        stdout.printf ("[SKIP: brittle test] ");
        stdout.flush ();
        return;
    }

    spawn ("umockdev-record" + " --all", out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert (sout.has_prefix ("P:"));
    assert_cmpint (sout.length, CompareOperator.GE, 100);

    var tb = new UMockdev.Testbed ();
    try {
        assert (tb.add_from_string (sout));
    } catch (UMockdev.Error e) {
        error ("Error when adding system dump to testbed: %s", e.message);
    }
}

static void
t_system_invalid ()
{
    string sout;
    string serr;
    int exit;

    if (!FileUtils.test("/sys/block/loop0", FileTest.EXISTS)) {
        stdout.printf ("[SKIP: no real /sys on this system] ");
        stdout.flush ();
        return;
    }

    spawn ("umockdev-record" + " /sys/class", out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "Invalid device /sys/class, has no uevent attribute\n");
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.NE, 0);

    spawn ("umockdev-record" + " /sys/block/loop0/size", out sout, out serr, out exit);
    // the real path might vary
    assert (serr.contains ("Invalid device"));
    assert (serr.contains ("/block/loop0/size"));
    assert (serr.contains ("has no uevent attribute"));
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.NE, 0);
}

/*
 * umockdev-record --ioctl recording to a file
 *
 * Note that this cannot test actual ioctls, as we cannot rely on an USB device
 * which accepts the ioctls that we can handle; but we can at least ensure that
 * the preload lib gets loaded, the log gets created, and recording happens on
 * the right device.
 */
static void
t_system_ioctl_log ()
{
    string sout;
    string serr;
    int exit;

    string workdir;
    try {
        workdir = DirUtils.make_tmp ("ioctl_log_test.XXXXXX");
    } catch (Error e) {
        error ("Failed to make temporary dir: %s", e.message);
    }
    string log = Path.build_filename (workdir, "log");

    // should not log anything as that device is not touched
    spawn ("umockdev-record" + " --ioctl=/dev/null=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    assert (!FileUtils.test (log, FileTest.EXISTS));

    // this should create a log
    spawn ("umockdev-record" + " --ioctl /dev/zero=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    assert (FileUtils.test (log, FileTest.EXISTS));
    assert_cmpstr (file_contents (log), CompareOperator.EQ, "@DEV /dev/zero\n");

    FileUtils.remove (log);

    // invalid syntax
    spawn ("umockdev-record" + " --ioctl /dev/null -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpint (exit, CompareOperator.NE, 1);
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert (serr.contains ("--ioctl"));
    assert (serr.contains ("="));
    assert (!FileUtils.test (log, FileTest.EXISTS));

    DirUtils.remove (workdir);
}

static void
t_system_ioctl_log_append_dev_mismatch ()
{
    string sout;
    string serr;
    int exit;
    string log;

    FileUtils.close(checked_open_tmp ("ioctl_log_test.XXXXXX", out log));

    // should log the header plus one read
    spawn ("umockdev-record" + " -i /dev/zero=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");

    string orig_contents = file_contents (log);

    // should fail as it is a different device
    spawn ("umockdev-record" + " -i /dev/null=" + log + " -- " + readbyte_path + " /dev/null",
           out sout, out serr, out exit);
    assert (serr.contains ("two different devices"));
    assert_cmpint (exit, CompareOperator.NE, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");

    // should not change original record
    assert_cmpstr (file_contents (log), CompareOperator.EQ, orig_contents);

    FileUtils.remove (log);
}

/*
 * umockdev-record --script recording to a file, with simple "readbyte" command
 */
static void
t_system_script_log_simple ()
{
    string sout;
    string serr;
    int exit;
    string log;

    FileUtils.close(checked_open_tmp ("test_script_log.XXXXXX", out log));

    // should not log anything as that device is not touched
    spawn ("umockdev-record" + " --script=/dev/null=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    assert_cmpstr (file_contents (log), CompareOperator.EQ, "");

    // should log the header plus one read
    spawn ("umockdev-record" + " --script=/dev/zero=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    string[] loglines = file_contents (log).split("\n");
    assert_cmpuint (loglines.length, CompareOperator.EQ, 2);
    assert_cmpstr (loglines[0], CompareOperator.EQ, "d 0 /dev/zero");

    string[] logwords = loglines[1].split(" ");
    assert_cmpuint (logwords.length, CompareOperator.EQ, 3);
    assert_cmpstr (logwords[0], CompareOperator.EQ, "r");
    // should be quick, give it 5 ms at most
    assert_cmpint (int.parse(logwords[1]), CompareOperator.LE, 5 * slow_testbed_factor);
    assert_cmpstr (logwords[2], CompareOperator.EQ, "^@");

    FileUtils.remove (log);
}

/*
 * umockdev-record --script recording to a file, with simple "readbyte" command in fopen mode
 * It would be so much more elegant to use Test.add_data_func() and re-use the
 * previous function, but this is broken: https://gitlab.gnome.org/GNOME/vala/issues/525
 */
static void
t_system_script_log_simple_fopen ()
{
    string sout;
    string serr;
    int exit;
    string log;

    FileUtils.close(checked_open_tmp ("test_script_log.XXXXXX", out log));

    // should not log anything as that device is not touched
    spawn ("umockdev-record" + " --script=/dev/null=" + log + " -- " + readbyte_path + " /dev/zero fopen",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    assert_cmpstr (file_contents (log), CompareOperator.EQ, "");

    // should log the header plus one read
    spawn ("umockdev-record" + " --script=/dev/zero=" + log + " -- " + readbyte_path + " /dev/zero fopen",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    string[] loglines = file_contents (log).split("\n");
    assert_cmpuint (loglines.length, CompareOperator.EQ, 2);
    assert_cmpstr (loglines[0], CompareOperator.EQ, "d 0 /dev/zero");

    string[] logwords = loglines[1].split(" ");
    assert_cmpuint (logwords.length, CompareOperator.EQ, 3);
    assert_cmpstr (logwords[0], CompareOperator.EQ, "r");
    // should be quick, give it 5 ms at most
    assert_cmpint (int.parse(logwords[1]), CompareOperator.LE, 5 * slow_testbed_factor);
    assert_cmpstr (logwords[2], CompareOperator.EQ, "^@");

    FileUtils.remove (log);
}

static void
t_system_script_log_append_same_dev ()
{
    string sout;
    string serr;
    int exit;
    string log;

    FileUtils.close(checked_open_tmp ("test_script_log.XXXXXX", out log));

    // should log the header plus one read
    spawn ("umockdev-record" + " --script=/dev/zero=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");

    // should still work as it is the same device, and append
    spawn ("umockdev-record" + " --script=/dev/zero=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");

    // should now have the header and two reads
    string[] loglines = file_contents (log).split("\n");
    assert_cmpuint (loglines.length, CompareOperator.EQ, 3);
    assert_cmpstr (loglines[0], CompareOperator.EQ, "d 0 /dev/zero");

    string[] logwords = loglines[1].split(" ");
    assert_cmpuint (logwords.length, CompareOperator.EQ, 3);
    assert_cmpstr (logwords[0], CompareOperator.EQ, "r");
    // should be quick, give it 5 ms at most
    assert_cmpint (int.parse(logwords[1]), CompareOperator.LE, 5 * slow_testbed_factor);
    assert_cmpstr (logwords[2], CompareOperator.EQ, "^@");

    logwords = loglines[2].split(" ");
    assert_cmpuint (logwords.length, CompareOperator.EQ, 3);
    assert_cmpstr (logwords[0], CompareOperator.EQ, "r");
    // should be quick, give it 5 ms at most
    assert_cmpint (int.parse(logwords[1]), CompareOperator.LE, 5 * slow_testbed_factor);
    assert_cmpstr (logwords[2], CompareOperator.EQ, "^@");

    FileUtils.remove (log);
}

static void
t_system_script_log_append_dev_mismatch ()
{
    string sout;
    string serr;
    int exit;
    string log;

    FileUtils.close(checked_open_tmp ("test_script_log.XXXXXX", out log));

    // should log the header plus one read
    spawn ("umockdev-record" + " --script=/dev/zero=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");

    string orig_contents = file_contents (log);

    // should fail as it is a different device
    spawn ("umockdev-record" + " --script=/dev/null=" + log + " -- " + readbyte_path + " /dev/null",
           out sout, out serr, out exit);
    assert (serr.contains ("two different devices"));
    assert_cmpint (exit, CompareOperator.EQ, 256);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");

    // should not change original record
    assert_cmpstr (file_contents (log), CompareOperator.EQ, orig_contents);

    FileUtils.remove (log);
}

static string
read_line_timeout(FileStream stream)
{
    string? line = null;
    char buffer[1000];
    int timeout = 50;

    while (timeout > 0) {
        line = stream.gets(buffer);
        if (line != null)
            return line;
        Thread.usleep(100000);
        timeout--;
    }

    assert(timeout > 0);
    return "<timeout>";
}

static ssize_t
read_buf_delay(ulong delay, Socket socket, uint8[] buffer, ssize_t length) throws Error
{
    ssize_t len = 0;
    int64 start;

    Thread.usleep (delay);
    start = get_monotonic_time();

    while (len < length && 5000000 > get_monotonic_time() - start) {
        unowned uint8[] buf = buffer[len:length];

        assert (socket.condition_timed_wait (IN, 5000000));
        len += socket.receive (buf);
    }

    assert (len == length);
    return len;
}

/*
 * umockdev-record --script recording to a file, with our chatter command
 */
static void
t_system_script_log_chatter ()
{
    string log;

    FileUtils.close(checked_open_tmp ("test_script_log.XXXXXX", out log));

    char[] ptyname = new char[8192];
    int ptym, ptys;
    assert (Linux.openpty (out ptym, out ptys, ptyname, null, null) == 0);
    Posix.close (ptys);

    // disable echo, canonical mode, and line ending translation
    Posix.termios ios;
    assert (Posix.tcgetattr (ptym, out ios) == 0);
    ios.c_iflag &= ~(Posix.IGNCR | Posix.INLCR | Posix.ICRNL);
    ios.c_oflag &= ~(Posix.ONLCR | Posix.OCRNL);
    ios.c_lflag &= ~(Posix.ICANON | Posix.ECHO);
    assert (Posix.tcsetattr (ptym, Posix.TCSANOW, ios) == 0);

    // start chatter
    Pid chatter_pid;
    try {
        assert (Process.spawn_async_with_pipes (null,
            {"umockdev-record", "--script", (string) ptyname + "=" + log, "--",
             Path.build_filename (tests_dir, "chatter"), (string) ptyname},
            null, SpawnFlags.SEARCH_PATH | SpawnFlags.DO_NOT_REAP_CHILD | SpawnFlags.STDOUT_TO_DEV_NULL,
            null, out chatter_pid, null, null, null));
    } catch (SpawnError e) {
        error ("Cannot call umockdev-record: %s", e.message);
    }

    var chatter_stream = FileStream.fdopen (ptym, "r+");
    assert (chatter_stream != null);

    // expect the first two lines
    assert_cmpstr (read_line_timeout (chatter_stream), CompareOperator.EQ, "Hello world!\n");
    assert_cmpstr (read_line_timeout (chatter_stream), CompareOperator.EQ, "What is your name?\n");

    // type name and second string after some delay
    Thread.usleep (500000);
    chatter_stream.puts ("John\n");

    while (!read_line_timeout (chatter_stream).contains ("line break in one write"));

    Thread.usleep (300000);
    chatter_stream.puts ("foo ☹ bar ^!\n");

    assert_cmpstr (read_line_timeout (chatter_stream), CompareOperator.EQ, "bye!\n");

    int status;
    assert_cmpint ((int) Posix.waitpid (chatter_pid, out status, 0), CompareOperator.EQ, (int) chatter_pid);
    assert_cmpint (status, CompareOperator.EQ, 0);

    // evaluate log
    var log_stream = FileStream.open (log, "r");
    char[] buf = new char[8192];
    int time = 0;
    assert_cmpint (log_stream.scanf ("d 0 %s\n", buf), CompareOperator.EQ, 1);
    assert_cmpstr ((string)buf, CompareOperator.EQ, (string)ptyname);
    assert_cmpint (log_stream.scanf ("w %d Hello world!^JWhat is your name?^J\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.LE, 20 * slow_testbed_factor);
    assert_cmpint (log_stream.scanf ("r %d John^J\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.GE, 450);
    assert_cmpint (time, CompareOperator.LE, 800 * slow_testbed_factor);
    assert_cmpint (log_stream.scanf ("w %d I ♥ John^Ja^I tab and a^J line break in one write^J\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.LE, 20 * slow_testbed_factor);
    assert_cmpint (log_stream.scanf ("r %d foo ☹ bar ^`!^J\n", &time), CompareOperator.EQ, 1);;
    assert_cmpint (time, CompareOperator.GE, 250);
    assert_cmpint (time, CompareOperator.LE, 450 * slow_testbed_factor);

    assert_cmpint (log_stream.scanf ("w %d bye!^J\n", &time), CompareOperator.EQ, 1);;
    assert_cmpint (time, CompareOperator.LE, 20 * slow_testbed_factor);

    // verify EOF
    assert_cmpint (log_stream.scanf ("%*c"), CompareOperator.EQ, -1);

    FileUtils.remove (log);
}

/*
 * umockdev-record --script recording to a file, with our chatter-socket-stream command
 */
static void
t_system_script_log_chatter_socket_stream ()
{
    string log;
    string spath = "/tmp/umockdev_test";
    Pid chatter_pid;

    try {
        FileUtils.close(FileUtils.open_tmp ("test_script_log.XXXXXX", out log));

        var s = new Socket (SocketFamily.UNIX, SocketType.STREAM, SocketProtocol.DEFAULT);
        assert (s != null);
        s.blocking = false;

        assert (s.bind (new UnixSocketAddress (spath), true));
        assert (s.listen ());

        // start chatter
        try {
            assert (Process.spawn_async_with_pipes (null,
                {"umockdev-record", "--script", spath + "=" + log, "--",
                 Path.build_filename (tests_dir, "chatter-socket-stream"), spath},
                null, SpawnFlags.SEARCH_PATH | SpawnFlags.DO_NOT_REAP_CHILD | SpawnFlags.STDOUT_TO_DEV_NULL,
                null, out chatter_pid, null, null, null));
        } catch (SpawnError e) {
            error ("Cannot call umockdev-record: %s", e.message);
        }

        // wait until chatter connects
        int timeout = 20;
        Socket conn = null;
        while (timeout > 0) {
            try {
                conn = s.accept ();
                break;
            } catch (IOError e) {
                if (e is IOError.WOULD_BLOCK) {
                    timeout--;
                    Thread.usleep(10000);
                } else
                    throw e;
            }
        }
        assert (conn != null);

        var buf = new uint8[1000];

        // expect the question
        ssize_t len = conn.receive (buf);
        assert (len > 0);
        buf[len] = 0;
        assert_cmpstr ((string) buf, CompareOperator.EQ, "What is your name?\n");

        // type name and second string after some delay
        Thread.usleep (300000);
        conn.send ("John\n".data);

        // give it some time for the response
        len = read_buf_delay (10000, conn, buf, 11);
        assert (len > 0);
        buf[len] = 0;
        assert_cmpstr ((string) buf, CompareOperator.EQ, "hello John\n");

        // check the send message
        len = read_buf_delay (20000, conn, buf, 6);
        assert (len > 0);
        buf[len] = 0;
        assert_cmpstr ((string) buf, CompareOperator.EQ, "send()");

        // send stuff for recv()
        Thread.usleep (20000);
        conn.send ("recv()".data);
    } catch (Error e) {
        FileUtils.remove (spath);
        error ("Error: %s", e.message);
    }
    FileUtils.remove (spath);

    int status;
    assert_cmpint ((int) Posix.waitpid (chatter_pid, out status, 0), CompareOperator.EQ, (int) chatter_pid);
    assert_cmpint (status, CompareOperator.EQ, 0);

    // evaluate log
    var log_stream = FileStream.open (log, "r");
    int time = 0;
    assert_cmpint (log_stream.scanf ("w %d What is your name?^J\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.LE, 20 * slow_testbed_factor);
    assert_cmpint (log_stream.scanf ("r %d John^J\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.GE, 250);
    assert_cmpint (time, CompareOperator.LE, 400 * slow_testbed_factor);
    assert_cmpint (log_stream.scanf ("w %d hello John^J\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.LE, 20 * slow_testbed_factor);
    assert_cmpint (log_stream.scanf ("w %d send()\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.GE, 10);
    assert_cmpint (log_stream.scanf ("r %d recv()\n", &time), CompareOperator.EQ, 1);
    assert_cmpint (time, CompareOperator.GE, 20);
    assert_cmpint (time, CompareOperator.LE, 60 * slow_testbed_factor);

    FileUtils.remove (log);
}

/*
 * umockdev-record --evemu-events recording to a file
 *
 * Note that this cannot test actual events, as we cannot inject events into a
 * device without root privileges; but we can at least ensure that the preload
 * lib gets loaded, the log gets created, the header written, and recording
 * happens on the right device.
 */
static void
t_system_evemu_log ()
{
    string sout;
    string serr;
    int exit;

    string workdir;
    try {
        workdir = DirUtils.make_tmp ("evemu_log_test.XXXXXX");
    } catch (Error e) {
        error ("Failed to make temporary dir: %s", e.message);
    }
    string log = Path.build_filename (workdir, "log");

    spawn ("umockdev-record" + " --evemu-events=/dev/null=" + log + " -- " + readbyte_path + " /dev/null",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    assert_cmpstr (file_contents (log), CompareOperator.EQ, "# EVEMU 1.2\n# device /dev/null\n");

    // appending a record for the same device should work
    spawn ("umockdev-record" + " --evemu-events=/dev/null=" + log + " -- " + readbyte_path + " /dev/null",
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    // appends an extra NL at the end
    assert_cmpstr (file_contents (log), CompareOperator.EQ, "# EVEMU 1.2\n# device /dev/null\n\n");

    // appending a record for a different device should fail
    spawn ("umockdev-record" + " --evemu-events=/dev/zero=" + log + " -- " + readbyte_path + " /dev/zero",
           out sout, out serr, out exit);
    assert (serr.contains ("two different devices"));
    assert_cmpint (exit, CompareOperator.EQ, 256);
    assert_cmpstr (sout, CompareOperator.EQ, "\0");
    // unchanged
    assert_cmpstr (file_contents (log), CompareOperator.EQ, "# EVEMU 1.2\n# device /dev/null\n\n");

    FileUtils.remove (log);

    // invalid syntax
    spawn ("umockdev-record" + " --evemu-events /dev/null -- true",
           out sout, out serr, out exit);
    assert_cmpint (exit, CompareOperator.NE, 1);
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert (serr.contains ("--ioctl"));
    assert (serr.contains ("="));
    assert (!FileUtils.test (log, FileTest.EXISTS));

    DirUtils.remove (workdir);
}
static void
t_run_invalid_args ()
{
    string sout;
    string serr;
    int exit;

    spawn ("umockdev-record" + " /dev/no/such/device", out sout, out serr, out exit);

    assert_in ("Cannot access device /dev/no/such/device: No such file", serr);
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.NE, 0);
}

static void
t_gphoto2_record ()
{
    string sout, sout_record;
    string serr;
    int exit;

    // check if we have gphoto2 and a camera
    spawn ("which gphoto2", out sout, out serr, out exit);
    if (exit != 0) {
        stdout.printf ("[SKIP: gphoto2 not installed] ");
        stdout.flush ();
        return;
    }
    spawn ("gphoto2 --auto-detect", out sout, out serr, out exit);
    if (exit != 0) {
        stdout.printf ("[SKIP: gphoto2 --auto-detect failed: %s] ", sout + serr);
        return;
    }

    // find bus and dev number
    Regex port_re;
    try {
        port_re = new Regex ("usb:([0-9]+),([0-9]+)");
    } catch (RegexError e) {
        error ("Internal error building regex: %s", e.message);
    }
    MatchInfo match;
    if (!port_re.match (sout, 0, out match)) {
        stdout.printf ("[SKIP: no gphoto2 compatible camera attached] ");
        return;
    }
    string busnum = match.fetch(1);
    string devnum = match.fetch(2);

    // record several operations
    spawn ("sh -c '%s /dev/bus/usb/%s/%s > gphoto-test.umockdev'".printf(
               "umockdev-record", busnum, devnum),
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpstr (sout, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);

    string cmd = "sh -c 'gphoto2 -l; gphoto2 -L'";
    spawn ("%s -i /dev/bus/usb/%s/%s=gphoto-test.ioctl -- %s".printf(
               "umockdev-record", busnum, devnum, cmd),
           out sout_record, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);

    // now run the same command under umockdev-run and ensure it outputs the
    // same thing
    spawn ("%s -d gphoto-test.umockdev -i /dev/bus/usb/%s/%s=gphoto-test.ioctl -- %s".printf(
               "umockdev-run", busnum, devnum, cmd),
           out sout, out serr, out exit);
    assert_cmpstr (serr, CompareOperator.EQ, "");
    assert_cmpint (exit, CompareOperator.EQ, 0);
    assert_cmpstr (sout, CompareOperator.EQ, sout_record);

    FileUtils.remove ("gphoto-test.umockdev");
    FileUtils.remove ("gphoto-test.ioctl");
}

int
main (string[] args)
{
    Test.init (ref args);

    string? f = Environment.get_variable ("SLOW_TESTBED_FACTOR");
    if (f != null && int.parse(f) > 0)
        slow_testbed_factor = int.parse(f);

    // determine path of helper programs
    tests_dir = Path.get_dirname (args[0]);
    if (tests_dir.has_suffix (".libs")) // libtool hack
        tests_dir = Path.get_dirname (tests_dir);
    readbyte_path = Path.build_filename (tests_dir, "readbyte");

    Test.add_func ("/umockdev-record/testbed-all-empty", t_testbed_all_empty);
    Test.add_func ("/umockdev-record/testbed-one", t_testbed_one);
    Test.add_func ("/umockdev-record/testbed-multiple", t_testbed_multiple);

    Test.add_func ("/umockdev-record/system-single", t_system_single);
    // causes eternal hangs or crashes in some environments
    //Test.add_func ("/umockdev-record/system-all", t_system_all);
    Test.add_func ("/umockdev-record/system-invalid", t_system_invalid);
    Test.add_func ("/umockdev-record/ioctl-log", t_system_ioctl_log);
    Test.add_func ("/umockdev-record/ioctl-log-append-dev-mismatch", t_system_ioctl_log_append_dev_mismatch);
    Test.add_func ("/umockdev-record/script-log-simple", t_system_script_log_simple);
    Test.add_func ("/umockdev-record/script-log-simple-fopen", t_system_script_log_simple_fopen);
    Test.add_func ("/umockdev-record/script-log-append-same-dev", t_system_script_log_append_same_dev);
    Test.add_func ("/umockdev-record/script-log-append-dev-mismatch", t_system_script_log_append_dev_mismatch);
    Test.add_func ("/umockdev-record/script-log-chatter", t_system_script_log_chatter);
    Test.add_func ("/umockdev-record/script-log-socket", t_system_script_log_chatter_socket_stream);
    Test.add_func ("/umockdev-record/evemu-log", t_system_evemu_log);

    // error conditions
    Test.add_func ("/umockdev-record/invalid-args", t_run_invalid_args);

    // these tests require particular hardware and get skipped otherwise
    Test.add_func ("/umockdev-record/gphoto2-record", t_gphoto2_record);

    return Test.run();
}
