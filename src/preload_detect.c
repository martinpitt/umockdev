/*
 * Copyright © 2014 Canonical Ltd.
 * Author: Christopher James Halse Rogers <christopher.halse.rogers@canonical.com>
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

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>

#include "preload_detect.h"

int umockdev_preload_provides_open()
{
    Dl_info info;

    /* Clear last dlerror */
    dlerror();
    
    if (dladdr(&open, &info) == 0) {
        fprintf(stderr, "Failed to find shared object providing open(): %s\n", dlerror());
        exit(1);
    }

    return strstr(info.dli_fname, "libumockdev-preload") != NULL;
}