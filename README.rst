umockdev
========
umockdev mocks hardware devices for creating unit tests for libraries and
programs that handle Linux hardware devices. It also provides tools to record
the properties and behaviour of particular devices,  and to run a program or
test suite under a test bed with the previously recorded devices loaded. This
also allows developers of software like gphoto or libmtp to receive these
records in bug reports and recreate the problem on their system without having
access to the affected hardware.

The ``UMockdevTestbed`` class builds a temporary sandbox for mock devices.
You can add a number of devices including arbitrary sysfs attributes and udev
properties, and then run your software in that test bed that is independent of
the actual hardware it is running on.  With this you can simulate particular
hardware in virtual environments up to some degree, without needing any
particular privileges or disturbing the whole system.

You can use this from the command line, and a wide range of programming
languages (C, Vala, and everything which supports gobject-introspection, such
as JavaScript or Python).

Right now umockdev supports the following features:

 * Emulation of arbitrary sysfs devices, attributes, and udev properties

 * Synthesis of arbitrary uevents

 * Emulation of /dev device nodes; they look just like the original real
   device (i. e. stat() delivers a block/char device with appropriate
   major/minor), but are backed by a PTY (for terminal devices) or a plain file
   (for everything else) by default. You can manually create other kinds of
   fake devices in your tests, too.

 * Recording and replay of read()s and write()s to a (character) device (e. g.
   for emulating modems)

 * Recording and replay of usbdevfs (for PtP/MTP devices) and evdev (touch pads,
   Wacom tablets, etc.) ioctls

Other aspects and functionality will be added in the future as use cases arise.

Component overview
==================
umockdev consists of the following parts:

- The ``umockdev-record`` program generates text dumps (conventionally called
  ``*.umockdev``) of some specified, or all of the system's devices and their
  sysfs attributes and udev properties. It can also record ioctls and
  reads/writes that a particular program sends and receives to/from a device,
  and store them into a text file (conventionally called ``*.ioctl`` for ioctl
  records, and ``*.script`` for read/write records).

- The libumockdev library provides the ``UMockdevTestbed`` GObject class which
  builds sysfs and /dev testbeds, provides API to generate devices,
  attributes, properties, and uevents on the fly, and can load ``*.umockdev``
  and ``*.ioctl`` records into them. It provides VAPI and GI bindings, so you
  can use it from C, Vala, and any programming language that supports
  introspection. This is the API that you should use for writing regression
  tests. You can find the API documentation in ``docs/reference`` in the
  source directory.

- The libumockdev-preload library intercepts access to /sys, /dev/, the
  kernel's netlink socket (for uevents) and ioctl() and re-routes them into
  the sandbox built by libumockdev. You don't interface with this library
  directly, instead you need to run your test suite or other program that uses
  libumockdev through the ``umockdev-wrapper`` program.

- The ``umockdev-run`` program builds a sandbox using libumockdev, can load
  ``*.umockdev``, ``*.ioctl``, and ``*.script`` files into it, and run a
  program in that sandbox. I. e. it is a CLI interface to libumockdev, which is
  useful in the "debug a failure with a particular device" use case if you get
  the text dumps from a bug report. This automatically takes care of using the
  preload library, i. e. you don't need ``umockdev-wrapper`` with this. You
  cannot use this program if you need to simulate uevents or change
  attributes/properties on the fly; for those you need to use libumockdev
  directly.

Examples
========
API: Create a fake battery
--------------------------
Batteries, and power supplies in general, are simple devices in the sense that
userspace programs such as upower only communicate with them through sysfs and
uevents. No /dev nor ioctls are necessary. ``docs/examples/`` has two example
programs how to use libumockdev to create a fake battery device, change it to
low charge, sending an uevent, and running upower on a local test system D-BUS
in the testbed, with watching what happens with ``upower --monitor-detail``.
``battery.c`` shows how to do that with plain GObject in C, ``battery.py`` is
the equivalent program in Python that uses the GI binding.

Command line: Record and replay PtP/MTP USB devices
---------------------------------------------------
- Connect your digital camera, mobile phone, or other device which supports
  PtP or MTP, and locate it in lsusb. For example

  Bus 001 Device 012: ID 0fce:0166 Sony Ericsson Xperia Mini Pro

- Dump the sysfs device and udev properties:

  | $ umockdev-record /dev/bus/usb/001/012 > mobile.umockdev
  |

- Now record the dynamic behaviour (i. e. usbfs ioctls) of various operations.
  You can store multiple different operations in the same file, which will
  share the common communication between them. For example:

  | $ umockdev-record --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-detect
  | $ umockdev-record --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-emptyfolders
  |

- Now you can disconnect your device, and run the same operations in a mocked
  testbed. Please note that ``/dev/bus/usb/001/012`` merely echoes what is in
  ``mobile.umockdev`` and it is independent of what is actually in the real
  /dev directory. You can rename that device in the generated ``*.umockdev``
  files and on the command line.

  | $ umockdev-run --device mobile.umockdev --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-detect
  | $ umockdev-run --device mobile.umockdev --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-emptyfolders

Note that if your ``*.ioctl`` files get too large for some purpose, you can
xz-compress them.

Command line: Record and replay tty devices
-------------------------------------------
This example records the behaviour of an USB 3G stick with ModemManager.

- Dump the sysfs device and udev properties of the relevant tty devices (a
  Huawei stick creates ttyUSB{0,1,2}):

  | umockdev-record /dev/ttyUSB* > huawei.umockdev
  |

- Record the communication that goes on between ModemManager and the 3G stick
  into a file ("script"):

  | umockdev-record -s /dev/ttyUSB0=0.script -s /dev/ttyUSB1=1.script \
  |     -s /dev/ttyUSB2=2.script -- modem-manager --debug

  (The --debug option for ModemManager is not necessary, but it's nice to see
  what's going on). Note that you should shut down the running system instance
  for that, or run this on a private D-BUS.

- Now you can disconnect the stick (not necessary, just to clearly prove that
  the following does not actually talk to the stick), and replay in a test bed:

  | umockdev-run -d huawei.umockdev -s /dev/ttyUSB0=0.script -s /dev/ttyUSB1=1.script \
  |      -s /dev/ttyUSB2=2.script -- modem-manager --debug


Development
===========
| Home page: https://github.com/martinpitt/umockdev
| GIT:       git://github.com/martinpitt/umockdev.git
| Bugs:      https://github.com/martinpitt/umockdev/issues
| Releases:  https://launchpad.net/umockdev/+download

Authors
=======
Martin Pitt <martin.pitt@ubuntu.com>

License
=======
Copyright (C) 2012 - 2013 Canonical Ltd.

umockdev is free software; you can redistribute it and/or modify it
under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

umockdev is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program; If not, see <http://www.gnu.org/licenses/>.
