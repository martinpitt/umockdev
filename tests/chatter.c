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
#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <err.h>

static void
writestr (int fd, const char *s)
{
    int r;
    r = write(fd, s, strlen(s));
    if (r <= 0)
        err(EXIT_FAILURE, "write");
}

int
main(int argc, char **argv)
{
    int fd;
    char buf[100];
    int len;

    if (argc != 2) {
	fprintf(stderr, "Usage: %s device\n", argv[0]);
	return 1;
    }

    fd = open(argv[1], O_RDWR);
    if (fd < 0) {
	perror("open:");
	return 1;
    }
    writestr(fd, "Hello world!\n");
    writestr(fd, "What is your name?\n");
    len = read(fd, buf, sizeof(buf) - 1);
    assert(len >= 0);
    buf[len] = 0;
    printf("Got input: %s", buf);
    writestr(fd, "I ♥ ");
    writestr(fd, buf);
    writestr(fd, "a\t tab and a\n   line break in one write\n");
    len = read(fd, buf, sizeof(buf) - 1);
    assert(len >= 0);
    buf[len] = 0;
    printf("Got input: %s", buf);
    writestr(fd, "bye!\n");
    close(fd);
    return 0;
}
