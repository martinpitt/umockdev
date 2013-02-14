/*
 * test-integration.vala
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

using Assertions;

//string run_cmd = "LC_ALL=C LD_LIBRARY_PATH=.libs src/umockdev-run";

static bool
have_program (string program)
{
    string sout;
    int exit;

    try {
        Process.spawn_command_line_sync ("which " + program, out sout, null, out exit);
    } catch (SpawnError e) {
        stderr.printf ("cannot call which %s: %s\n", program, e.message);
        Process.abort();
    }

    return exit == 0;
}

static void
check_program_out (string program, string run_command, string expected_out, string expected_err = "")
{
    string sout;
    string serr;
    int exit;

    if (!have_program (program)) {
        stderr.printf ("[SKIP: %s not installed] ", program);
        return;
    }

    string cmd = "env LC_ALL=C LD_LIBRARY_PATH=.libs src/umockdev-run " + run_command;
    try {
        Process.spawn_command_line_sync (cmd, out sout, out serr, out exit);
    } catch (SpawnError e) {
        stderr.printf ("cannot call %s: %s\n", cmd, e.message);
        Process.abort();
    }

    assert_cmpstr (sout, Op.EQ, expected_out);
    assert_cmpstr (serr, Op.EQ, expected_err);
    assert_cmpint (exit, Op.EQ, 0);
}

static void
t_gphoto_detect ()
{
    check_program_out ("gphoto2",
        "-l devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 --auto-detect",
        """Model                          Port            
----------------------------------------------------------
Canon PowerShot SX200 IS       usb:001,011     
""");
}

static void
t_gphoto_folderlist ()
{
    check_program_out ("gphoto2",
        "-l devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 -l",
        """There is 1 folder in folder '/'.
 - store_00010001
There is 1 folder in folder '/store_00010001'.
 - DCIM
There is 1 folder in folder '/store_00010001/DCIM'.
 - 100CANON
There are 0 folders in folder '/store_00010001/DCIM/100CANON'.
""");
}

static void
t_gphoto_filelist ()
{
    check_program_out ("gphoto2",
        "-l devices/cameras/canon-powershot-sx200.umockdev -i /dev/bus/usb/001/011=devices/cameras/canon-powershot-sx200.ioctl -- gphoto2 -L",
        """There is no file in folder '/'.
There is no file in folder '/store_00010001'.
There is no file in folder '/store_00010001/DCIM'.
There are 9 files in folder '/store_00010001/DCIM/100CANON'.
#1     IMG_0095.JPG               rd  1640 KB 3264x2448 image/jpeg
#2     IMG_0096.JPG               rd  1669 KB 3264x2448 image/jpeg
#3     IMG_0097.JPG               rd  1741 KB 3264x2448 image/jpeg
#4     IMG_0098.JPG               rd  1328 KB 3264x2448 image/jpeg
#5     IMG_0099.JPG               rd  1290 KB 3264x2448 image/jpeg
#6     IMG_0100.JPG               rd  2340 KB 3264x2448 image/jpeg
#7     IMG_0101.JPG               rd  1916 KB 3264x2448 image/jpeg
#8     IMG_0102.JPG               rd  2026 KB 3264x2448 image/jpeg
#9     IMG_0103.JPG               rd  1810 KB 3264x2448 image/jpeg
""");
}


int
main (string[] args)
{
  Test.init (ref args);

  // tests with gphoto2 program for PowerShot
  Test.add_func ("/umockdev-integration/gphoto-detect", t_gphoto_detect);
  Test.add_func ("/umockdev-integration/gphoto-folderlist", t_gphoto_folderlist);
  Test.add_func ("/umockdev-integration/gphoto-filelist", t_gphoto_filelist);

  return Test.run();
}
