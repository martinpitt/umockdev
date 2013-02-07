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
static string[] opt_load;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_ioctl;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_program;

static const GLib.OptionEntry[] options = {
    {"load", 'l', 0, OptionArg.FILENAME_ARRAY, ref opt_load,
     "Load an umockdev-record file into the testbed. Can be specified multiple times.",
     "filename"},
    {"ioctl", 'i', 0, OptionArg.FILENAME_ARRAY, ref opt_ioctl,
     "Load an ioctl record file into the testbed. Can be specified multiple times.",
     "devname=ioctlfilename"},
    {"", 0, 0, OptionArg.STRING_ARRAY, ref opt_program, "", "program [args..]"},
    { null }
};

static int
main (string[] args)
{
    var oc = new OptionContext ("-- program [args..]");
    oc.set_summary ("Run a program under an umockdev testbed.");
    oc.add_main_entries (options, null);
    try {
        oc.parse (ref args);
    } catch (OptionError e) {
        stderr.printf("Error: %s\nRun %s --help for how to use this program\n", e.message, args[0]);
        Process.exit (1);
    }

    Environment.set_variable ("LD_PRELOAD", "libumockdev-preload.so.0", true); // FIXME

    var testbed = new UMockdev.Testbed ();

    foreach (var path in opt_load) {
        string record;
        try {
            FileUtils.get_contents (path, out record);
        } catch (FileError e) {
            stderr.printf ("Error: Cannot open %s: %s\n", path, e.message);
            Process.exit (1);
        }
        try {
            testbed.add_from_string (record);
        } catch (GLib.Error e) {
            stderr.printf ("Error: Invalid record file %s: %s\n", path, e.message);
            Process.exit (1);
        }
    }

    foreach (var i in opt_ioctl) {
        string[] parts = i.split ("=", 2); // devname, ioctlfilename
        string contents;
        try {
            FileUtils.get_contents (parts[1], out contents);
        } catch (FileError e) {
            stderr.printf ("Error: Cannot open %s: %s\n", parts[1], e.message);
            Process.exit (1);
        }
        string dest = Path.build_filename (testbed.get_root_dir(), "ioctl", parts[0]);
        DirUtils.create_with_parents (Path.get_dirname (dest), 0755);
        try {
            FileUtils.set_contents (dest, contents);
        } catch (FileError e) {
            stderr.printf ("Error: Cannot write %s: %s\n", dest, e.message);
            Process.exit (1);
        }
    }

    int status;
    try {
        Process.spawn_sync (null, opt_program, null, SpawnFlags.SEARCH_PATH,
                            null, null, null, out status);
    } catch (SpawnError e) {
            stderr.printf ("Error: %s\n", e.message);
            Process.exit (1);
    }

    return status;
}
