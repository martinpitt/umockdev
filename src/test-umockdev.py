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

import sys
import os.path
import unittest

try:
    from gi.repository import GLib, GUdev
except ImportError as e:
    print('GI module not available, skipping test: %s' % e)
    sys.exit(0)

from gi.repository import UMockdev

class Testbed(unittest.TestCase):
    def setUp(self):
        self.testbed = UMockdev.Testbed()

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
            'usb', 'extkeyboard1',
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

        self.assertEqual(dev.get_property_keys(), 
                         ['DEVPATH', 'ID_INPUT', 'ID_INPUT_KEYBOARD',
                          'SUBSYSTEM', 'UDEV_LOG'])
        self.assertEqual(dev.get_property('DEVPATH'), '/devices/extkeyboard1')
        self.assertEqual(dev.get_property('SUBSYSTEM'), 'usb')
        self.assertEqual(dev.get_property('ID_INPUT'), '1')
        self.assertEqual(dev.get_property('ID_INPUT_KEYBOARD'), '1')
        self.assertEqual(dev.get_property('NO_SUCH_PROP'), None)

    def test_set_attribute(self):
        '''testbed set_attribute()'''

        syspath = self.testbed.add_device(
            'usb', 'extkeyboard1', ['idVendor', '0815', 'idProduct', 'AFFE'], [])

        # change an existing attribute
        self.testbed.set_attribute(syspath, 'idProduct', 'BEEF')

        # add a new one
        self.testbed.set_attribute(syspath, 'color', 'yellow')

        client = GUdev.Client.new(None)
        dev = client.query_by_sysfs_path(syspath)
        self.assertNotEqual(dev, None)
        self.assertEqual(dev.get_sysfs_attr('idVendor'), '0815')
        self.assertEqual(dev.get_sysfs_attr('idProduct'), 'BEEF')
        self.assertEqual(dev.get_sysfs_attr('color'), 'yellow')

    def test_set_property(self):
        '''testbed set_property()'''

        syspath = self.testbed.add_device(
            'usb', 'extkeyboard1', [], ["ID_INPUT", "1"])

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

        counter = [0, 0, 0, None]  # add, remove, change, last device

        def on_uevent(client, action, device, counters):
            if action == 'add':
                counters[0] += 1
            elif action == 'remove':
                counters[1] += 1
            else:
                assert action == 'change'
                counters[2] += 1

            counters[3] = device.get_sysfs_path()

        syspath = self.testbed.add_device('pci', 'mydev', ['idVendor', '0815'], ['ID_INPUT', '1'])
        self.assertNotEqual(syspath, None)

        # set up listener for uevent signal
        client = GUdev.Client.new(['pci'])
        client.connect('uevent', on_uevent, counter)

        mainloop = GLib.MainLoop()

        # send a signal and run main loop for 0.5 seconds to catch it
        self.testbed.uevent(syspath, 'add')
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()
        self.assertEqual(counter, [1, 0, 0, syspath])

        counter[0] = 0
        counter[3] = None
        self.testbed.uevent(syspath, 'change')
        GLib.timeout_add(500, mainloop.quit)
        mainloop.run()
        self.assertEqual(counter, [0, 0, 1, syspath])

unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
