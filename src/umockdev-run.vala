/**
 * umockdev-run: Run a program under a testbed
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

[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_device;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_ioctl;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_script;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_unix_stream;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_program;
static bool opt_version = false;

static const GLib.OptionEntry[] options = {
    {"device", 'd', 0, OptionArg.FILENAME_ARRAY, ref opt_device,
     "Load an umockdev-record device description into the testbed. Can be specified multiple times.",
     "filename"},
    {"ioctl", 'i', 0, OptionArg.FILENAME_ARRAY, ref opt_ioctl,
     "Load an umockdev-record ioctl dump into the testbed. Can be specified multiple times.",
     "devname=ioctlfilename"},
    {"script", 's', 0, OptionArg.FILENAME_ARRAY, ref opt_script,
     "Load an umockdev-record script into the testbed. Can be specified multiple times.",
     "devname=scriptfilename"},
    {"unix-stream", 'u', 0, OptionArg.FILENAME_ARRAY, ref opt_unix_stream,
     "Load an umockdev-record script for a mocked Unix stream socket. Can be specified multiple times.",
     "socket_path=scriptfilename"},
    {"", 0, 0, OptionArg.STRING_ARRAY, ref opt_program, "", ""},
    {"version", 0, 0, OptionArg.NONE, ref opt_version, "Output version information and exit"},
    { null }
};

Pid child_pid;

static void
child_sig_handler (int sig)
{
    debug ("umockdev-run: caught signal %i, propagating to child\n", sig);
    if (Posix.kill (child_pid, sig) != 0)
        stderr.printf ("umockdev-run: unable to propagate signal %i to child %i: %s\n",
                       sig, child_pid, strerror (errno));
}

static int
main (string[] args)
{
    var oc = new OptionContext ("-- program [args..]");
    oc.set_summary ("Run a program under an umockdev testbed.");
    oc.add_main_entries (options, null);
    try {
        oc.parse (ref args);
    } catch (Error e) {
        stderr.printf("Error: %s\nRun %s --help for how to use this program\n", e.message, args[0]);
        return 1;
    }

    if (opt_version) {
        stdout.printf("%s\n", Config.VERSION);
        return 0;
    }

    string? preload = Environment.get_variable ("LD_PRELOAD");
    if (preload == null)
        preload = "";
    else
        preload = preload + ":";
    Environment.set_variable ("LD_PRELOAD", preload + "libumockdev-preload.so.0", true);

    var testbed = new UMockdev.Testbed ();

    foreach (var path in opt_device) {
        string record;
        try {
            FileUtils.get_contents (path, out record);
        } catch (Error e) {
            stderr.printf ("Error: Cannot open %s: %s\n", path, e.message);
            return 1;
        }
        try {
            testbed.add_from_string (record);
        } catch (Error e) {
            stderr.printf ("Error: Invalid record file %s: %s\n", path, e.message);
            return 1;
        }
    }

    foreach (var i in opt_ioctl) {
        string[] parts = i.split ("=", 2); // devname, ioctlfilename
        if (parts.length != 2) {
            stderr.printf ("Error: --ioctl argument must be devname=filename\n");
            return 1;
        }
        try {
            testbed.load_ioctl (parts[0], parts[1]);
        } catch (Error e) {
            stderr.printf ("Error: Cannot install %s for device %s: %s\n", parts[1], parts[0], e.message);
            return 1;
        }
    }

    foreach (var i in opt_script) {
        string[] parts = i.split ("=", 2); // devname, scriptfilename
        if (parts.length != 2) {
            stderr.printf ("Error: --script argument must be devname=filename\n");
            return 1;
        }
        try {
            testbed.load_script (parts[0], parts[1]);
        } catch (Error e) {
            stderr.printf ("Error: Cannot install %s for device %s: %s\n", parts[1], parts[0], e.message);
            return 1;
        }
    }

    foreach (var i in opt_unix_stream) {
        string[] parts = i.split ("=", 2); // socket_path, scriptfilename
        if (parts.length != 2) {
            stderr.printf ("Error: --unix-stream argument must be socket_path=filename\n");
            return 1;
        }
        try {
            testbed.load_socket_script (parts[0], Posix.SOCK_STREAM, parts[1]);
        } catch (Error e) {
            stderr.printf ("Error: Cannot install %s for stream socket %s: %s\n", parts[1], parts[0], e.message);
            return 1;
        }
    }

    if (opt_program.length == 0) {
        stderr.printf ("No program specified. See --help for how to use umockdev-run\n");
        return 1;
    }

    // we want to run opt_program as a subprocess instead of execve()ing, so
    // that we can run device script threads in the background
    int status;
    try {
        Process.spawn_async (null, opt_program, null,
                            SpawnFlags.SEARCH_PATH | SpawnFlags.CHILD_INHERITS_STDIN | SpawnFlags.DO_NOT_REAP_CHILD ,
                            null, out child_pid);
    } catch (Error e) {
            stderr.printf ("Cannot run %s: %s\n", opt_program[0], e.message);
            Process.exit (1);
    }

    // propagate signals to the child
    var act = Posix.sigaction_t() { sa_handler = child_sig_handler, sa_flags = Posix.SA_RESETHAND };
    Posix.sigemptyset (act.sa_mask);
    assert (Posix.sigaction (Posix.SIGTERM, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGHUP, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGINT, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGQUIT, act, null) == 0);
    assert (Posix.sigaction (Posix.SIGABRT, act, null) == 0);

    Posix.waitpid (child_pid, out status, 0);
    Process.close_pid (child_pid);

    // free the testbed here already, so that it gets cleaned up before raise()
    testbed = null;

    if (Process.if_exited (status))
        return Process.exit_status (status);
    if (Process.if_signaled (status))
        Process.raise (Process.term_sig (status));

    return status;
}
