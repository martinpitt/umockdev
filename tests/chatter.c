/*
 * chatter.c -- do some read/writes on a given device, for testing device r/w recording
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

#include <stdio.h>
#include <assert.h>

int main (int argc, char** argv)
{
    FILE *dev;
    char buf[100];

    if (argc != 2) {
        fprintf(stderr, "Usage: %s device\n", argv[0]);
        return 1;
    }

    dev = fopen(argv[1], "r+");
    if (dev == NULL) {
        perror("fopen:");
        return 1;
    }
    fputs("Hello world!\n", dev);
    fputs("What is your name?\n", dev);
    assert(fgets(buf, sizeof(buf), dev) > 0);
    fprintf(dev, "I ♥ %s\n  and\t a tab and a line break in one write\n", buf);
    assert(fgets(buf, sizeof(buf), dev) > 0);
    fprintf(dev, "you said '%s'\n", buf);
    fclose(dev);
    return 0;
}
