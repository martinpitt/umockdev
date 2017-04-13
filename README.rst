umockdev
========
umockdev mocks Linux devices for creating integration tests for hardware
related libraries and programs. It also provides tools to record the properties
and behaviour of particular devices, and to run a program or test suite under
a test bed with the previously recorded devices loaded. This allows
developers of software like gphoto or libmtp to receive these records in bug
reports and recreate the problem on their system without having access to the
affected hardware.

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

- Emulation of arbitrary sysfs devices, attributes, and udev properties.

- Synthesis of arbitrary uevents.

- Emulation of /dev device nodes; they look just like the original real
  device (i. e. stat() delivers a block/char device with appropriate
  major/minor), but are backed by a PTY (for terminal devices) or a plain file
  (for everything else) by default. You can manually create other kinds of
  fake devices in your tests, too.

- Recording and replay of read()s/recv()s and write()s/send()s from/to a
  character device (e. g. for emulating modems) or an Unix socket (e. g. for
  Android's /dev/socket/rild). These records are called ``scripts``. Replay can
  optionally use a configurable fuzz factor in case the expected (previously
  recorded) script data doesn't perfectly match what is actually being sent
  from the tested application.

- Recording and replay of usbdevfs (for PtP/MTP devices) and evdev (touch pads,
  Wacom tablets, etc.) ioctls.

- Recording and replay of evdev input events using the evemu events format
  (https://github.com/bentiss/evemu/blob/master/README.md). Unlike recorded
  evdev scripts these are architecture independent and human readable.

- Mocking of files and directories in /proc

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

- The libumockdev-preload library intercepts access to /sys, /dev/, /proc/, the
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

Mocking /proc and /dev
======================
When enabled, the preload library diverts access to ``/proc`` and ``/dev`` to
the corresponding directories in ``$UMOCKDEV_DIR``, aka.
``umockdev_testbed_get_root()``. However, if a path does not exist there, it
falls through the real ``/proc`` and ``/dev``. Thus you can easily replace
files like ``/proc/cpuinfo`` or add new ones without losing standard files such
as ``/dev/null`` or ``/proc/pid/*``. Currently there is no way to
"remove" files from the real directories or fully control them. You can get the
effect of removing a file by creating a broken symlink in the umockdev
directory though.

In contrast, an ``UMockdevTestbed`` fully controls the visible ``/sys``
directory; for a program there is no (regular) way to see the real ``/sys``,
unless it circumvents the libc API.

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

  ::

    Bus 001 Device 012: ID 0fce:0166 Sony Ericsson Xperia Mini Pro

- Dump the sysfs device and udev properties:

  ::

    $ umockdev-record /dev/bus/usb/001/012 > mobile.umockdev

- Now record the dynamic behaviour (i. e. usbfs ioctls) of various operations.
  You can store multiple different operations in the same file, which will
  share the common communication between them. For example:

  ::

    $ umockdev-record --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-detect
    $ umockdev-record --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-emptyfolders

- Now you can disconnect your device, and run the same operations in a mocked
  testbed. Please note that ``/dev/bus/usb/001/012`` merely echoes what is in
  ``mobile.umockdev`` and it is independent of what is actually in the real
  /dev directory. You can rename that device in the generated ``*.umockdev``
  files and on the command line.

  ::

    $ umockdev-run --device mobile.umockdev --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-detect
    $ umockdev-run --device mobile.umockdev --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-emptyfolders

Note that if your ``*.ioctl`` files get too large for some purpose, you can
xz-compress them.

Command line: Record and replay tty devices
-------------------------------------------
This example records the behaviour of an USB 3G stick with ModemManager.

- Dump the sysfs device and udev properties of the relevant tty devices (a
  Huawei stick creates ttyUSB{0,1,2}):

  ::

    umockdev-record /dev/ttyUSB* > huawei.umockdev


- Record the communication that goes on between ModemManager and the 3G stick
  into a file ("script"):

  ::

    umockdev-record -s /dev/ttyUSB0=0.script -s /dev/ttyUSB1=1.script \
        -s /dev/ttyUSB2=2.script -- modem-manager --debug

  (The --debug option for ModemManager is not necessary, but it's nice to see
  what's going on). Note that you should shut down the running system instance
  for that, or run this on a private D-BUS.

- Now you can disconnect the stick (not necessary, just to clearly prove that
  the following does not actually talk to the stick), and replay in a test bed:

  ::

    umockdev-run -d huawei.umockdev -s /dev/ttyUSB0=0.script -s /dev/ttyUSB1=1.script \
         -s /dev/ttyUSB2=2.script -- modem-manager --debug


Record and replay an Unix socket
--------------------------------
This example records the behaviour of ofonod when talking to Android's rild
through ``/dev/socket/rild``.

- Record the communication:

  ::

    sudo pkill ofonod
    sudo umockdev-record -s /dev/socket/rild=phonecall.script -- ofonod -n -d

  Now make a call, send a SMS, or anything else you want to replay later.
  Press Control-C when you are done.

- ofonod's messages that get sent to rild are not 100% predictable, some bytes
  in some messages are always different. Edit the recorded rild.script to set
  a fuzz factor of 5, i. e. at most 5% of the bytes in a message are allowed
  to be different from the recorded ones. Insert a line

  ::

     f 5 -

  at the top of the file. See docs/script-format.txt for details.

- Now you can run ofonod in a testbed with the mocked rild:

  ::

    sudo pkill ofonod
    sudo umockdev-run -u /dev/socket/rild=phonecall.script -- ofonod -n -d

  Note that you don't need to record device properties or specify -d/--device
  for unix sockets, since their path is all that is to be known about them.

  With the API, you would do this with a call like

  ::

    umockdev_testbed_load_socket_script(testbed, "/dev/socket/rild",
                                        SOCK_STREAM, "phonecall.script", &error);

  Note that for Unix sockets you cannot ``use umockdev_testbed_get_dev_fd()``,
  you can only use scripts with them. If you need full control in your test suite,
  you can of course create the socket in <testbed root>/<socket path> and
  handle the bind/accept/communication yourself.

Record and replay input devices
-------------------------------
For those the "evemu" format is preferrable as it is platform independent
(scripts depend on the architecture endianess and size of time_t) and human
readable. ioctls need to be recorded as well, as they specify the input
device's capability beyond what it is already exposed in sysfs, particularly
for multi-touch devices.

This uses the "evtest" program, but you can use anything which listens to evdev
devices.

- Record the static device data, ioctls, and some events. This needs to run as
  root:

  ::

    umockdev-record /dev/input/event3 > mouse.umockdev
    umockdev-record -i /dev/input/event3=mouse.ioctl -e /dev/input/event3=mouse.events \
        -- evtest /dev/input/event3

  Now cause some events on the devices (key presses, mouse clicks, touch
  clicks, etc.), and stop evtest with Control-C.

- Replay is straightforward. It does not need root privileges:

  ::

    umockdev-run -d mouse.umockdev -i /dev/input/event3=mouse.ioctl \
        -e /dev/input/event3=mouse.events - evtest /dev/input/event3

  Press Control-C again to stop evtest.

Command line: Mock file in /proc
================================
By default, ``/proc`` is the standard system directory:

::

  $ umockdev-run -- head -n2 /proc/cpuinfo
  processor	: 0
  vendor_id	: GenuineIntel

But you can replace files (or directories) in it by the ones in the mock dir:

::

  $ umockdev-run -- sh -c 'mkdir $UMOCKDEV_DIR/proc;
  >   echo hello > $UMOCKDEV_DIR/proc/cpuinfo;
  >   cat /proc/cpuinfo'
  hello


Build, Test, Run
================
If you want to build umockdev from a git checkout, run ./autogen.sh to build
the autotools files; you need autoreconf, autoconf, automake, libtool, and
gtk-doc-tools for this.

After that, or if you build from a release tarball, umockdev uses a standard
autotools build system:

- Run ``./configure`` first; you may want to supply ``--prefix``,
  ``--sysconfdir``, and other options, see ``./configure --help``.
- Run ``make`` to build the project.
- Run ``make check`` to run the tests against the build tree.
- Run ``make check-code-coverage`` to run the tests against the build tree and
  measure the code coverage (requires configuring with --enable-code-coverage).
  Report will be written to ``umockdev-*-coverage/index.html``.
- Run ``make install`` as root to install into the configured prefix
  (``/usr/local`` by default).
- Run ``make check-installed`` to run the test suite against the installed
  version of umockdev.

If you don't want to install umockdev but use it from the build tree, set
these environment variables, assuming that your current directory is the
top-level directory of the umockdev tree:

::

  LD_LIBRARY_PATH=`pwd`/.libs:$LD_LIBRARY_PATH
  GI_TYPELIB_PATH=`pwd`:$GI_TYPELIB_PATH
  PATH=`pwd`/src:$PATH

Debugging
=========
To debug umockdev itself and what it's doing, you can set the
``$UMOCKDEV_DEBUG`` environment variable to a list (comma or space separated)
of

path
   Redirection of paths in ``/sys``, ``/dev`` etc. to testbed

netlink
   Redirection of netlink socket and uevent synthesis

script
   Script (device reads/writes) recording and replay

ioctl
   ioctl recording and replay

ioctl-tree
   detailed parsing and traversal of recorded ioctl trees

all
   All debug categories

Development
===========
| Home page: https://github.com/martinpitt/umockdev
| GIT:       git://github.com/martinpitt/umockdev.git
| Bugs:      https://github.com/martinpitt/umockdev/issues
| Releases:  https://launchpad.net/umockdev/+download

umockdev is very much demand driven. If you want to work on a new feature (such
as adding support for more ioctls) or contribute a bug fix, please check out
the git repository, push your changes to github, and create a pull request.
Contributions are appreciated, and I will do my best to provide timely reviews.

If you find a bug in umockdev or have an idea about a new feature but don't
want to implement it yourself, please file a report in the github issue
tracker. Please always include the version of umockdev that you are using, and
a complete runnable reproducer of the problem (i. e. the code and recorded
scripts/ioctls, etc.), unless it is a feature request.

Authors
=======
Martin Pitt <martin.pitt@ubuntu.com>

You can contact me on IRC: pitti on Freenode, I'm hanging out in
#ubuntu-quality and other channels. You can also file an issue on github and
I'll answer your question there.

License
=======
Copyright (C) 2012 - 2014 Canonical Ltd.
Copyright (C) 2017 Martin Pitt

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
