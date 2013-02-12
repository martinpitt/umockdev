/* umockdev example: use libumockdev in C to fake a battery
 * Build with:
 * gcc battery.c -Wall `pkg-config --cflags --libs umockdev-1.0 gio-2.0` -o /tmp/battery
 * Run with:
 * umockdev-wrapper /tmp/battery
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

#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <glib.h>
#include <gio/gio.h>
#include "umockdev.h"

/* determine upowerd path */
static const char*
upowerd_path(void)
{
    static char path[PATH_MAX];
    gchar *contents, *exec;

    g_assert (g_file_get_contents ("/usr/share/dbus-1/system-services/org.freedesktop.UPower.service",
                                   &contents, NULL, NULL));
    exec = strstr (contents, "Exec=");
    g_assert (exec != NULL);
    *strchr(exec, '\n') = '\0';
    strcpy (path, exec + 5);
    g_free (contents);
    return path;
}

int main()
{
    UMockdevTestbed *testbed;
    const char* sys_bat;
    GTestDBus* dbus;
    GPid upowerd;
    const char* upowerd_argv[] = {upowerd_path(), NULL};

    /* create test bed */
    testbed = umockdev_testbed_new ();

    /* add a battery with good charge */
    sys_bat = umockdev_testbed_add_device (testbed, "power_supply", "fakeBAT0", NULL,
            /* attributes */
            "type", "Battery",
            "present", "1",
            "status", "Discharging",
            "energy_full", "60000000",
            "energy_full_design", "80000000",
            "energy_now", "48000000",
            "voltage_now", "12000000",
            NULL,
            /* properties */
            "POWER_SUPPLY_ONLINE", "1",
            NULL);

    /* start a fake system D-BUS */
    dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
    g_test_dbus_up (dbus);
    g_setenv ("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address (dbus), TRUE);

    puts("-- starting upower on test dbus under umockdev-wrapper");
    g_assert (g_spawn_async_with_pipes (NULL, (gchar**) upowerd_argv, NULL,
                G_SPAWN_STDOUT_TO_DEV_NULL |  G_SPAWN_STDERR_TO_DEV_NULL, NULL,
                NULL, &upowerd, NULL, NULL, NULL, NULL));
    /* give it some time to settle */
    sleep(1);

    puts("-- Initial upower --dump");
    g_assert (g_spawn_command_line_sync ("upower --dump", NULL, NULL, NULL, NULL));

    puts("-- Starting upower monitoring now");
    g_assert (g_spawn_command_line_async ("upower --monitor-detail", NULL));

    sleep(1);
    puts("-- setting battery charge to 2.5\% now");
    umockdev_testbed_set_attribute (testbed, sys_bat, "energy_now", "1500000");
    /* send uevent to notify upowerd */
    umockdev_testbed_uevent (testbed, sys_bat, "change");
    sleep(1);

    /* clean up */
    puts("-- cleaning up");
    kill (upowerd, SIGTERM);
    waitpid (upowerd, NULL, 0);

    g_test_dbus_down (dbus);

    return 0;
}
