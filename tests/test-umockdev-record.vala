/*
 * test-umockdev-record.vala
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

string umockdev_record_path;
string umockdev_run_path;
string rootdir;

// wrappers to avoid "unhandled error" warnings
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
        stderr.printf ("'%s' not found in '%s'\n", needle, haystack);
        Process.abort();
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

    spawn (umockdev_record_path + " --all", out sout, out serr, out exit);

    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpstr (sout, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
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
                                  {"simple_attr", "1", "multiline_attr", "a\\b\nc\\d\nlast"},
                                  {"SIMPLE_PROP", "1"});
    tb.set_attribute_binary (syspath, "binary_attr", {0x41, 0xFF, 0, 5, 0xFF, 0});
    tb.set_attribute_link (syspath, "driver", "../../drivers/foo");

    spawn (umockdev_record_path + " --all", out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, """P: /devices/dev1
E: SIMPLE_PROP=1
E: SUBSYSTEM=pci
H: binary_attr=41FF0005FF00
L: driver=../../drivers/foo
A: multiline_attr=a\\b\nc\\d\nlast
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
    var dev1 = tb.add_devicev ("pci", "dev1", null, {"dev1color", "green"}, {"DEV1COLOR", "GREEN"});
    var subdev1 = tb.add_devicev ("pci", "subdev1", dev1, {"subdev1color", "yellow"},
                                  {"SUBDEV1COLOR", "YELLOW"});
    tb.add_devicev ("pci", "dev2", null, {"dev2color", "brown"}, {"DEV2COLOR", "BROWN"});

    // should grab device and all parents
    spawn (umockdev_record_path + " " + subdev1, out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, """P: /devices/dev1/subdev1
E: SUBDEV1COLOR=YELLOW
E: SUBSYSTEM=pci
A: subdev1color=yellow

P: /devices/dev1
E: DEV1COLOR=GREEN
E: SUBSYSTEM=pci
A: dev1color=green

""");

    // only dev1
    spawn (umockdev_record_path + " " + dev1, out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, """P: /devices/dev1
E: DEV1COLOR=GREEN
E: SUBSYSTEM=pci
A: dev1color=green

""");

    // with --all it should have all three
    spawn (umockdev_record_path + " --all", out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert (sout.contains ("P: /devices/dev1/subdev1\n"));
    assert (sout.contains ("P: /devices/dev1\n"));
    assert (sout.contains ("P: /devices/dev2\n"));
}


// tracing ioctls does not work in testbed
static void
t_testbed_no_ioctl_record ()
{
    string sout;
    string serr;
    int exit;

    var tb = new UMockdev.Testbed ();
    tb.add_devicev ("mem", "zero", null, {"dev", "1:5"}, {});
    spawn (umockdev_record_path + " --ioctl /sys/devices/zero=/dev/stdout -- head -c1 /dev/zero",
           out sout, out serr, out exit);
    assert_cmpint (exit, Op.NE, 0);
    assert_cmpstr (sout, Op.EQ, "");
    assert (serr.contains ("UMOCKDEV_DIR cannot be used"));
}

static void
t_system_single ()
{
    string sout;
    string serr;
    int exit;

    spawn (umockdev_record_path + " /dev/null /dev/loop0", out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_in("E: DEVNAME=/dev/null", sout);
    assert_in("P: /devices/virtual/block/loop0", sout);
    assert_in("E: DEVNAME=/dev/loop0", sout);
}

// system /sys: umockdev-record --all works and result loads back
static void
t_system_all ()
{
    string sout;
    string serr;
    int exit;

    spawn (umockdev_record_path + " --all", out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert (sout.has_prefix ("P:"));
    assert_cmpint (sout.length, Op.GE, 100);

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

    spawn (umockdev_record_path + " /sys/class", out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "Invalid device /sys/class, has no uevent attribute\n");
    assert_cmpstr (sout, Op.EQ, "");
    assert_cmpint (exit, Op.NE, 0);

    spawn (umockdev_record_path + " /sys/block/loop0/size", out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "Invalid device /sys/devices/virtual/block/loop0/size, has no uevent attribute\n");
    assert_cmpstr (sout, Op.EQ, "");
    assert_cmpint (exit, Op.NE, 0);
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
    } catch (Error e) { Process.abort (); }
    string log = Path.build_filename (workdir, "log");

    // should not log anything as that device is not touched
    spawn (umockdev_record_path + " --ioctl=/dev/null=" + log + " -- head -c1 /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, "\0");
    assert (!FileUtils.test (log, FileTest.EXISTS));

    // this should create a log
    spawn (umockdev_record_path + " --ioctl /dev/zero=" + log + " -- head -c1 /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, "\0");
    assert (FileUtils.test (log, FileTest.EXISTS));
    assert_cmpstr (file_contents (log), Op.EQ, "");

    FileUtils.remove (log);

    // invalid syntax
    spawn (umockdev_record_path + " --ioctl /dev/null -- head -c1 /dev/zero",
           out sout, out serr, out exit);
    assert_cmpint (exit, Op.NE, 1);
    assert_cmpstr (sout, Op.EQ, "");
    assert (serr.contains ("--ioctl"));
    assert (serr.contains ("="));
    assert (!FileUtils.test (log, FileTest.EXISTS));

    DirUtils.remove (workdir);
}

/*
 * umockdev-record --script recording to a file, with simple "head" command
 */
static void
t_system_script_log_simple ()
{
    string sout;
    string serr;
    int exit;
    string log;

    try {
        FileUtils.close(FileUtils.open_tmp ("test_script_log.XXXXXX", out log));
    } catch (Error e) { Process.abort (); }

    // should not log anything as that device is not touched
    spawn (umockdev_record_path + " --script=/dev/null=" + log + " -- head -c1 /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, "\0");
    assert_cmpstr (file_contents (log), Op.EQ, "");

    // should log one read
    spawn (umockdev_record_path + " --script=/dev/zero=" + log + " -- head -c1 /dev/zero",
           out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, "\0");
    string[] logwords = file_contents (log).split(" ");
    assert_cmpuint (logwords.length, Op.EQ, 3);
    assert_cmpstr (logwords[0], Op.EQ, "r");
    // should be quick, give it 5 ms at most
    assert_cmpint (int.parse(logwords[1]), Op.LE, 5);
    assert_cmpstr (logwords[2], Op.EQ, "^@");

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

/*
 * umockdev-record --script recording to a file, with our chatter command
 */
static void
t_system_script_log_chatter ()
{
    string log;

    try {
        FileUtils.close(FileUtils.open_tmp ("test_script_log.XXXXXX", out log));
    } catch (Error e) { Process.abort (); }

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
            {umockdev_record_path, "--script", (string) ptyname + "=" + log, "--",
             Path.build_filename (rootdir, "tests", "chatter"), (string) ptyname},
            null, SpawnFlags.SEARCH_PATH | SpawnFlags.DO_NOT_REAP_CHILD | SpawnFlags.STDOUT_TO_DEV_NULL,
            null, out chatter_pid, null, null, null));
    } catch (SpawnError e) {
        error ("Cannot call umockdev-record: %s", e.message);
    }

    var chatter_stream = FileStream.fdopen (ptym, "r+");
    assert (chatter_stream != null);

    // expect the first two lines
    assert_cmpstr (read_line_timeout (chatter_stream), Op.EQ, "Hello world!\n");
    assert_cmpstr (read_line_timeout (chatter_stream), Op.EQ, "What is your name?\n");

    // type name and second string after some delay
    Thread.usleep (500000);
    chatter_stream.puts ("John\n");

    while (!read_line_timeout (chatter_stream).contains ("line break in one write"));

    Thread.usleep (300000);
    chatter_stream.puts ("foo ☹ bar ^!\n");

    assert_cmpstr (read_line_timeout (chatter_stream), Op.EQ, "bye!\n");

    int status;
    assert_cmpint ((int) Posix.waitpid (chatter_pid, out status, 0), Op.EQ, (int) chatter_pid);
    assert_cmpint (status, Op.EQ, 0);

    // evaluate log
    var log_stream = FileStream.open (log, "r");
    int time = 0;
    assert_cmpint (log_stream.scanf ("w %d Hello world!^JWhat is your name?^J\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.LE, 20);
    assert_cmpint (log_stream.scanf ("r %d John^J\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.GE, 450);
    assert_cmpint (time, Op.LE, 800);
    assert_cmpint (log_stream.scanf ("w %d I ♥ John^Ja^I tab and a^J line break in one write^J\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.LE, 20);
    assert_cmpint (log_stream.scanf ("r %d foo ☹ bar ^`!^J\n", &time), Op.EQ, 1);;
    assert_cmpint (time, Op.GE, 250);
    assert_cmpint (time, Op.LE, 450);

    assert_cmpint (log_stream.scanf ("w %d bye!^J\n", &time), Op.EQ, 1);;
    assert_cmpint (time, Op.LE, 20);

    // verify EOF
    assert_cmpint (log_stream.scanf ("%*c"), Op.EQ, -1);

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
                {umockdev_record_path, "--script", spath + "=" + log, "--",
                 Path.build_filename (rootdir, "tests", "chatter-socket-stream"), spath},
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
        assert_cmpstr ((string) buf, Op.EQ, "What is your name?\n");

        // type name and second string after some delay
        Thread.usleep (300000);
        conn.send ("John\n".data);

        // give it some time for the response
        Thread.usleep (10000);
        len = conn.receive (buf);
        assert (len > 0);
        buf[len] = 0;
        assert_cmpstr ((string) buf, Op.EQ, "hello John\n");

        // check the send message
        Thread.usleep (20000);
        len = conn.receive (buf);
        assert (len > 0);
        buf[len] = 0;
        assert_cmpstr ((string) buf, Op.EQ, "send()");

        // send stuff for recv()
        Thread.usleep (20000);
        conn.send ("recv()".data);
    } catch (Error e) {
        FileUtils.remove (spath);
        error ("Error: %s", e.message);
    }
    FileUtils.remove (spath);

    int status;
    assert_cmpint ((int) Posix.waitpid (chatter_pid, out status, 0), Op.EQ, (int) chatter_pid);
    assert_cmpint (status, Op.EQ, 0);

    // evaluate log
    var log_stream = FileStream.open (log, "r");
    int time = 0;
    assert_cmpint (log_stream.scanf ("w %d What is your name?^J\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.LE, 20);
    assert_cmpint (log_stream.scanf ("r %d John^J\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.GE, 250);
    assert_cmpint (time, Op.LE, 400);
    assert_cmpint (log_stream.scanf ("w %d hello John^J\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.LE, 20);
    assert_cmpint (log_stream.scanf ("w %d send()\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.GE, 10);
    assert_cmpint (log_stream.scanf ("r %d recv()\n", &time), Op.EQ, 1);
    assert_cmpint (time, Op.GE, 20);
    assert_cmpint (time, Op.LE, 40);

    FileUtils.remove (log);
}

static void
t_run_invalid_args ()
{
    string sout;
    string serr;
    int exit;

    spawn (umockdev_record_path + " /dev/no/such/device", out sout, out serr, out exit);

    assert_in ("Cannot access device /dev/no/such/device: No such file", serr);
    assert_cmpstr (sout, Op.EQ, "");
    assert_cmpint (exit, Op.NE, 0);
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
        stderr.printf ("[SKIP: gphoto2 not installed] ");
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
               umockdev_record_path, busnum, devnum),
           out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpstr (sout, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);

    string cmd = "sh -c 'gphoto2 -l; gphoto2 -L'";
    spawn ("%s -i /dev/bus/usb/%s/%s=gphoto-test.ioctl -- %s".printf(
               umockdev_record_path, busnum, devnum, cmd),
           out sout_record, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);

    // now run the same command under umockdev-run and ensure it outputs the
    // same thing
    spawn ("%s -d gphoto-test.umockdev -i /dev/bus/usb/%s/%s=gphoto-test.ioctl -- %s".printf(
               umockdev_run_path, busnum, devnum, cmd),
           out sout, out serr, out exit);
    assert_cmpstr (serr, Op.EQ, "");
    assert_cmpint (exit, Op.EQ, 0);
    assert_cmpstr (sout, Op.EQ, sout_record);

    FileUtils.remove ("gphoto-test.umockdev");
    FileUtils.remove ("gphoto-test.ioctl");
}

int
main (string[] args)
{
    Test.init (ref args);

    // determine path of umockdev-record
    string? r = Environment.get_variable ("TOP_BUILDDIR");
    if (r == null)
        rootdir = Path.get_dirname (Path.get_dirname (Path.get_dirname (Posix.fixed_realpath (args[0]))));
    else
        rootdir = r;

    umockdev_record_path = Path.build_filename (rootdir, "src", "umockdev-record");
    umockdev_run_path = Path.build_filename (rootdir, "src", "umockdev-run");

    Test.add_func ("/umockdev-record/testbed-all-empty", t_testbed_all_empty);
    Test.add_func ("/umockdev-record/testbed-one", t_testbed_one);
    Test.add_func ("/umockdev-record/testbed-multiple", t_testbed_multiple);
    Test.add_func ("/umockdev-record/testbed-no-ioctl-record", t_testbed_no_ioctl_record);

    Test.add_func ("/umockdev-record/system-single", t_system_single);
    Test.add_func ("/umockdev-record/system-all", t_system_all);
    Test.add_func ("/umockdev-record/system-invalid", t_system_invalid);
    Test.add_func ("/umockdev-record/ioctl-log", t_system_ioctl_log);
    Test.add_func ("/umockdev-record/script-log-simple", t_system_script_log_simple);
    Test.add_func ("/umockdev-record/script-log-chatter", t_system_script_log_chatter);
    Test.add_func ("/umockdev-record/script-log-socket", t_system_script_log_chatter_socket_stream);

    // error conditions
    Test.add_func ("/umockdev-record/invalid-args", t_run_invalid_args);

    // these tests require particular hardware and get skipped otherwise
    Test.add_func ("/umockdev-record/gphoto2-record", t_gphoto2_record);

    return Test.run();
}
