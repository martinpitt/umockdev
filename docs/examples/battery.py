# umockdev example: use libumockdev in Python to fake a battery
#
# Copyright (C) 2013 Canonical Ltd.
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

import subprocess
import os
import sys
import time

from gi.repository import Gio, UMockdev

# determine upowerd path
with open('/usr/share/dbus-1/system-services/org.freedesktop.UPower.service') as f:
    for line in f:
        if line.startswith('Exec='):
            upowerd_path = line.split('=', 1)[1].strip()
            break
    else:
        sys.stderr.write('Cannot determine upowerd path\n')
        sys.exit(1)

# create test bed
testbed = UMockdev.Testbed.new()

# add a battery with good charge
sys_bat = testbed.add_device('power_supply', 'fakeBAT0', None,
                             ['type', 'Battery',
                              'present', '1',
                              'status', 'Discharging',
                              'energy_full', '60000000',
                              'energy_full_design', '80000000',
                              'energy_now', '48000000',
                              'voltage_now', '12000000'],
                             ['POWER_SUPPLY_ONLINE', '1'])

# start a fake system D-BUS
dbus = Gio.TestDBus.new(Gio.TestDBusFlags.NONE)
dbus.up()
os.environ['DBUS_SYSTEM_BUS_ADDRESS'] = dbus.get_bus_address()

print('-- starting upower on test dbus under umockdev-wrapper')
upowerd = subprocess.Popen([ upowerd_path, '-v'], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)

# give it some time to settle
time.sleep(0.5)

print('-- Initial upower --dump')
subprocess.call(['upower', '--dump'])
print('-- Starting upower monitoring now')
monitor = subprocess.Popen(['upower', '--monitor-detail'])

time.sleep(1)
print('-- setting battery charge to 2.5% now')
testbed.set_attribute(sys_bat, 'energy_now', '1500000')
# send uevent to notify upowerd
testbed.uevent(sys_bat, 'change')

time.sleep(1)

# clean up
print('-- cleaning up')
monitor.terminate()
monitor.wait()
upowerd.terminate()
upowerd.wait()
dbus.down()
