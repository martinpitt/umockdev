![CI status](https://github.com/martinpitt/umockdev/actions/workflows/tests.yml/badge.svg)

umockdev
========
umockdev mocks Linux devices for creating integration tests for hardware
related libraries and programs. It also provides tools to record the properties
and behaviour of particular devices, and to run a program or test suite under
a test bed with the previously recorded devices loaded. This allows
developers of software like gphoto or libmtp to receive these records in bug
reports and recreate the problem on their system without having access to the
affected hardware.

The `UMockdevTestbed` class builds a temporary sandbox for mock devices.
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
  Android's /dev/socket/rild). These records are called "scripts". Replay can
  optionally use a configurable fuzz factor in case the expected (previously
  recorded) script data doesn't perfectly match what is actually being sent
  from the tested application.

- Replay of usbdevfs (e. g. for PtP/MTP devices). Two methods are available for
  flexible and pure in-order replay. The `--ioctl` based replay may allow
  interactive emulation while the `pcap`/`usbmon` based replay is purely
  in-order and supports control transfer replay.

- Recording and replay of evdev (touch pads, Wacom tablets, etc.) ioctls.

- Recording and replay of spidev ioctls and read/write commands using `--ioctl`
  for both `umockdev-record` and `umockdev-run`. This is an in-order
  record/replay of all SPI transfers. Similar to scripts
  with the difference that full duplex transfers via ioctl are supported.
  Timinges/errors are currently not recorded.

- Recording and replay of evdev input events using the
  [evemu events format](https://github.com/bentiss/evemu/blob/master/README.md).
  Unlike recorded evdev scripts these are architecture independent and human
  readable.

- Mocking of files and directories in /proc

Other aspects and functionality will be added in the future as use cases arise.

Component overview
==================
umockdev consists of the following parts:

- The `umockdev-record` program generates text dumps (conventionally called
  `*.umockdev`) of some specified, or all of the system's devices and their
  sysfs attributes and udev properties. It can also record ioctls and
  reads/writes that a particular program sends and receives to/from a device,
  and store them into a text file (conventionally called `*.ioctl` for ioctl
  records, and `*.script` for read/write records).

- The libumockdev library provides the `UMockdevTestbed` GObject class which
  builds sysfs and /dev testbeds, provides API to generate devices,
  attributes, properties, and uevents on the fly, and can load `*.umockdev`
  and `*.ioctl` records into them. It provides VAPI and GI bindings, so you
  can use it from C, Vala, and any programming language that supports
  introspection. This is the API that you should use for writing regression
  tests. You can find the API documentation in [docs/reference/](./docs/reference/).

- The libumockdev-preload library intercepts access to /sys, /dev/, /proc/, the
  kernel's netlink socket (for uevents) and ioctl() and re-routes them into
  the sandbox built by libumockdev. You don't interface with this library
  directly, instead you need to run your test suite or other program that uses
  libumockdev through the `umockdev-wrapper` program.

- The `umockdev-run` program builds a sandbox using libumockdev, can load
  `*.umockdev`, `*.ioctl`, and `*.script` files into it, and run a
  program in that sandbox. I. e. it is a CLI interface to libumockdev, which is
  useful in the "debug a failure with a particular device" use case if you get
  the text dumps from a bug report. This automatically takes care of using the
  preload library, i. e. you don't need `umockdev-wrapper` with this. You
  cannot use this program if you need to simulate uevents or change
  attributes/properties on the fly; for those you need to use libumockdev
  directly.

Mocking /proc and /dev
======================
When enabled, the preload library diverts access to `/proc` and `/dev` to
the corresponding directories in `$UMOCKDEV_DIR`, aka.
`umockdev_testbed_get_root()`. However, if a path does not exist there, it
falls through the real `/proc` and `/dev`. Thus you can easily replace
files like `/proc/cpuinfo` or add new ones without losing standard files such
as `/dev/null` or `/proc/pid/*`. Currently there is no way to
"remove" files from the real directories or fully control them. You can get the
effect of removing a file by creating a broken symlink in the umockdev
directory though.

In contrast, an `UMockdevTestbed` fully controls the visible `/sys`
directory; for a program there is no (regular) way to see the real `/sys`,
unless it circumvents the libc API.

Examples
========
API: Create a fake battery
--------------------------
Batteries, and power supplies in general, are simple devices in the sense that
userspace programs such as upower only communicate with them through sysfs and
uevents. No /dev nor ioctls are necessary. [docs/examples/](./docs/examples/) has two example
programs how to use libumockdev to create a fake battery device, change it to
low charge, sending an uevent, and running upower on a local test system D-BUS
in the testbed, with watching what happens with `upower --monitor-detail`.
`battery.c` shows how to do that with plain GObject in C, `battery.py` is
the equivalent program in Python that uses the GI binding.

Command line: Record and replay PtP/MTP USB devices (unordered)
---------------------------------------------------------------

With this method of record and replay a tree of dependent USB URBs is generated
and replayed. The advantage is that discontinuities may occur during replay,
as the replayer will always try to find the appropriate response, possibly
changing the order of replay.

If you need completely in-order replay or USB control commands, then the pcap
based replayer will be more appropriate.

- Connect your digital camera, mobile phone, or other device which supports
  PtP or MTP, and locate it in lsusb. For example

      Bus 001 Device 012: ID 0fce:0166 Sony Ericsson Xperia Mini Pro

- Dump the sysfs device and udev properties:

      umockdev-record /dev/bus/usb/001/012 > mobile.umockdev

- Now record the dynamic behaviour (i. e. usbfs ioctls) of various operations.
  You can store multiple different operations in the same file, which will
  share the common communication between them. For example:

      umockdev-record --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-detect
      umockdev-record --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-emptyfolders

- Now you can disconnect your device, and run the same operations in a mocked
  testbed. Please note that `/dev/bus/usb/001/012` merely echoes what is in
  `mobile.umockdev` and it is independent of what is actually in the real
  /dev directory. You can rename that device in the generated `*.umockdev`
  files and on the command line.

      umockdev-run --device mobile.umockdev --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-detect
      umockdev-run --device mobile.umockdev --ioctl /dev/bus/usb/001/012=mobile.ioctl mtp-emptyfolders

Note that if your `*.ioctl` files get too large for some purpose, you can
xz-compress them.

Command line: Record and replay USB devices using `usbmon` pcap captures
------------------------------------------------------------------------

This method of USB replay is a pure in-order replay. This has the advantage that
timeouts will be correctly emulated rather than causing discontinuities in the
replayer and possibly incorrect device state emulation. `pcap` currently also
has the advantage of correctly replaying USB control transfers.

- Connect your device and locate it in lsusb. For example

      Bus 001 Device 004: ID 06cb:00bd Synaptics, Inc. Prometheus MIS Touch Fingerprint Reader

- Dump the sysfs device and udev properties:

      umockdev-record /dev/bus/usb/001/004 > fingerprint.umockdev

- Use `wireshark` to record the bus in question (bus 001, `usbmon1`). By
  default this will record all devices. To minimize the size of the capture, you
  may use a filter to only record/save communication to the device in question.

  After starting the capture, run the command to capture the required
  interactions. For example the `synaptics/custom.py` test script from libfprint.

- Now you can disconnect your device, and run the same operations in a mocked
  testbed. To do so, load the sysfs device and udev properties. Then specify
  the `--pcap` option with the corresponding sysfs path of the device. Doing
  so will create the appropriate `usbdevfs` device node.

  Note that you need to specify the sysfs path from the device description.

      umockdev-run --device fingerprint.umockdev --pcap /sys/devices/pci0000:00/0000:00:14.0/usb1/1-9=fingerprint.pcapng synaptics/custom.py


Command line: Record and replay tty devices
-------------------------------------------
This example records the behaviour of an USB 3G stick with ModemManager.

- Dump the sysfs device and udev properties of the relevant tty devices (a
  Huawei stick creates ttyUSB{0,1,2}):

      umockdev-record /dev/ttyUSB* > huawei.umockdev


- Record the communication that goes on between ModemManager and the 3G stick
  into a file ("script"):

      umockdev-record -s /dev/ttyUSB0=0.script -s /dev/ttyUSB1=1.script \
          -s /dev/ttyUSB2=2.script -- modem-manager --debug

  (The `--debug` option for ModemManager is not necessary, but it's nice to see
  what's going on). Note that you should shut down the running system instance
  for that, or run this on a private D-BUS.

- Now you can disconnect the stick (not necessary, just to clearly prove that
  the following does not actually talk to the stick), and replay in a test bed:

      umockdev-run -d huawei.umockdev -s /dev/ttyUSB0=0.script -s /dev/ttyUSB1=1.script \
           -s /dev/ttyUSB2=2.script -- modem-manager --debug


Record and replay an Unix socket
--------------------------------
This example records the behaviour of ofonod when talking to Android's rild
through `/dev/socket/rild`.

- Record the communication:

      sudo pkill ofonod
      sudo umockdev-record -s /dev/socket/rild=phonecall.script -- ofonod -n -d

  Now make a call, send a SMS, or anything else you want to replay later.
  Press Control-C when you are done.

- ofonod's messages that get sent to rild are not 100% predictable, some bytes
  in some messages are always different. Edit the recorded rild.script to set
  a fuzz factor of 5, i. e. at most 5% of the bytes in a message are allowed
  to be different from the recorded ones. Insert a line

      f 5 -

  at the top of the file. See [docs/script-format.txt](./docs/script-format.txt) for details.

- Now you can run ofonod in a testbed with the mocked rild:

      sudo pkill ofonod
      sudo umockdev-run -u /dev/socket/rild=phonecall.script -- ofonod -n -d

  Note that you don't need to record device properties or specify -d/--device
  for unix sockets, since their path is all that is to be known about them.

  With the API, you would do this with a call like

      umockdev_testbed_load_socket_script(testbed, "/dev/socket/rild",
                                          SOCK_STREAM, "phonecall.script", &error);

  Note that for Unix sockets you cannot `use umockdev_testbed_get_dev_fd()`,
  you can only use scripts with them. If you need full control in your test suite,
  you can of course create the socket in `<testbed root>/<socket path>` and
  handle the bind/accept/communication yourself.

Record and replay input devices
-------------------------------
For those the "evemu" format is preferable as it is platform independent
(scripts depend on the architecture endianess and size of time_t) and human
readable. ioctls need to be recorded as well, as they specify the input
device's capability beyond what it is already exposed in sysfs, particularly
for multi-touch devices.

This uses the `evtest` program, but you can use anything which listens to evdev
devices.

- Record the static device data, ioctls, and some events. This needs to run as
  root:

      sudo umockdev-record /dev/input/event3 > mouse.umockdev
      sudo umockdev-record -i /dev/input/event3=mouse.ioctl \
        -e /dev/input/event3=mouse.events -- evtest /dev/input/event3

  Now cause some events on the devices (key presses, mouse clicks, touch
  clicks, etc.), and stop evtest with Control-C.

- Replay is straightforward. It does not need root privileges:

      umockdev-run -d mouse.umockdev -i /dev/input/event3=mouse.ioctl \
          -e /dev/input/event3=mouse.events -- evtest /dev/input/event3

  Press Control-C again to stop evtest.

Command line: Mock file in /proc
================================
By default, `/proc` is the standard system directory:

    $ umockdev-run -- head -n2 /proc/cpuinfo
    processor	: 0
    vendor_id	: GenuineIntel

But you can replace files (or directories) in it by the ones in the mock dir:

    $ umockdev-run -- sh -c 'mkdir $UMOCKDEV_DIR/proc;
    >   echo hello > $UMOCKDEV_DIR/proc/cpuinfo;
    >   cat /proc/cpuinfo'
    hello


Build, Test, Run
================

If you want to build umockdev from a git checkout, install the necessary build
dependencies first. On a Debian based system:

    sudo apt install -y meson pkg-config valac libglib2.0-dev libudev-dev libgudev-1.0-dev libpcap-dev python3-gi gobject-introspection libgirepository1.0-dev gir1.2-glib-2.0 gir1.2-gudev-1.0 gtk-doc-tools

In order to run all integration tests, install the test dependencies:

    sudo apt install -y udev xserver-xorg-video-dummy xserver-xorg-input-evdev xserver-xorg-input-synaptics xinput usbutils gphoto2

umockdev uses the [meson build system](https://mesonbuild.com/). Configure a
build tree with desired options with

    meson setup build/
    cd build/

You may want to supply `--prefix=/usr` or similar options, see `meson setup --help`.

- Build the configured build directory with `meson compile`.
- Run tests against the build tree with `meson test`.
- Generate a code coverage report with configuring the build tree with `-Db_coverage=true`
  and running `ninja coverage-text`.
- Install into the configured prefix with `sudo meson install` (`/usr/local` by default).

If you don't want to install umockdev but use it from the build tree, run the
programs with these environment variables, assuming that your current directory is the build
directory:

    LD_LIBRARY_PATH=`pwd` GI_TYPELIB_PATH=`pwd` ./umockdev-run ...

Debugging
=========
To debug umockdev itself and what it's doing, you can set the
`$UMOCKDEV_DEBUG` environment variable to a list (comma or space separated)
of

- `path`: Redirection of paths in `/sys`, `/dev` etc. to testbed
- `netlink`: Redirection of netlink socket and uevent synthesis
- `script`: Script (device reads/writes) recording and replay
- `ioctl`: ioctl recording and replay
- `ioctl-tree`: detailed parsing and traversal of recorded ioctl trees
- `all`: All debug categories

Development
===========
umockdev is being developed and released on https://github.com/martinpitt/umockdev.

umockdev is very much demand driven. If you want to work on a new feature (such
as adding support for more ioctls) or contribute a bug fix, please check out
the git repository, push your changes to github, and create a pull request.
Contributions are appreciated, and I will do my best to provide timely reviews.

If you find a bug in umockdev or have an idea about a new feature but don't
want to implement it yourself, please file a report in the github issue
tracker. Please always include the version of umockdev that you are using, and
a complete runnable reproducer of the problem (i. e. the code and recorded
scripts/ioctls, etc.), unless it is a feature request.

License
=======
- Copyright (C) 2012 - 2014 Canonical Ltd.
- Copyright (C) 2017 - 2023 Martin Pitt

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
