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
 * @title: umockdev
 * @short_description: Build a test bed for testing software that handles Linux
 * hardware devices.
 *
 * Please see README.rst about an overview of the parts of umockdev, and how
 * they fit together.
 */

/**
 * UMockdevTestbed:
 *
 * The #UMockdevTestbed class builds a temporary sandbox for mock devices.
 * You can add a number of devices including arbitrary sysfs attributes and
 * udev properties, and then run your software in that test bed that is
 * independent of the actual hardware it is running on.  With this you can
 * simulate particular hardware in virtual environments up to some degree,
 * without needing any particular privileges or disturbing the whole system.
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
    /**
     * umockdev_testbed_new:
     *
     * Create a new #UMockdevTestbed object. This is initially empty, call
     * methods like #umockdev_testbed_add_device or
     * #umockdev_testbed_add_from_string to fill it.
     *
     * Returns: The newly created #UMockdevTestbed object.
     */
    public Testbed()
    {
        try {
            this.root_dir = DirUtils.make_tmp("umockdev.XXXXXX");
        } catch (FileError e) {
            error("Cannot create temporary directory: %s", e.message);
        }
        this.sys_dir = Path.build_filename(this.root_dir, "sys");
        DirUtils.create(this.sys_dir, 0755);

        this.dev_fd = new HashTable<string, int> (str_hash, str_equal);
        this.dev_script_runner = new HashTable<string, ScriptRunner> (str_hash, str_equal);

        Environment.set_variable("UMOCKDEV_DIR", this.root_dir, true);
        debug("Created udev test bed %s", this.root_dir);
    }

    ~Testbed()
    {
        // merely calling remove_all() does not invoke ScriptRunner dtor, so stop manually
        foreach (unowned ScriptRunner r in this.dev_script_runner.get_values())
            r.stop ();
        this.dev_script_runner.remove_all();

        foreach (int fd in this.dev_fd.get_values()) {
            debug ("closing master pty fd %i for emulated device", fd);
            Posix.close (fd);
        }

        if (this.socket_server != null) {
            debug ("shutting down socket server thread");
            this.socket_server.stop ();
            this.socket_server = null;
        }

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

    /**
     * umockdev_testbed_set_attribute:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Attribute name
     * @value: Attribute string value
     *
     * Set a text sysfs attribute for a device.
     */
    public void set_attribute(string devpath, string name, string value)
    {
        this.set_attribute_binary(devpath, name, value.data);
    }

    /**
     * umockdev_testbed_set_attribute_binary:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Attribute name
     * @value: Attribute binary value
     * @value_length1: Length of @value in bytes.
     *
     * Set a binary sysfs attribute for a device.
     */
    public void set_attribute_binary(string devpath, string name, uint8[] value)
    {
        try {
            FileUtils.set_data(Path.build_filename(this.root_dir, devpath, name), value);
        } catch (FileError e) {
            error("Cannot write attribute file: %s", e.message);
        }
    }

    /**
     * umockdev_testbed_set_attribute_int:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Attribute name
     * @value: Attribute integer value
     *
     * Set an integer sysfs attribute for a device.
     */
    public void set_attribute_int(string devpath, string name, int value)
    {
        this.set_attribute(devpath, name, value.to_string());
    }

    /**
     * umockdev_testbed_set_attribute_hex:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Attribute name
     * @value: Attribute integer value
     *
     * Set an integer sysfs attribute for a device. Set an integer udev
     * property for a device. @value is interpreted as a hexadecimal number.
     * For example, for value==31 this sets the attribute contents to "1f".
     */
    public void set_attribute_hex(string devpath, string name, uint value)
    {
        this.set_attribute(devpath, name, "%x".printf(value));
    }

    /**
     * umockdev_testbed_set_attribute_link:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Attribute name
     * @value: Attribute link target value
     *
     * Set a symlink sysfs attribute for a device; this is primarily important
     * for setting "driver" links.
     */
    public void set_attribute_link(string devpath, string name, string value)
    {
        var path = Path.build_filename(this.root_dir, devpath, name);
        if (FileUtils.symlink(value, path) < 0) {
            error("Cannot create symlink %s: %s", path, strerror(errno));
        }
    }

    /**
     * umockdev_testbed_set_property:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Property name
     * @value: Property string value
     *
     * Set a string udev property for a device.
     */
    public new void set_property(string devpath, string name, string value)
    {
        var uevent_path = Path.build_filename(this.root_dir, devpath, "uevent");
        string props = "";
        string real_value;

        /* the kernel sets DEVNAME without prefix */
        if (name == "DEVNAME" && value.has_prefix("/dev/"))
            real_value = value.substring(5);
        else
            real_value = value;

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
                    props += prefix + real_value + "\n";
                } else {
                    props += line + "\n";
                }
            }
            inp.close();

            /* if property name does not yet exist, append it */
            if (!existing)
                props += prefix + real_value + "\n";

            /* write it back */
            FileUtils.set_data(uevent_path, props.data);
        } catch (GLib.Error e) {
            error("Cannot update uevent file: %s", e.message);
        }
    }

    /**
     * umockdev_testbed_set_property_int:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Property name
     * @value: Property integer value
     *
     * Set an integer udev property for a device.
     */
    public void set_property_int(string devpath, string name, int value)
    {
        this.set_property(devpath, name, value.to_string());
    }

    /**
     * umockdev_testbed_set_property_hex:
     * @self: A #UMockdevTestbed.
     * @devpath: The full device path, as returned by #umockdev_testbed_add_device()
     * @name: Property name
     * @value: Property integer value
     *
     * Set an integer udev property for a device. @value is interpreted as a
     * hexadecimal number. For example, for value==31 this sets the property's
     * value to "1f".
     */
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
        string? dev_node = null;

        if (parent != null) {
            if (!parent.has_prefix("/sys/")) {
                critical("add_devicev(): parent device %s does not start with /sys/", parent);
                return null;
            }
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
        string dev_path_no_sys = dev_path.substring(dev_path.index_of("/devices/"));
        assert(FileUtils.symlink(Path.build_filename(make_dotdots(dev_path), "class", subsystem),
                                 Path.build_filename(dev_dir, "subsystem")) == 0);

        /* device symlink from class/; skip directories in name; this happens
         * when being called from add_from_string() when the parent devices do
         * not exist yet */
        assert(FileUtils.symlink(Path.build_filename("..", "..", dev_path_no_sys),
                                 Path.build_filename(class_dir, Path.get_basename(name))) == 0);

        /* bus symlink */
        if (subsystem == "usb" || subsystem == "pci") {
            class_dir = Path.build_filename(this.sys_dir, "bus", subsystem, "devices");
            assert(DirUtils.create_with_parents(class_dir, 0755) == 0);
            assert(FileUtils.symlink(Path.build_filename("..", "..", "..", dev_path_no_sys),
                                     Path.build_filename(class_dir, Path.get_basename(name))) == 0);
        }

        /* properties; they go into the "uevent" sysfs attribute */
        string props = "";
        for (int i = 0; i < properties.length - 1; i += 2) {
            /* the kernel sets DEVNAME without prefix */
            if (properties[i] == "DEVNAME" && properties[i+1].has_prefix("/dev/")) {
                dev_node = properties[i+1].substring(5);
                props += "DEVNAME=" + dev_node + "\n";
            } else
                props += properties[i] + "=" + properties[i+1] + "\n";
        }
        if (properties.length % 2 != 0)
            warning("add_devicev: Ignoring property key '%s' without value", properties[properties.length-1]);
        this.set_attribute(dev_path, "uevent", props);

        /* attributes */
        for (int i = 0; i < attributes.length - 1; i += 2) {
            this.set_attribute(dev_path, attributes[i], attributes[i+1]);
            if (attributes[i] == "dev" && dev_node != null) {
                /* put the major/minor information into /dev for our preload */
                string infodir = Path.build_filename(this.root_dir, "dev", ".node");
                DirUtils.create_with_parents(infodir, 0755);
                assert(FileUtils.symlink(attributes[i+1],
                                         Path.build_filename(infodir, dev_node.replace("/", "_"))) == 0);

                /* create a /sys/dev link for it, like in real sysfs */
                string sysdev_dir = Path.build_filename(this.sys_dir, "dev",
                    (dev_path.contains("/block/") ? "block" : "char"));
                if (DirUtils.create_with_parents(sysdev_dir, 0755) != 0)
                    error("cannot create dir '%s': %s", sysdev_dir, strerror(errno));
                string dest = Path.build_filename(sysdev_dir, attributes[i+1]);
                if (!FileUtils.test(dest, FileTest.EXISTS)) {
                    if (FileUtils.symlink("../../" + dev_path.substring(5), dest) < 0)
                        error("add_device %s: failed to symlink %s to %s: %s\n", name, dest,
                              dev_path.substring(5), strerror(errno));
                }
            }
        }
        if (attributes.length % 2 != 0)
            warning("add_devicev: Ignoring attribute key '%s' without value", attributes[attributes.length-1]);

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
     * umockdev_testbed_remove_device:
     * @self: A #UMockdevTestbed.
     * @syspath: Sysfs path of device
     *
     * Remove a device from the testbed. This removes the sysfs directory, the
     * /sys/class/ link, the device node, and all other information related to
     * it. Note that this will also remove all child devices (i. e.
     * subdirectories of @syspath).
     */
    public void remove_device (string syspath)
    {
        string real_path = Path.build_filename(this.root_dir, syspath);
        string devname = Path.get_basename(syspath);

        if (!FileUtils.test(real_path, FileTest.IS_DIR)) {
            critical("umockdev_testbed_remove_device(): device %s does not exist", syspath);
            return;
        }

        string subsystem;

        try {
            subsystem = Path.get_basename(
                FileUtils.read_link(Path.build_filename(real_path, "subsystem")));
        } catch (FileError e) {
            critical("umockdev_testbed_remove_device(): cannot determine subsystem of %s: %s",
                     syspath, e.message);
            return;
        }

        // /dev and pointers to it
        try {
            string dev_maj_min;
            FileUtils.get_contents(Path.build_filename(real_path, "dev"), out dev_maj_min);
            FileUtils.unlink(Path.build_filename(this.sys_dir, "dev",
                        (syspath.contains("/block/") ? "block" : "char"), dev_maj_min));

            string? dev_node = find_devnode(real_path);
            if (dev_node != null) {
                string real_node = Path.build_filename(this.root_dir, dev_node);
                FileUtils.unlink(real_node);
                DirUtils.remove(Path.get_dirname(real_node));
                FileUtils.unlink(Path.build_filename(this.root_dir, "dev", ".node", dev_node.substring(5).replace("/", "_")));
            }
        } catch (FileError e) {}

        // class symlink
        FileUtils.unlink(Path.build_filename(this.sys_dir, "class", subsystem, devname));
        DirUtils.remove(Path.build_filename(this.sys_dir, "class", subsystem));
        // bus symlink
        if (subsystem == "usb" || subsystem == "pci") {
            FileUtils.unlink(Path.build_filename(this.sys_dir, "bus", subsystem, "devices", devname));
            DirUtils.remove(Path.build_filename(this.sys_dir, "bus", subsystem, "devices"));
            DirUtils.remove(Path.build_filename(this.sys_dir, "bus", subsystem));
        }
        // sysfs dir
        remove_dir(real_path, true);
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
     *             the given contents, otherwise the created dev file will be a
     *             pty; see #umockdev_testbed_get_dev_fd for details.</listitem>
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
        try {
            if (this.re_record_val == null)
                this.re_record_val = new Regex("^([PS]): (.*)(?>\n|$)");
            if (this.re_record_keyval == null)
                this.re_record_keyval = new Regex("^([EAHL]): ([^=\n]+)=(.*)(?>\n|$)");
            if (this.re_record_optval == null)
                this.re_record_optval = new Regex("^([N]): ([^=\n]+)(?>=([0-9A-F]+))?(?>\n|$)");
        } catch (RegexError e) {
            error("Internal error, cannot create regex: %s", e.message);
        }

        string cur_data = data;
        while (cur_data[0] != '\0')
            cur_data = this.add_dev_from_string (cur_data);

        return true;
    }

    /**
     * umockdev_testbed_add_from_file:
     * @self: A #UMockdevTestbed.
     * @path: Path to file with description of the device(s) as generated with umockdev-record
     * @error: return location for a GError, or %NULL
     *
     * Add a set of devices to the testbed from a textual description. This
     * reads a file with the format generated by the umockdev-record tool, and
     * is mostly a convenience wrapper around
     * @umockdev_testbed_add_from_string.
     *
     * Returns: %TRUE on success, %FALSE if the @path cannot be read or thhe
     *          data is invalid and an error occurred.
     */
    public bool add_from_file (string path) throws UMockdev.Error, FileError
    {
        string contents;

        FileUtils.get_contents(path, out contents);
        return this.add_from_string(contents);
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

    /**
     * umockdev_testbed_load_script:
     * @self: A #UMockdevTestbed.
     * @dev: Device path (/dev/...) for which to load the script record.
     * @recordfile: Path of the script record file.
     * @error: return location for a GError, or %NULL
     *
     * Load a script record file for a particular device into the testbed.
     * script records can be created with umockdev-record --script.
     *
     * Returns: %TRUE on success, %FALSE if @recordfile is invalid and an error
     *          occurred.
     */
    public bool load_script (string dev, string recordfile)
        throws FileError
    {
        assert (!this.dev_script_runner.contains (dev));

        int fd = this.get_dev_fd (dev);
        if (fd < 0)
            throw new FileError.INVAL (dev + " is not a device suitable for scripts");

        this.dev_script_runner.insert (dev, new ScriptRunner (dev, recordfile, fd));
        return true;
    }

    /**
     * umockdev_testbed_load_socket_script:
     * @self: A #UMockdevTestbed.
     * @path: Unix socket path
     * @type: Unix socket type (#SOCK_STREAM, #SOCK_DGRAM)
     * @recordfile: Path of the script record file.
     * @error: return location for a GError, or %NULL
     *
     * Add an Unix socket to the testbed that is backed by a recorded script.
     * Clients can connect to the socket using @path (i. e. without the testbed
     * prefix).
     *
     * Returns: %TRUE on success, %FALSE if the @path or @type are
     *          invalid and an error occurred.
     */
    public bool load_socket_script (string path, int type, string recordfile) throws FileError
    {
        int fd = Posix.socket (Posix.AF_UNIX, type, 0);
        if (fd < 0)
            throw new FileError.INVAL ("Cannot create socket type %i: %s".printf(
                                       type, strerror(errno)));

        string real_path = Path.build_filename (this.root_dir, path);
        if (DirUtils.create_with_parents(Path.get_dirname(real_path), 0755) != 0)
            throw new FileError.INVAL ("Cannot create socket path: %s".printf(
                                       strerror(errno)));

        // start thread to accept client connections at first socket creation
        if (this.socket_server == null)
            this.socket_server = new SocketServer ();

        this.socket_server.add (real_path, fd, recordfile);
        return true;
    }

    private string add_dev_from_string (string data) throws UMockdev.Error
    {
        char type;
        string? key;
        string? val;
        string? devpath = null;
        string? subsystem = null;
        string cur_data = data;
        string? devnode_path = null;
        uint8[] devnode_contents = {};

        cur_data = this.record_parse_line(cur_data, out type, out key, out devpath);
        if (cur_data == null || type != 'P')
            throw new UMockdev.Error.PARSE("device descriptions must start with a \"P: /devices/path/...\" line");
        if (!devpath.has_prefix("/devices/"))
            throw new UMockdev.Error.VALUE("invalid device path '%s': must start with /devices/",
                                           devpath);
        debug("parsing device description for %s", devpath);

        string[] attrs = {};
        string[] binattrs = {}; /* hex encoded values */
        string[] linkattrs = {};
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

                case 'L':
                    linkattrs += key;
                    linkattrs += val;
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
                    devnode_path = Path.build_filename(this.root_dir, "dev", key);
                    if (val != null)
                        devnode_contents = decode_hex(val);
                    break;

                case 'S':
                    /* TODO: ignored for now */
                    break;

                default:
                    throw new UMockdev.Error.PARSE("Unknown line type '%c'\n", type);
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

        /* add link attributes */
        for (int i = 0; i < linkattrs.length; i += 2)
            this.set_attribute_link (syspath, linkattrs[i], linkattrs[i+1]);

        /* create fake device node */
        if (devnode_path != null)
            this.create_node_for_device(subsystem, devnode_path, devnode_contents);

        /* skip over multiple blank lines */
        while (cur_data[0] != '\0' && cur_data[0] == '\n')
            cur_data = cur_data.next_char();

        return cur_data;
    }

    private void
    create_node_for_device (string subsystem, string node_path, uint8[] node_contents)
        throws UMockdev.Error
    {
        assert (DirUtils.create_with_parents(Path.get_dirname(node_path), 0755) == 0);

        // for pre-defined contents, block, and USB devices we create a normal file
        if (node_contents.length > 0 || subsystem == "block" || subsystem == "usb") {
            try {
                debug ("create_node_for_device: creating file device %s", node_path);
                FileUtils.set_data(node_path, node_contents);
                /* set sticky bit on block devices, to indicate proper
                 * stat() faking to our preload lib */
                if (subsystem == "block")
                    FileUtils.chmod(node_path, 01644);
            } catch (FileError e) {
                error("Cannot create dev node file: %s", e.message);
            }

            return;
        }

        // otherwise we create a PTY
        int ptym, ptys;
        char[] ptyname_array = new char[8192];
        assert (Linux.openpty (out ptym, out ptys, ptyname_array, null, null) == 0);
        string ptyname = (string) ptyname_array;
        debug ("create_node_for_device: creating pty device %s: got pty %s", node_path, ptyname);
        Posix.close (ptys);

        // disable echo, canonical mode, and line ending translation by
        // default, as that's usually what we want
        Posix.termios ios;
        assert (Posix.tcgetattr (ptym, out ios) == 0);
        ios.c_iflag &= ~(Posix.IGNCR | Posix.INLCR | Posix.ICRNL);
        ios.c_oflag &= ~(Posix.ONLCR | Posix.OCRNL);
        ios.c_lflag &= ~(Posix.ICANON | Posix.ECHO);
        assert (Posix.tcsetattr (ptym, Posix.TCSANOW, ios) == 0);

        assert (FileUtils.symlink (ptyname, node_path) == 0);

        // store ptym for controlling the master end
        string devname = node_path.substring (this.root_dir.length);
        assert (!this.dev_fd.contains (devname));
        this.dev_fd.insert (devname, ptym);
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

    /**
     * umockdev_testbed_disable:
     * @self: A #UMockdevTestbed.
     *
     * Disable the testbed.  This can be used for temporarily switching back to
     * the real /sys and /dev without having to destroy or change $UMOCKDEV_DIR
     * and the #UMockdevTestbed instance. Use @umockdev_testbed_enable() to
     * re-enable the testbed.
     */
    public void disable()
    {
        FileStream.open(Path.build_filename(this.root_dir, "disabled"), "w");
    }

    /**
     * umockdev_testbed_enable:
     * @self: A #UMockdevTestbed.
     *
     * Re-enable the testbed after @@umockdev_testbed_disable().
     */
    public void enable()
    {
        FileUtils.remove(Path.build_filename(this.root_dir, "disabled"));
    }

    /**
     * umockdev_testbed_clear:
     * @self: A #UMockdevTestbed.
     *
     * Remove all added devices from testbed directory.  After that, the
     * umockdev root directory will be in the same state as directly after the
     * constructor.
     */
    public void clear()
    {
        remove_dir (this.root_dir, false);
        // /sys should always exist
        DirUtils.create(this.sys_dir, 0755);
    }

    /**
     * umockdev_testbed_get_dev_fd:
     * @self: A #UMockdevTestbed.
     * @devnode: Device node name ("/dev/...")
     *
     * Simulated devices without a pre-defined contents are backed by a
     * stream-like device node (PTY). Return the file descriptor
     * for accessing their "master" side, i. e. the end that gets
     * controlled by test suites. The tested program opens the "slave" side,
     * which is just openening the device specified by @devnode, e. g.
     * /dev/ttyUSB2. Once that happened, your test can directly communicate
     * with the tested program over that descriptor.
     *
     * Returns: File descriptor for communicating with clients that connect to
     *           @devnode, or -1 if @devnode does not exist or is not a
     *           simulated stream device. This must not be closed!
     */
    public int get_dev_fd(string devnode)
    {
        // Note: the more efficient comparison of get() against null doesn't
        // yet work with vala 0.16
        if (this.dev_fd.contains (devnode))
            return this.dev_fd.get (devnode);
        else
            return -1;
    }

    private string root_dir;
    private string sys_dir;
    private Regex re_record_val;
    private Regex re_record_keyval;
    private Regex re_record_optval;
    private UeventSender.sender? ev_sender = null;
    private HashTable<string,int> dev_fd;
    private HashTable<string,ScriptRunner> dev_script_runner;
    private SocketServer socket_server = null;

}


// Recursively remove a directory and all its contents.
private static void
remove_dir (string path, bool remove_toplevel=true)
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
            remove_dir (Path.build_filename(path, name), true);
    }

    if (remove_toplevel)
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

private static string?
find_devnode (string devpath)
{
    string? devname = null;

    // read current properties from the uevent file
    File f = File.new_for_path(Path.build_filename(devpath, "uevent"));
    try {
        var inp = new DataInputStream(f.read());
        string line;
        size_t len;
        while ((line = inp.read_line(out len)) != null) {
            if (line.has_prefix("DEVNAME=")) {
                devname = line.substring(8);
                if (!devname.has_prefix("/dev/"))
                    devname = "/dev/" + devname;
                break;
            }
        }
        inp.close();
    } catch (GLib.Error e) {
        warning("Cannot read uevent file: %s\n", e.message);
    }
    return devname;
}

private class ScriptRunner {

    public ScriptRunner (string device, string script_file, int fd) throws FileError
    {
        this.script = FileStream.open (script_file, "r");
        if (this.script == null)
            throw new FileError.FAILED ("Cannot open script record file " + script_file);

        this.device = device;
        this.script_file = script_file;
        this.fd = fd;
        this.running = true;

        this.thread = new Thread<void*> (device, this.run);
    }

    ~ScriptRunner ()
    {
        this.stop ();
    }

    public void stop ()
    {
        if (!this.running)
            return;

        debug ("Stopping script runner for %s: joining thread", this.device);
        this.running = false;
        this.thread.join ();
    }

    private void* run ()
    {
        char op;
        uint32 delta;
        uint8[] data;

        debug ("ScriptRunner[%s]: start", this.device);

        while (this.running) {
            data = this.next_line (out op, out delta);

            switch (op) {
                case 'r':
                    debug ("ScriptRunner[%s]: read op; sleeping %" + uint32.FORMAT + " ms",
                           this.device, delta);
                    Thread.usleep (delta * 1000);
                    debug ("ScriptRunner[%s]: read op after sleep; writing data '%s'", this.device, encode(data));
                    ssize_t l = Posix.write (this.fd, data, data.length);
                    if (l < 0)
                        error ("ScriptRunner[%s]: write failed: %s", this.device, strerror (errno));
                    assert (l == data.length);
                    break;

                case 'w':
                    debug ("ScriptRunner[%s]: write op, data '%s'", this.device, encode(data));
                    this.op_write (data, delta);
                    break;

                case 'Q':
                    this.running = false;
                    break;

                case 'f':
                    if (delta > 100)
                        error ("ScriptRunner[%s]: fuzz value %u is invalid (must be between 0 and 100)",
                               this.device, delta);
                    this.fuzz = delta;
                    debug ("ScriptRunner[%s]: setting fuzz level to %u%%", this.device, this.fuzz);
                    break;

                default:
                    debug ("ScriptRunner[%s]: got unknown line op %c, ignoring", this.device, op);
                    break;
            }
        }

        debug ("ScriptRunner[%s]: not running any more, ending thread", this.device);
        return null;
    }

    private uint8[] next_line (out char op, out uint32 delta)
    {
        // read operation code; skip empty lines and comments
        int c;
        for (;;) {
            c = this.script.getc ();
            if (c == FileStream.EOF) {
                debug ("ScriptRunner[%s]: end of script %s, closing", this.device, this.script_file);
                op = 'Q';
                delta = 0;
                return {};
            } else if (c == '#') {
                assert (this.script.read_line () != null);
            } else if (c != '\n') {
                op = (char) c;
                break;
            }
        }

        var cur_pos = this.script.tell ();
        if (this.script.getc () != ' ')
            error ("Missing space after operation code in %s at position %li", this.script_file, cur_pos);

        // read time delta
        cur_pos = this.script.tell ();
        if (this.script.scanf ("%" + uint32.FORMAT + " ", out delta) != 1)
            error ("Cannot parse time in %s at position %li", this.script_file, cur_pos);

        // remainder of the line is the data
        string? line = this.script.read_line ();
        assert (line != null);

        return decode (line);
    }

    private void op_write (uint8[] data, uint32 delta)
    {
        Posix.fd_set fds;
        Posix.timeval timeout = {0, 200000};
        size_t offset = 0;
        uint8[] buf = new uint8 [data.length];

        // a recorded block might be actually written in multiple smaller
        // chunks
        while (this.running && offset < data.length) {
            Posix.FD_ZERO (out fds);
            Posix.FD_SET (this.fd, ref fds);
            int res = Posix.select (this.fd + 1, &fds, null, null, timeout);
            if (res < 0) {
                if (errno == Posix.EINTR)
                    continue;
                error ("ScriptRunner op_write[%s]: select() failed: %s",
                       this.device, strerror (errno));
            }

            if (res == 0) {
                debug ("ScriptRunner[%s]: timed out on read operation on expected block '%s', trying again",
                       this.device, encode(data[offset:data.length]));
                continue;
            }

            ssize_t len = Posix.read (this.fd, buf, data.length - offset);
            // if the client closes the fd, we'll get EIO
            if (len <= 0) {
                debug ("ScriptRunner[%s]: got failure or EOF on read operation on expected block '%s', resetting",
                       this.device, encode(data[offset:data.length]));
                this.script.seek (0, FileSeek.SET);
                return;
            }

            if (this.fuzz == 0) {
                if (Posix.memcmp (buf, data[offset:data.length], len) != 0)
                    error ("ScriptRunner op_write[%s]: data mismatch; got block '%s' (%" + ssize_t.FORMAT +
                           " bytes), expected block '%s'",
                           this.device, encode(buf), len, encode(data[offset:offset+len]));
            } else {
                uint d = hamming (buf, data[offset:offset+len]);
                if (d * 100 > this.fuzz * len) {
                    error ("ScriptRunner op_write[%s]: data mismatch; got block '%s' (%" + ssize_t.FORMAT +
                           " bytes), expected block '%s', difference %u%% > fuzz level %u%%",
                           this.device, encode(buf), len, encode(data[offset:offset+len]),
                           (uint) (d * 1000 / len + 5) / 10, this.fuzz);
                } /* else {
                    debug ("ScriptRunner op_write[%s]: data matches: got block '%s' (%" + ssize_t.FORMAT +
                                   " bytes), expected block '%s', difference %u%% <= fuzz level %u%%\n",
                                   this.device, encode(buf), len, encode(data[offset:offset+len]),
                                   (d * 1000 / len + 5) / 10, this.fuzz);
                } */
            }

            offset += len;
            debug ("ScriptRunner[%s]: op_write, got %" + ssize_t.FORMAT + " bytes; offset: %" +
                   size_t.FORMAT + ", full block size %i",
                   this.device, len, offset, data.length);
        }
    }

    private static uint8[] decode (string quoted)
    {
        uint8[] data = {};
        for (int i = 0; i < quoted.length; ++i) {
            if (quoted.data[i] == '^') {
                assert (i + 1 < quoted.length);
                data += (quoted.data[i+1] == '`') ? '^' : (quoted.data[i+1] - 64);
                ++i;
            } else
                data += quoted.data[i];
        }
        return data;
    }

    private static string encode (uint8[] data)
    {
        uint8[] quoted = {};
        for (int i = 0; i < data.length; ++i) {
            if (data[i] < 32) {
                quoted += '^';
                quoted += data[i] + 64;
            } else if (data[i] == '^') {
                /* we cannot encode ^ as ^^, as we need that for 0x1E already; so
                 * take the next free code which is 0x60 */
                quoted += '^';
                quoted += '`';
            } else
                quoted += data[i];
        }

        // null terminator
        quoted += 0;
        return (string) quoted;
    }

    private static uint hamming (uint8[] d1, uint8[] d2)
    {
        assert (d1.length == d2.length);
        uint d = 0;
        uint i;

        // use xor instead of "if !=" to avoid branching
        for (i = 0; i < d1.length; ++i)
            d += (uint) ((d1[i] ^ d2[i]) > 0);
        return d;
    }

    public string device { get; private set; }
    private string script_file;
    private Thread<void*> thread;
    private FileStream script;
    private int fd;
    private bool running;
    private uint fuzz = 0;
}


private class SocketServer {

    public SocketServer ()
    {
        this.running = true;
        this.socket_scriptfile = new HashTable<string, string> (str_hash, str_equal);
        this.script_runners = new HashTable<string, ScriptRunner> (str_hash, str_equal);
        this.thread = new Thread<void*> ("SocketServer", this.run);

        // we use a control pipe which we trigger when adding or stopping, to
        // interrupt the select()
        var fds = new int[2];
        assert (Posix.pipe (fds) == 0);
        this.ctrl_r = fds[0];
        this.ctrl_w = fds[1];
    }

    ~SocketServer ()
    {
        this.stop ();
    }

    public void stop ()
    {
        if (!this.running)
            return;

        this.running = false;

        // wake up the select() in our thread
        debug ("Stopping SocketServer: signalling thread");
        char b = '1';
        assert (Posix.write (this.ctrl_w, &b, 1) == 1);

        // merely calling remove_all() does not invoke ScriptRunner dtor, so stop manually
        foreach (unowned ScriptRunner r in this.script_runners.get_values())
            r.stop ();
        this.script_runners.remove_all();

        debug ("Stopping SocketServer: joining thread");
        this.thread.join ();
    }

    public void add (string sock_path, int fd, string record_file)
    {
        try {
            var s = new Socket.from_fd (fd);
            assert (s != null);
            assert (s.bind (new UnixSocketAddress (sock_path), true));
            assert (s.listen ());
            this.listen_sockets += s;
        } catch (GLib.Error e) {
            error ("load_socket_script(): cannot create Socket: %s", e.message);
        }

        debug ("SocketServer.add: Created socket path %s, fd %i", sock_path, fd);

        this.socket_scriptfile.insert (sock_path, record_file);

        // wake up the select() in our thread
        char b = '1';
        assert (Posix.write (this.ctrl_w, &b, 1) == 1);
    }

    private void* run ()
    {
        debug ("starting SocketServer thread");
        while (this.running) {
            // wait for incoming connects
            Posix.fd_set fds;
            Posix.FD_ZERO (out fds);
            Posix.FD_SET (this.ctrl_r, ref fds);
            int max = this.ctrl_r;
            foreach (unowned Socket s in this.listen_sockets) {
                Posix.FD_SET (s.fd, ref fds);
                if (s.fd > max)
                    max = s.fd;
            }
            /* ideally we'd use an infinite timeout here; but Vala
             * currently doesn't allow that, and also it's a good defense
             * against infinite hangs */
            int res = Posix.select (max + 1, &fds, null, null, {0, 500000});
            if (res < 0) {
                if (errno == Posix.EINTR)
                    continue;
                error ("socket server thread: select() failed: %s", strerror (errno));
            }
            if (res == 0)
                continue;  // timeout

            // if we got triggered by our control fd, consume the data
            if (Posix.FD_ISSET (this.ctrl_r, fds) > 0) {
                debug ("socket server thread: woken up by control fd");
                char buf;
                assert (Posix.read (this.ctrl_r, &buf, 1) == 1);
                continue;
            }

            debug ("socket server thread: select() got requests");

            // accept the incoming connections and create ScriptRunners for them
            foreach (unowned Socket s in this.listen_sockets) {
                if (Posix.FD_ISSET (s.fd, fds) > 0) {
                    int fd = Posix.accept (s.fd, null, null);
                    if (fd < 0)
                        error ("socket server thread: accept() failed: %s", strerror (errno));
                    string sock_path = null;
                    try {
                        sock_path = ((UnixSocketAddress) s.get_local_address()).path;
                        string script = this.socket_scriptfile.get (sock_path);
                        debug ("socket server thread: accepted request on server socket fd %i, path %s, script %s",
                               s.fd, sock_path, script);
                        string key = "%s%i".printf (sock_path, fd);
                        this.script_runners.insert (key, new ScriptRunner (key, script, fd));
                    } catch (GLib.Error e) {
                        error ("socket server thread: cannot launch ScriptRunner: %s", e.message);
                    }
                }
            }
        }

        debug ("socket server thread: end");
        return null;
    }

    private Socket[] listen_sockets = {};
    private HashTable<string,string> socket_scriptfile;
    private HashTable<string,ScriptRunner> script_runners;
    private Thread<void*> thread;
    private bool running;
    private int ctrl_r;
    private int ctrl_w;
}

/**
 * SECTION:umockdeverror
 * @title: umockdev errors
 * @short_description: #GError types for parsing umockdev files
 * hardware devices.
 *
 * See #GError for more information on error domains.
 */

/**
 * UMockdevError:
 * @UMOCKDEV_ERROR_PARSE:
 * There is a malformed or missing line in the device description.
 * @UMOCKDEV_ERROR_VALUE:
 * A value in the device description has an invalid value, for example a device
 * path does not start with "/devices/".
 *
 * Error codes for parsing umockdev files.
 */
public errordomain Error {
   PARSE,
   VALUE,
}

} /* namespace */
