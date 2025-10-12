/*
 * chatter-socket.c -- do some read/writes on a given Unix stream socket, for
 * testing socket r/w recording
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
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <sys/socket.h>
#include <sys/un.h>

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
    struct sockaddr_un addr;
    char buf[100];
    int len;

    if (argc != 2)
        errx(EXIT_FAILURE, "Usage: %s socket\n", argv[0]);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        err(EXIT_FAILURE, "socket");

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
        err(EXIT_FAILURE, "connect");

    writestr(fd, "What is your name?\n");
    len = read(fd, buf, sizeof(buf) - 1);
    assert(len >= 0);
    buf[len] = 0;
    printf("Got name: %s\n", buf);
    writestr(fd, "hello ");
    writestr(fd, buf);

    /* test send/recv, too */
    usleep(20000);
    len = send(fd, "send()", 6, 0);
    assert(len == 6);

    len = recv(fd, buf, sizeof(buf) - 1, 0);
    assert(len > 0);
    buf[len] = 0;
    printf("Got recv: %s\n", buf);

    close(fd);
    return 0;
}
