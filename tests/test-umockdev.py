#!/usr/bin/python3

# test-umockdev
#
# Copyright (C) 2012 Canonical Ltd.
# Author: Martin Pitt <martin.pitt@ubuntu.com>
#
# umockdev is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# umockdev is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program; If not, see <http://www.gnu.org/licenses/>.

import fcntl
import os.path
import struct
import sys
import unittest
import warnings

try:
    import gi
    gi.require_version('GUdev', '1.0')
    gi.require_version('UMockdev', '1.0')
    # 'pkgutil.get_loader' is deprecated and slated for removal in Python 3.14
    with warnings.catch_warnings():
        warnings.filterwarnings('ignore', category=DeprecationWarning)
        from gi.repository import GLib, GUdev
except ImportError as e:
    print('GI module not available, skipping test: %s' % e)
    sys.exit(0)

with warnings.catch_warnings():
    warnings.filterwarnings('ignore', category=DeprecationWarning)
    from gi.repository import UMockdev


class Testbed(unittest.TestCase):
    def setUp(self):
        self.testbed = UMockdev.Testbed.new()

    def test_root_dir(self):
        self.assertTrue(os.path.isdir(self.testbed.get_root_dir()))

    def test_empty(self):
        '''empty testbed without any devices'''

        client = GUdev.Client.new(None)
        enum = GUdev.Enumerator.new(client)
        self.assertEqual(enum.execute(), [])

    def test_add_device(self):
        '''testbed with adding one device'''

        syspath = self.testbed.add_device(
            'usb', 'extkeyboard1', None,
            ['idVendor', '0815', 'idProduct', 'AFFE'],
            ['ID_INPUT', '1',  'ID_INPUT_KEYBOARD', '1'])
        self.assertEqual(syspath, '/sys/devices/extkeyboard1')

        client = GUdev.Client.new(None)
        enum = GUdev.Enumerator.new(client)
        devices = enum.execute()
        self.assertEqual(len(devices), 1)
        dev = devices[0]

        # check that the device matches what we put into our testbed
        self.assertEqual(dev.get_name(), 'extkeyboard1')
        self.assertEqual(dev.get_sysfs_path(), syspath)
        self.assertEqual(dev.get_sysfs_attr('idVendor'), '0815')
        self.assertEqual(dev.get_sysfs_attr('idProduct'), 'AFFE')
        self.assertEqual(dev.get_sysfs_attr('noSuchAttr'), None)

        keys = dev.get_property_keys()
        # older udev versions have this key, newer don't
        try:
            keys.remove('UDEV_LOG')
        except ValueError:
            pass
        self.assertEqual(keys, ['DEVPATH', 'ID_INPUT', 'ID_INPUT_KEYBOARD', 'SUBSYSTEM'])
        self.assertEqual(dev.get_property('DEVPATH'), '/devices/extkeyboard1')
        self.assertEqual(dev.get_property('SUBSYSTEM'), 'usb')
        self.assertEqual(dev.get_property('ID_INPUT'), '1')
        self.assertEqual(dev.get_property('ID_INPUT_KEYBOARD'), '1')
        self.assertEqual(dev.get_property('NO_SUCH_PROP'), None)

    def test_set_attribute(self):
        '''testbed set_attribute()'''

        syspath = self.testbed.add_device(
            'usb', 'extkeyboard1', None, ['idVendor', '0815', 'idProduct', 'AFFE'], [])

        # change an existing attribute
        self.testbed.set_attribute(syspath, 'idProduct', 'BEEF')

        # add a new one
        self.testbed.set_attribute(syspath, 'color', 'yellow')

        # add a binary attribute
        self.testbed.set_attribute_binary(syspath, 'descriptor', b'\x01\x00\xFF\x00\x05')

        client = GUdev.Client.new(None)
        dev = client.query_by_sysfs_path(syspath)
        self.assertNotEqual(dev, None)
        self.assertEqual(dev.get_sysfs_attr('idVendor'), '0815')
        self.assertEqual(dev.get_sysfs_attr('idProduct'), 'BEEF')
        self.assertEqual(dev.get_sysfs_attr('color'), 'yellow')

        # validate binary attribute
        with open(os.path.join(self.testbed.get_root_dir() + syspath, 'descriptor'), 'rb') as f:
            self.assertEqual(f.read(), b'\x01\x00\xFF\x00\x05')

    def test_set_property(self):
        '''testbed set_property()'''

        syspath = self.testbed.add_device(
            'usb', 'extkeyboard1', None, [], ["ID_INPUT", "1"])

        # change an existing property
        self.testbed.set_property(syspath, 'ID_INPUT', '0')

        # add a new one
        self.testbed.set_property(syspath, 'ID_COLOR', 'green')

        client = GUdev.Client.new(None)
        dev = client.query_by_sysfs_path(syspath)
        self.assertNotEqual(dev, None)
        self.assertEqual(dev.get_property('ID_INPUT'), '0')
        self.assertEqual(dev.get_property('ID_COLOR'), 'green')

    def test_uevent(self):
        '''testbed uevent()'''

        counter = [0, 0, 0, None, None, None]  # add, remove, change, last device, idVendor, ID_INPUT

        def on_uevent(client, action, device, counters):
            if action == 'add':
                counters[0] += 1
            elif action == 'remove':
                counters[1] += 1
            else:
                assert action == 'change'
                self.assertEqual(device.get_sysfs_attr('idVendor'), '0815')
                self.assertEqual(device.get_property('ID_INPUT'), '1')
                counters[2] += 1

            counters[3] = device.get_sysfs_path()
            counters[4] = device.get_sysfs_attr('idVendor')
            counters[5] = device.get_property('ID_INPUT')

        # set up listener for uevent signal
        client = GUdev.Client.new(['pci'])
        client.connect('uevent', on_uevent, counter)
        mainloop = GLib.MainLoop()

        syspath = self.testbed.add_device('pci', 'mydev', None, ['idVendor', '0815'], ['ID_INPUT', '1'])
        self.assertNotEqual(syspath, None)

        # Run the main loop for 0.5 seconds to catch the "add" uevent, which
        # should be automatically generated by the call to `add_device()`.
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()
        self.assertEqual(counter, [1, 0, 0, syspath, '0815', '1'])

        counter[0] = 0
        counter[3] = None
        counter[4] = None
        counter[5] = None

        self.testbed.uevent(syspath, 'change')
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()
        self.assertEqual(counter, [0, 0, 1, syspath, '0815', '1'])

        counter[2] = 0
        counter[3] = None
        counter[4] = None
        counter[5] = None

        self.testbed.remove_device(syspath)

        # Run the main loop for 0.5 seconds to catch the "remove" uevent, which
        # should be automatically generated by the call to `remove_device()`.
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()
        self.assertEqual(counter, [0, 1, 0, syspath, None, '1'])

    def test_remove_usb_parent_device(self):
        '''testbed removing a USB parent device removes children'''

        counter = {}

        def on_uevent(client, action, device, counters):
            key = device.get_sysfs_path()
            counters.setdefault(key, [0, 0])
            if action == 'add':
                counters[key][0] += 1
            elif action == 'remove':
                counters[key][1] += 1

        # set up listener for uevent signal
        client = GUdev.Client.new(['usb'])
        client.connect('uevent', on_uevent, counter)
        mainloop = GLib.MainLoop()

        parent_syspath = self.testbed.add_device('usb', 'myparentdev', None, ['idVendor', '0815'], ['ID_INPUT', '1'])
        self.assertNotEqual(parent_syspath, None)

        child1_syspath = self.testbed.add_device('usb', 'child1', parent_syspath, ['idVendor', '0815'], ['ID_INPUT', '1'])
        self.assertNotEqual(child1_syspath, None)

        child2_syspath = self.testbed.add_device('usb', 'child2', parent_syspath, ['idVendor', '0815'], ['ID_INPUT', '1'])
        self.assertNotEqual(child2_syspath, None)

        # Run the main loop for 0.5 seconds to catch the "add" uevent, which
        # should be automatically generated by the call to `add_device()`.
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()
        self.assertIn(parent_syspath, counter)
        self.assertIn(child1_syspath, counter)
        self.assertIn(child2_syspath, counter)
        self.assertEqual(counter[parent_syspath], [1, 0])
        self.assertEqual(counter[child1_syspath], [1, 0])
        self.assertEqual(counter[child2_syspath], [1, 0])

        # Just remove the parent, the children should also be removed.
        self.testbed.remove_device(parent_syspath)

        # Run the main loop for 0.5 seconds to catch the "remove" uevent, which
        # should be automatically generated by the call to `remove_device()`.
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()
        self.assertEqual(counter[parent_syspath], [1, 1])
        self.assertEqual(counter[child1_syspath], [1, 1])
        self.assertEqual(counter[child2_syspath], [1, 1])

        # Verify that the parent syspath has been removed (and therefore the
        # children syspaths have also been removed.)
        self.assertFalse(os.path.exists(parent_syspath))

    def test_add_from_string(self):
        self.assertTrue(self.testbed.add_from_string('''P: /devices/dev1
E: SIMPLE_PROP=1
E: SUBSYSTEM=pci
H: binary_attr=41FF0005FF00
A: multiline_attr=a\\\\b\\nc\\\\d\\nlast
A: simple_attr=1
'''))

        client = GUdev.Client.new(None)
        enum = GUdev.Enumerator.new(client)
        devices = enum.execute()
        self.assertEqual([d.get_sysfs_path() for d in devices], ['/sys/devices/dev1'])

        device = client.query_by_sysfs_path('/sys/devices/dev1')
        self.assertEqual(device.get_subsystem(), 'pci')
        # self.assertEqual(device.get_parent(), None)
        self.assertEqual(device.get_sysfs_attr('simple_attr'), '1')
        self.assertEqual(device.get_sysfs_attr('multiline_attr'),
                         'a\\b\nc\\d\nlast')
        self.assertEqual(device.get_property('SIMPLE_PROP'), '1')
        with open(os.path.join(self.testbed.get_root_dir(),
                               '/sys/devices/dev1/binary_attr'), 'rb') as f:
            self.assertEqual(f.read(), b'\x41\xFF\x00\x05\xFF\x00')

    def test_add_from_string_errors(self):
        try:
            # python 3.2+
            assertRaisesRegex = self.assertRaisesRegex
        except AttributeError:
            # python 2 && python < 3.2
            assertRaisesRegex = self.assertRaisesRegexp

        # does not start with P:
        with assertRaisesRegex(GLib.GError, 'must start with.*P:'):
            self.testbed.add_from_string('E: SIMPLE_PROP=1\n')

        # no value
        with assertRaisesRegex(GLib.GError, 'malformed attribute'):
            self.testbed.add_from_string('P: /devices/dev1\nE: SIMPLE_PROP\n')

    def test_custom_ioctl(self):
        handler = UMockdev.IoctlBase()

        def handle_ioctl(handler, client):
            if client.get_request() != 1:
                return False

            in_data = struct.pack('l', -1)
            out_data = struct.pack('l', 1)
            arg = client.get_arg()
            data = arg.resolve(0, len(out_data))
            if data.retrieve() != in_data:
                return False
            data.update(0, out_data)

            client.complete(99, 0)
            return True

        handler.connect("handle-ioctl", handle_ioctl)

        self.testbed.add_from_string('P: /devices/test\nN: test\nE: SUBSYSTEM=test')
        self.testbed.attach_ioctl('/dev/test', handler)

        fd = os.open('/dev/test', os.O_RDONLY)
        arg = bytearray(struct.pack('l', -1))
        self.assertEqual(fcntl.ioctl(fd, 1, arg, True), 99)
        arg = struct.unpack('l', arg)[0]
        self.assertEqual(arg, 1)

        # Check that an detach/attach works
        self.testbed.detach_ioctl('/dev/test')
        self.testbed.attach_ioctl('/dev/test', handler)


unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
