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

using UMockdevUtils;

[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_device;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_ioctl;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_pcap;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_script;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_unix_stream;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_evemu_events;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_program;
static bool opt_version = false;

const GLib.OptionEntry[] options = {
    {"device", 'd', 0, OptionArg.FILENAME_ARRAY, ref opt_device,
     "Load an umockdev-record device description into the testbed. Can be specified multiple times.",
     "filename"},
    {"ioctl", 'i', 0, OptionArg.FILENAME_ARRAY, ref opt_ioctl,
     "Load an umockdev-record ioctl dump into the testbed. Can be specified multiple times.",
     "devname=ioctlfilename"},
    {"pcap", 'p', 0, OptionArg.FILENAME_ARRAY, ref opt_pcap,
     "Load an pcap/pcapng USB dump into the testbed. Can be specified multiple times.",
     "sysfs=pcapfilename"},
    {"script", 's', 0, OptionArg.FILENAME_ARRAY, ref opt_script,
     "Load an umockdev-record script into the testbed. Can be specified multiple times.",
     "devname=scriptfilename"},
    {"unix-stream", 'u', 0, OptionArg.FILENAME_ARRAY, ref opt_unix_stream,
     "Load an umockdev-record script for a mocked Unix stream socket. Can be specified multiple times.",
     "socket_path=scriptfilename"},
    {"evemu-events", 'e', 0, OptionArg.FILENAME_ARRAY, ref opt_evemu_events,
     "Load an evemu .events file into the testbed. Can be specified multiple times.",
     "devname=eventsfilename"},
    {"", 0, 0, OptionArg.STRING_ARRAY, ref opt_program, "", ""},
    {"version", 0, 0, OptionArg.NONE, ref opt_version, "Output version information and exit"},
    { null }
};

GLib.MainLoop loop;
Pid child_pid;
int child_status;

static void
child_watch_cb (Pid pid, int status)
{
    /* Record status and quit mainloop. */
    child_status = status;
    loop.quit();
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
    checked_setenv ("LD_PRELOAD", preload + "libumockdev-preload.so.0");

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

    foreach (var i in opt_pcap) {
        string[] parts = i.split ("=", 2); // sysfsname, ioctlfilename
        if (parts.length != 2) {
            stderr.printf ("Error: --ioctl argument must be devname=filename\n");
            return 1;
        }
        try {
            testbed.load_pcap (parts[0], parts[1]);
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

    foreach (var i in opt_evemu_events) {
        string[] parts = i.split ("=", 2); // devname, eventsfilename
        if (parts.length != 2) {
            stderr.printf ("Error: --evemu-events argument must be devname=filename\n");
            return 1;
        }
        try {
            testbed.load_evemu_events (parts[0], parts[1]);
        } catch (Error e) {
            stderr.printf ("Error: Cannot install %s for device %s: %s\n", parts[1], parts[0], e.message);
            return 1;
        }
    }

    if (opt_program.length == 0) {
        stderr.printf ("No program specified. See --help for how to use umockdev-run\n");
        return 1;
    }

    // we want to run opt_program as a subprocess instead of execve()ing, so
    // that we can run device script threads in the background
    loop = new GLib.MainLoop(null);
    try {
        child_pid = spawn_process_under_test (opt_program, child_watch_cb);
    } catch (Error e) {
        error("Cannot run %s: %s", opt_program[0], e.message);
    }

    loop.run();

    // free the testbed here already, so that it gets cleaned up before raise()
    testbed = null;

    if (Process.if_exited (child_status))
        return Process.exit_status (child_status);
    if (Process.if_signaled (child_status))
        Process.raise (Process.term_sig (child_status));

    return child_status;
}
