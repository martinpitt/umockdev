/*
 * Copyright (C) 2012-2013 Canonical Ltd.
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

namespace UMockdev {

/**
 * SECTION:umockdev
 * @short_description: Build a test bed for testing software that handles Linux
 * hardware devices.
 *
 * The #UMockdevTestbed class builds a temporary sandbox for mock devices.
 * Right now this covers sysfs, uevents, basic support for /dev devices, and
 * recording/mocking usbdevfs ioctls (for PtP/MTP devices), but other aspects
 * will be added in the future.  You can add a number of devices including
 * arbitrary sysfs attributes and udev properties, and then run your software
 * in that test bed that is independent of the actual hardware it is running
 * on.  With this you can simulate particular hardware in virtual environments
 * up to some degree, without needing any particular privileges or disturbing
 * the whole system.
 *
 * You can either add devices by specifying individual device paths,
 * properties, and attributes, or use the umockdev-record tool to create a human
 * readable/editable record of a real device (and all its parents) and load
 * that into the testbed with umockdev_testbed_add_from_string().
 *
 * Instantiating a #UMockdevTestbed object creates a temporary directory with
 * an empty sysfs tree and sets the $UMOCKDEV_DIR environment variable so that
 * programs subsequently started under umockdev-wrapper will use the test bed
 * instead of the system's real sysfs.
 */

public class Testbed: GLib.Object {
    public Testbed()
    {
        try {
            this.root_dir = DirUtils.make_tmp("umockdev.XXXXXX");
        } catch (FileError e) {
            stderr.printf("Cannot create temporary directory: %s\n", e.message);
            Process.abort();
        }
        this.sys_dir = Path.build_filename(this.root_dir, "sys");
        DirUtils.create(this.sys_dir, 0755);

        Environment.set_variable("UMOCKDEV_DIR", this.root_dir, true);
        debug("Created udev test bed %s", this.root_dir);
    }

    ~Testbed()
    {
        debug ("Removing test bed %s", this.root_dir);
        remove_dir (this.root_dir);
        Environment.unset_variable("UMOCKDEV_DIR");
    }

    /**
     * umockdev_testbed_get_root_dir:
     * @self: A #UMockdevTestbed.
     * 
     * Get the root directory for the testbed.
     *
     * Returns: The testbed's root directory.
     */
    public string get_root_dir()
    {
        return this.root_dir;
    }

    /**
     * umockdev_testbed_get_sys_dir:
     * @self: A #UMockdevTestbed.
     * 
     * Get the sysfs directory for the testbed.
     *
     * Returns: The testbed's sysfs directory.
     */
    public string get_sys_dir()
    {
        return this.sys_dir;
    }

    public void set_attribute(string devpath, string name, string value)
    {
        this.set_attribute_binary(devpath, name, value.data);
    }

    public void set_attribute_binary(string devpath, string name, uint8[] value)
    {
        try {
            FileUtils.set_data(Path.build_filename(this.root_dir, devpath, name), value);
        } catch (FileError e) {
            stderr.printf("Cannot write attribute file: %s\n", e.message);
            Process.abort();
        }
    }

    public void set_attribute_int(string devpath, string name, int value)
    {
        this.set_attribute(devpath, name, value.to_string());
    }

    public void set_attribute_hex(string devpath, string name, uint value)
    {
        this.set_attribute(devpath, name, "%x".printf(value));
    }

    public new void set_property(string devpath, string name, string value)
    {
        var uevent_path = Path.build_filename(this.root_dir, devpath, "uevent");
        string props = "";

         /* read current properties from the uevent file; if name is already set,
          * replace its value with the new one */
        File f = File.new_for_path(uevent_path);
        bool existing = false;
        string prefix = name + "=";
        try {
            var inp = new DataInputStream(f.read());
            string line;
            size_t len;
            while ((line = inp.read_line(out len)) != null) {
                if (line.has_prefix(prefix)) {
                    existing = true;
                    props += prefix + value + "\n";
                } else {
                    props += line + "\n";
                }
            }
            inp.close();

            /* if property name does not yet exist, append it */
            if (!existing)
                props += prefix + value + "\n";

            /* write it back */
            FileUtils.set_data(uevent_path, props.data);
        } catch (GLib.Error e) {
            stderr.printf("Cannot update uevent file: %s\n", e.message);
            Process.abort();
        }
    }

    public void set_property_int(string devpath, string name, int value)
    {
        this.set_property(devpath, name, value.to_string());
    }

    public void set_property_hex(string devpath, string name, uint value)
    {
        this.set_property(devpath, name, "%x".printf(value));
    }


    /**
     * umockdev_testbed_add_devicev:
     * @self: A #UMockdevTestbed.
     * @subsystem: The subsystem name, e. g. "usb"
     * @name: The device name; arbitrary, but needs to be unique within the testbed
     * @parent: (allow-none): device path of the parent device. Use %NULL for a
     *          top-level device.
     * @attributes: (array zero-terminated=1):
     *              A list of device sysfs attributes, alternating names and
     *              values, terminated with %NULL:
     *              { "key1", "value1", "key2", "value2", ..., NULL }
     * @properties: (array zero-terminated=1):
     *              A list of device udev properties; same format as @attributes
     *
     * This method is mostly meant for language bindings (where it is named
     * umockdev_testbed_add_device()). For C programs it is usually more convenient to
     * use umockdev_testbed_add_device().
     *
     * Add a new device to the testbed. A Linux kernel device always has a
     * subsystem (such as "usb" or "pci"), and a device name. The test bed only
     * builds a very simple sysfs structure without nested namespaces, so it
     * requires device names to be unique. Some gudev client programs might make
     * assumptions about the name (e. g. a SCSI disk block device should be called
     * sdaN). A device also has an arbitrary number of sysfs attributes and udev
     * properties; usually you should specify them upon creation, but it is also
     * possible to change them later on with umockdev_testbed_set_attribute() and
     * umockdev_testbed_set_property().
     *
     * Returns: The sysfs path for the newly created device. Free with g_free().
     *
     * Rename to: umockdev_testbed_add_device
     */
    public string? add_devicev(string subsystem, string name, string? parent,
                               [CCode(array_null_terminated=true, array_length=false)] string[] attributes,
                               [CCode(array_null_terminated=true, array_length=false)] string[] properties)
    {
        string dev_path;

        if (parent != null) {
            if (!FileUtils.test(parent, FileTest.IS_DIR)) {
                critical("add_devicev(): parent device %s does not exist", parent);
                return null;
            }
            dev_path = Path.build_filename(parent, name);
        } else
            dev_path = Path.build_filename("/sys/devices", name);
        var dev_dir = Path.build_filename(this.root_dir, dev_path);

        /* must not exist yet; do allow existing children, though */
        if (FileUtils.test(dev_dir, FileTest.EXISTS) &&
            FileUtils.test(Path.build_filename(dev_dir, "uevent"), FileTest.EXISTS))
            error("device %s already exists", dev_dir);

        /* create device and corresponding subsystem dir */
        if (DirUtils.create_with_parents(dev_dir, 0755) != 0)
            error("cannot create dev dir '%s': %s", dev_dir, strerror(errno));
        var class_dir = Path.build_filename(this.sys_dir, "class", subsystem);
        if (DirUtils.create_with_parents(class_dir, 0755) != 0)
            error("cannot create class dir '%s': %s", class_dir, strerror(errno));

        /* subsystem symlink */
        assert(FileUtils.symlink(Path.build_filename(make_dotdots(dev_path), "class", subsystem),
                                 Path.build_filename(dev_dir, "subsystem")) == 0);

        /* device symlink from class/; skip directories in name; this happens
         * when being called from add_from_string() when the parent devices do
         * not exist yet */
        assert(FileUtils.symlink(Path.build_filename("..", "..", dev_path.str("/devices/")),
                                 Path.build_filename(class_dir, Path.get_basename(name))) == 0);

        /* bus symlink */
        if (subsystem == "usb" || subsystem == "pci") {
            class_dir = Path.build_filename(this.sys_dir, "bus", subsystem, "devices");
            assert(DirUtils.create_with_parents(class_dir, 0755) == 0);
            assert(FileUtils.symlink(Path.build_filename("..", "..", "..", dev_path.str("/devices/")),
                                     Path.build_filename(class_dir, Path.get_basename(name))) == 0);
        }

        /* attributes */
        for (int i = 0; i < attributes.length - 1; i += 2)
            this.set_attribute(dev_path, attributes[i], attributes[i+1]);
        if (attributes.length % 2 != 0)
            warning("add_devicev: Ignoring attribute key '%s' without value", attributes[attributes.length-1]);

        /* properties; they go into the "uevent" sysfs attribute */
        string props = "";
        for (int i = 0; i < properties.length - 1; i += 2)
            props += properties[i] + "=" + properties[i+1] + "\n";
        if (properties.length % 2 != 0)
            warning("add_devicev: Ignoring property key '%s' without value", properties[properties.length-1]);
        this.set_attribute(dev_path, "uevent", props);

        return dev_path;
    }

    /**
     * umockdev_testbed_add_device: (skip)
     * @self: A #UMockdevTestbed.
     * @subsystem: The subsystem name, e. g. "usb"
     * @name: The device name; arbitrary, but needs to be unique within the testbed
     * @parent: (allow-none): device path of the parent device. Use %NULL for a
     *          top-level device.
     * @...: Arbitrarily many pairs of sysfs attributes (alternating names and
     *       values), terminated by %NULL, followed by arbitrarily many pairs of udev
     *       properties, terminated by another %NULL.
     *
     * Add a new device to the testbed. A Linux kernel device always has a
     * subsystem (such as "usb" or "pci"), and a device name. The test bed only
     * builds a very simple sysfs structure without nested namespaces, so it
     * requires device names to be unique. Some gudev client programs might make
     * assumptions about the name (e. g. a SCSI disk block device should be called
     * sdaN). A device also has an arbitrary number of sysfs attributes and udev
     * properties; usually you should specify them upon creation, but it is also
     * possible to change them later on with umockdev_testbed_set_attribute() and
     * umockdev_testbed_set_property().
     *
     * Example:
     *   |[
     *   umockdev_testbed_add_device (testbed, "usb", "dev1", NULL,
     *                              "idVendor", "0815", "idProduct", "AFFE", NULL,
     *                              "ID_MODEL", "KoolGadget", NULL);
     *   ]|
     *
     * Returns: The sysfs path for the newly created device. Free with g_free().
     */
    public string? add_device(string subsystem, string name, string? parent, ...)
    {
        string[] attributes = {};
        string[] properties = {};
        int arg_set = 0; /* 0 -> attributes, 1 -> properties */

        /* we iterate arguments until NULL twice; first for the attributes,
         * then for the properties */
        var l = va_list();
        while (true) {
            string? arg = l.arg();
            if (arg == null) {
                if (++arg_set > 1)
                    break;
                else
                    continue;
            }
            if (arg_set == 0)
                attributes += arg;
            else
                properties += arg;
        }

        return this.add_devicev(subsystem, name, parent, attributes, properties);
    }

    /**
     * umockdev_testbed_add_from_string:
     * @self: A #UMockdevTestbed.
     * @data: Description of the device(s) as generated with umockdev-record
     * @error: return location for a GError, or %NULL
     *
     * Add a set of devices to the testbed from a textual description. This reads
     * the format generated by the umockdev-record tool.
     *
     * Each paragraph defines one device. A line starts with a type tag (like 'E'),
     * followed by a colon, followed by either a value or a "key=value" assignment,
     * depending on the type tag. A device description must start with a 'P:' line.
     * Available type tags are:
     * <itemizedlist>
     *   <listitem><type>P:</type> <emphasis>path</emphasis>: device path in sysfs, starting with
     *             <filename>/devices/</filename>; must occur exactly once at the
     *             start of device definition</listitem>
     *   <listitem><type>E:</type> <emphasis>key=value</emphasis>: udev property
     *             </listitem>
     *   <listitem><type>A:</type> <emphasis>key=value</emphasis>: ASCII sysfs
     *             attribute, with backslash-style escaping of \ (\\) and newlines
     *             (\n)</listitem>
     *   <listitem><type>H:</type> <emphasis>key=value</emphasis>: binary sysfs
     *             attribute, with the value being written as continuous hex string
     *             (e. g. 0081FE0A..)</listitem>
     *   <listitem><type>N:</type> <emphasis>devname</emphasis>[=<emphasis>contents</emphasis>]:
     *             device node name (without the <filename>/dev/</filename>
     *             prefix); if <emphasis>contents</emphasis> is given (encoded in a
     *             continuous hex string), it creates a
     *             <filename>/dev/devname</filename> in the mock environment with
     *             the given contents, otherwise the created dev file will be
     *             empty.</listitem>
     *   <listitem><type>S:</type> <emphasis>linkname</emphasis>: device node
     *             symlink (without the <filename>/dev/</filename> prefix); ignored right
     *             now.</listitem>
     * </itemizedlist>
     *
     * Returns: %TRUE on success, %FALSE if the data is invalid and an error
     *          occurred.
     */
    public bool add_from_string (string data) throws UMockdev.Error
    {
        /* lazily initialize the parsing regexps */
        if (this.re_record_val == null)
            this.re_record_val = /^([PS]): (.*)(?>\n|$)/;
        if (this.re_record_keyval == null)
            this.re_record_keyval = /^([EAH]): ([a-zA-Z0-9_:+-]+)=(.*)(?>\n|$)/;
        if (this.re_record_optval == null)
            this.re_record_optval = /^([N]): ([^=\n]+)(?>=([0-9A-F]+))?(?>\n|$)/;

        string cur_data = data;
        while (cur_data[0] != '\0')
            cur_data = this.add_dev_from_string (cur_data);

        return true;
    }

    /**
     * umockdev_testbed_uevent:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @action: "add", "remove", or "change"
     *
     * Generate an uevent for a device.
     */
    public void uevent (string devpath, string action)
    {
        if (this.ev_sender == null) {
            debug("umockdev_testbed_uevent: lazily initializing uevent_sender");
            this.ev_sender = new UeventSender.sender(this.root_dir);
            assert(this.ev_sender != null);
        }
        debug("umockdev_testbed_uevent: sending uevent %s for device %s", action, devpath);
        this.ev_sender.send(devpath, action);
    }

    /**
     * umockdev_testbed_load_ioctl:
     * @self: A #UMockdevTestbed.
     * @dev: Device path (/dev/...) for which to load the ioctl record.
     * @recordfile: Path of the ioctl record file.
     * @error: return location for a GError, or %NULL
     *
     * Load an ioctl record file for a particular device into the testbed.
     * ioctl records can be created with umockdev-record --ioctl.
     * They can optionally be xz compressed to save space (but then are
     * required to have an .xz file name suffix).
     *
     * Returns: %TRUE on success, %FALSE if the data is invalid and an error
     *          occurred.
     */
    public bool load_ioctl (string dev, string recordfile) throws FileError
    {
        string dest = Path.build_filename(this.root_dir, "ioctl", dev);
        string contents;

        assert(DirUtils.create_with_parents(Path.get_dirname(dest), 0755) == 0);

        if (recordfile.has_suffix (".xz")) {
            try {
                int exit;
                Process.spawn_sync (null, {"xz", "-cd", recordfile}, null,
                                    SpawnFlags.SEARCH_PATH,
                                    null,
                                    out contents,
                                    null,
                                    out exit);
                assert (exit == 0);
            } catch (SpawnError e) {
                error ("Cannot call xz to decompress %s: %s", recordfile, e.message);
            }
        } else
            assert (FileUtils.get_contents(recordfile, out contents));

        return FileUtils.set_contents(dest, contents);
    }

    private string add_dev_from_string(string data) throws UMockdev.Error
    {
        char type;
        string? key;
        string? val;
        string? devpath = null;
        string? subsystem = null;
        string cur_data = data;

        cur_data = this.record_parse_line(cur_data, out type, out key, out devpath);
        if (cur_data == null || type != 'P')
            throw new UMockdev.Error.PARSE("device descriptions must start with a \"P: /devices/path/...\" line");
        if (!devpath.has_prefix("/devices/"))
            throw new UMockdev.Error.VALUE("invalid device path '%s': must start with /devices/",
                                           devpath);
        debug("parsing device description for %s", devpath);

        string[] attrs = {};
        string[] binattrs = {}; /* hex encoded values */
        string[] props = {};

        /* scan until we see an empty line */
        while (cur_data.length > 0 && cur_data[0] != '\n') {
            cur_data = this.record_parse_line(cur_data, out type, out key, out val);
            if (cur_data == null)
                throw new UMockdev.Error.PARSE("malformed attribute or property line in description of device %s",
                                               devpath);
            //debug("umockdev_testbed_add_dev_from_string: type %c key >%s< val >%s<", type, key, val);
            switch (type) {
                case 'H':
                    binattrs += key;
                    binattrs += val;
                    break;

                case 'A':
                    attrs += key;
                    attrs += val.compress();
                    break;

                case 'E':
                    props += key;
                    props += val;
                    if (key == "SUBSYSTEM") {
                        if (subsystem != null)
                            throw new UMockdev.Error.VALUE("duplicate SUBSYSTEM property in description of device %s",
                                                           devpath);
                        subsystem = val;
                    }
                    break;

                case 'P':
                    throw new UMockdev.Error.PARSE("invalid P: line in description of device %s", devpath);

                case 'N':
                    /* create directory of file */
                    string path = Path.build_filename(this.root_dir, "dev", key);
                    assert (DirUtils.create_with_parents(Path.get_dirname(path), 0755) == 0);
                    uint8[] contents = {};
                    if (val != null)
                        contents = decode_hex(val);
                    try {
                        FileUtils.set_data(path, contents);
                    } catch (FileError e) {
                        stderr.printf("Cannot create dev node file: %s\n", e.message);
                        Process.abort();
                    }
                    break;

                case 'S':
                    /* TODO: ignored for now */
                    break;

                default:
                    assert_not_reached();
            }
        }

        if (subsystem == null)
            throw new UMockdev.Error.VALUE("missing SUBSYSTEM property in description of device %s",
                                       devpath);
        debug("creating device %s (subsystem %s)", devpath, subsystem);
        string syspath = this.add_devicev(subsystem,
                                          devpath.substring(9), // chop off "/devices/"
                                          null, attrs, props);

        /* add binary attributes */
        for (int i = 0; i < binattrs.length; i += 2)
            this.set_attribute_binary (syspath, binattrs[i], decode_hex(binattrs[i+1]));

        /* skip over multiple blank lines */
        while (cur_data[0] != '\0' && cur_data[0] == '\n')
            cur_data = cur_data.next_char();

        return cur_data;
    }

    /**
     * umockdev_testbed_record_parse_line:
     * @data: String to parse
     * @type: Pointer to a gchar which will get the line type (one of P, N,
     *        S, E, or H)
     * @key:  Pointer to a string which will get the key name; this will be
     *        set to %NULL for line types which do not have a key (P, N, S). You
     *        need to free this with g_free().
     * @value: Pointer to a string which will get the value. You need to
     *         free this with g_free().
     *
     * Parse one line from a device record file.
     *
     * Returns: Pointer to the next line start in @data, or %NULL if the first line
     * is not valid.
     */
    private string? record_parse_line (string data, out char type, out string? key, out string? val)
    {
        MatchInfo match;

        if (this.re_record_val.match(data, 0, out match)) {
            key = null;
            val = match.fetch(2);
        } else if (this.re_record_keyval.match(data, 0, out match)) {
            key = match.fetch(2);
            val = match.fetch(3);
            assert(val != null);
        } else if (this.re_record_optval.match(data, 0, out match)) {
            key = match.fetch(2);
            val = match.fetch(3);
        } else {
            debug("record_parse_line: >%s< does not match anything, failing", data);
            type = '\0';
            key = null;
            val = null;
            return null;
        }

        string type_str = match.fetch(1);
        assert(type_str != null);
        type = type_str[0];

        int end_pos;
        assert(match.fetch_pos(0, null, out end_pos));
        return data.substring(end_pos);
    }

    private string root_dir;
    private string sys_dir;
    private Regex re_record_val;
    private Regex re_record_keyval;
    private Regex re_record_optval;
    private UeventSender.sender? ev_sender = null;
}


// Recursively remove a directory and all its contents.
private static void
remove_dir (string path)
{
    if (FileUtils.test(path, FileTest.IS_DIR) && !FileUtils.test(path, FileTest.IS_SYMLINK)) {
        Dir d;
        try {
            d = Dir.open(path, 0);
        } catch (FileError e) {
            warning("cannot open: %s: %s", path, e.message);
            return;
        }

        string name;
        while ((name = d.read_name()) != null)
            remove_dir (Path.build_filename(path, name));
    }

    if (FileUtils.remove(path) < 0)
        warning("cannot remove %s: %s", path, strerror(errno));
}

private static string
make_dotdots (string path)
{
    int offset = 0;
    uint count = 0;

    /* count slashes in devpath */
    while ((offset = path.index_of_char('/', offset)) >= 0) {
        ++count;
        ++offset; /* advance beyond */
    }

    /* we need one .. less than count */
    string dots = "";
    while (count > 1) {
        dots += "../";
        --count;
    }
    return dots;
}

private static uint8
hexdigit (char c)
{
    return (c >= 'a') ? (c - 'a' + 10) :
           (c >= 'A') ? (c - 'A' + 10) :
           (c - '0');
}

private static uint8[]
decode_hex (string data) throws UMockdev.Error
{
    /* hex digits must come in pairs */
    if (data.length % 2 != 0)
        throw new UMockdev.Error.PARSE("malformed hexadecimal value: %s", data);
    var len = data.length;
    uint8[] bin = new uint8[len / 2];

    for (uint i = 0; i < bin.length; ++i)
        bin[i] = (hexdigit (data[i*2]) << 4) | hexdigit (data[i*2+1]);

    return bin;
}

public errordomain Error {
   PARSE,
   VALUE,
}

} /* namespace */
