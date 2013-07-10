/*
 * Record Linux devices and their ancestors from sysfs/udev.
 * All attributes and properties are included, non-ASCII ones get printed in hex.
 * The record is written to the standard output.
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

static void
exit_error(string message, ...)
{
    stderr.vprintf(message, va_list());
    stderr.puts("\n");
    Process.exit(1);
}

static void
devices_from_dir (string dir, ref GenericArray<string> devs)
{
    Dir d;
    try {
        d = Dir.open(dir);
    } catch (FileError e) {
        return;
    }

    bool has_uevent = false;
    bool has_subsystem = false;
    string entry;
    while ((entry = d.read_name())!= null) {
        if (entry == "uevent")
            has_uevent = true;
        else if (entry == "subsystem")
            has_subsystem = true;
        else {
            var p = Path.build_filename(dir, entry);
            Posix.Stat st;
            if (Posix.lstat(p, out st) == 0 && Posix.S_ISDIR(st.st_mode) &&
                !Posix.S_ISLNK(st.st_mode))
                devices_from_dir(p, ref devs);
        }
    }
    if (has_uevent && has_subsystem)
        devs.add(dir);
}

[CCode (array_length=false, array_null_terminated=true)]
static string[]
all_devices ()
{
    var devs = new GenericArray<string>();
    devices_from_dir("/sys/devices", ref devs);
    return devs.data;
}

// If dev is a block or character device, convert it to a sysfs path.
static string
resolve (string dev)
{
    Posix.Stat st;
    if (Posix.stat(dev, out st) != 0)
        exit_error("Cannot access device %s: %s", dev, strerror(errno));

    uint major = Posix.major(st.st_rdev);
    uint minor = Posix.minor(st.st_rdev);

    string link;
    // character device?
    if (Posix.S_ISCHR(st.st_mode))
        link = "/sys/dev/char/%u:%u".printf(major, minor);
    else if (Posix.S_ISBLK(st.st_mode))
        link = "/sys/dev/block/%u:%u".printf(major, minor);
    else
        link = dev;

    string real = Posix.fixed_realpath(link);
    // FIXME: does not work under testbed for test suite
    //assert(real != null);
    if (real == null)
        real = link;

    if (!FileUtils.test(Path.build_filename(real, "uevent"), FileTest.EXISTS))
        exit_error("Invalid device %s, has no uevent attribute", real);

    return real;
}

static string?
parent(string dev)
{
    string p = Path.get_dirname(dev);
    if (!p.has_prefix("/sys"))
        return null;
    if (FileUtils.test(Path.build_filename(p, "uevent"), FileTest.EXISTS) &&
        FileUtils.test(Path.build_filename(p, "subsystem"), FileTest.EXISTS))
        return p;
    // we might have intermediate directories without uevent, so try the next
    // higher one
    return parent(p);
}

static string
format_hex(uint8[] bytes, int len=-1)
{
    if (len < 0)
        len = bytes.length;
    var result = new StringBuilder();
    for (int i = 0; i < len; ++i)
        result.append_printf("%02X", bytes[i]);
    return result.str;
}

static void
write_attr(string name, uint8[] val)
{
    // check if it's text or binary
    string strval = (string) val;
    if (val.length == strval.length && strval.validate())
        stdout.printf("A: %s=%s", name, strval.chomp().escape(""));
    else
        stdout.printf("H: %s=%s", name, format_hex(val));
    stdout.putc('\n');
}

// Return contents of device node, if applicable.
static string
dev_contents(string dev)
{
    Posix.Stat st;

    if (Posix.lstat(dev, out st) != 0)
        return "";

    // only attempt this for safe devices; USB for now
    if (!Posix.S_ISCHR(st.st_mode) || Posix.major(st.st_rdev) != 189)
        return "";

    // read the first KiB
    int fd = Posix.open(dev, Posix.O_RDONLY|Posix.O_NONBLOCK);
    if (fd < 0)
        return "";

    // read the first KiB; ignore if it's bigger
    uint8[] buffer = new uint8[1025];
    ssize_t len = Posix.read(fd, buffer, 1025);
    string result = "";
    if (len > 0 && len <= 1024)
        result = "=" + format_hex(buffer, (int) len);
    Posix.close(fd);
    return result;
}

static void
record_device(string dev)
{
    debug("recording device %s", dev);

    // we start with udevadm dump of this device, which will include all udev properties
    string u_out;
    int exitcode;
    try {
        Process.spawn_sync(null,
                           {"udevadm", "info", "--query=all", "--path", dev},
                           null,
                           SpawnFlags.SEARCH_PATH,
                           null,
                           out u_out,
                           null,
                           out exitcode);
        if (exitcode != 0)
            throw new SpawnError.FAILED("udevadm exited with code %i".printf(exitcode));
    } catch (SpawnError e) {
        exit_error("Cannot call udevadm: %s", e.message);
    }

    foreach (string line in u_out.split("\n")) {
        // filter out redundant/uninteresting properties and link priority
        if (line.length == 0 || line.has_prefix("E: DEVPATH=") ||
            line.has_prefix("E: UDEV_LOG=") || line.has_prefix("E: USEC_INITIALIZED=") ||
            line.has_prefix("L: "))
            continue;

        if (line.has_prefix("N: ")) {
            line = line + dev_contents("/dev/" + line.substring(3).chomp());
        }
        stdout.puts(line);
        stdout.putc('\n');
    }

    // work around kernel crash, skip reading attributes for Tegra stuff (LP #1190225)
    if (dev.contains("tegra")) {
        stdout.putc('\n');
        return;
    }

    // now append all attributes
    Dir d;
    try {
        d = Dir.open(dev);
    } catch (FileError e) {
        exit_error("Cannot open directory %s: %s", dev, e.message);
        return; /* not reached, just to avoid warnings */
    }
    var attributes = new List<string>();
    string entry;
    // filter out the uninteresting attributes, sort the others
    while ((entry = d.read_name()) != null)
        if (entry != "subsystem" && entry != "firmware_node" && entry != "uevent")
            attributes.append(entry);
    attributes.sort(strcmp);

    foreach (var attr in attributes) {
        string attr_path = Path.build_filename(dev, attr);
        // only look at files or symlinks
        if (FileUtils.test(attr_path, FileTest.IS_SYMLINK)) {
            try {
                stdout.printf("L: %s=%s\n", attr, FileUtils.read_link(attr_path));
            } catch (FileError e) {
                exit_error("Cannot read link %s: %s", attr, e.message);
            }
        } else if (FileUtils.test(attr_path, FileTest.IS_REGULAR)) {
            uint8[] contents;
            try {
                FileUtils.get_data(attr_path, out contents);
                write_attr(attr, contents);
            } catch (FileError e) {} // some attributes are EACCES, or "no such device", etc.
        }
    }

    stdout.putc('\n');
}

static void
dump_devices(string[] devices)
{
    // process arguments parentwards first
    var seen = new HashTable<string,unowned string>(str_hash, str_equal);
    foreach (string device in devices) {
        while (device != null) {
            if (!seen.contains(device)) {
                seen.add(device.dup());
                record_device(device);
            }
            device = parent(device);
        }
    }
}

// split a devname=filename argument into a device number and a file name
static void
split_devfile_arg(string arg, out string devnum, out string fname)
{
    string[] parts = arg.split ("=", 2); // devname, ioctlfilename
    if (parts.length != 2)
        exit_error("--ioctl argument must be devname=filename");
    string dev = parts[0];
    fname = parts[1];

    // build device major/minor
    Posix.Stat st;
    if (Posix.stat(dev, out st) != 0)
        exit_error("Cannot access device %s: %s", dev, strerror(errno));

    if (Posix.S_ISCHR(st.st_mode) || Posix.S_ISBLK(st.st_mode)) {
        // if we have a device node, get devnum from stat
        devnum = ((Posix.major(st.st_rdev) << 8) + Posix.minor(st.st_rdev)).to_string();
    } else {
        // otherwise we assume that we have a sysfs device, resolve via dev attribute
        string contents;
        try {
            FileUtils.get_contents(Path.build_filename(dev, "dev"), out contents);
        } catch (FileError e) {
            exit_error("Cannot open %s/dev: %s", dev, e.message);
        }
        string[] fields = contents.strip().split(":");
        assert(fields.length == 2);
        devnum = ((int.parse(fields[0]) << 8) | int.parse(fields[1])).to_string();
    }
}

// Record ioctls for given device into outfile
static void
record_ioctl(string arg)
{
    string devnum, outfile;
    split_devfile_arg(arg, out devnum, out outfile);
    Environment.set_variable("UMOCKDEV_IOCTL_RECORD_FILE", outfile, true);
    Environment.set_variable("UMOCKDEV_IOCTL_RECORD_DEV", devnum, true);
}

// Record reads/writes for given device into outfile
static uint record_script_counter = 0;
static void
record_script(string arg)
{
    string devnum, outfile;
    split_devfile_arg(arg, out devnum, out outfile);
    string c = record_script_counter.to_string();

    Environment.set_variable("UMOCKDEV_SCRIPT_RECORD_FILE_" + c, outfile, true);
    Environment.set_variable("UMOCKDEV_SCRIPT_RECORD_DEV_" + c, devnum, true);

    record_script_counter++;
}

[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_devices;
static bool opt_all = false;
static string? opt_ioctl = null;
[CCode (array_length=false, array_null_terminated=true)]
static string[] opt_script;
static bool opt_version = false;

static const GLib.OptionEntry[] options = {
    {"all", 'a', 0, OptionArg.NONE, ref opt_all, "Record all devices"},
    {"ioctl", 'i', 0, OptionArg.FILENAME, ref opt_ioctl,
     "Trace ioctls on the device, record into given file. In this case, all positional arguments are a command (and its arguments) to run that gets traced.", "devname=FILE"},
    {"script", 's', 0, OptionArg.FILENAME_ARRAY, ref opt_script,
     "Trace reads and writes on the device, record into given file. In this case, all positional arguments are a command (and its arguments) to run that gets traced. Can be specified multiple times.", "devname=FILE"},
    {"", 0, 0, OptionArg.STRING_ARRAY, ref opt_devices, "Path of a device in /dev or /sys, or command and arguments with --ioctl.", "DEVICE [...]"},
    {"version", 0, 0, OptionArg.NONE, ref opt_version, "Output version information and exit"},
    { null }
};

public static int
main (string[] args)
{
    var oc = new OptionContext("");
    oc.set_summary("Record Linux devices and their ancestors from sysfs/udev, or record ioctls for a device.");
    oc.add_main_entries (options, null);
    try {
        oc.parse (ref args);
    } catch (OptionError e) {
        exit_error("Error: %s\nRun %s --help for how to use this program", e.message, args[0]);
    }

    if (opt_version) {
        stdout.printf("%s\n", Config.VERSION);
        return 0;
    }

    if (opt_all && opt_devices.length > 0)
        exit_error("Specifying a device list together with --all is invalid.");
    if (!opt_all && opt_devices.length == 0)
        exit_error("Need to specify at least one device or --all.");
    if ((opt_ioctl != null || opt_script.length > 0) && (opt_all || opt_devices.length < 1))
        exit_error("For recording ioctls or scripts you have to specify a command to run");

    // device dump mode
    if (opt_ioctl == null && opt_script.length == 0) {
        // Evaluate --all and resolve devices
        if (opt_all)
            opt_devices = all_devices();
        else {
            for (int i = 0; i < opt_devices.length; ++i)
                opt_devices[i] = resolve(opt_devices[i]);
        }
        dump_devices(opt_devices);
        return 0;
    }

    // in ioctl/script recording mode opt_devices is the command to run

    string? preload = Environment.get_variable("LD_PRELOAD");
    if (preload == null)
        preload = "";
    else
        preload = preload + ":";
    Environment.set_variable("LD_PRELOAD", preload + "libumockdev-preload.so.0", true);

    // set up environment to tell our preload what to record
    if (opt_ioctl != null)
        record_ioctl(opt_ioctl);
    foreach (string s in opt_script)
        record_script(s);

    Posix.execvp(opt_devices[0], opt_devices);
    exit_error("Cannot run program %s: %s", opt_devices[0], strerror(errno));
    return 0;
}
