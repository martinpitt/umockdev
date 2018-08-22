/*
 * readbyte.c -- read a byte from a given file, using open()/read() or fopen()/fread()
 *
 * Copyright (C) 2018 Martin Pitt <martin@piware.de>
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
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define writestr(s) assert(write(fd, s, strlen(s)) >= 0)

int
main(int argc, char **argv)
{
    char buf;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s file [open|fopen]\n", argv[0]);
        return 1;
    }

    if (argc == 2 || strcmp(argv[2], "open") == 0) {
        int fd;
        ssize_t len;
        fd = open(argv[1], O_RDWR);
        if (fd < 0) {
            perror("open");
            return 1;
        }
        len = read(fd, &buf, 1);
        if (len < 0) {
            perror("read");
            return 1;
        }
        close(fd);
    }

    else if (strcmp(argv[2], "fopen") == 0) {
        FILE *f;
        size_t len;

        f = fopen(argv[1], "rb");
        if (f == NULL) {
            perror("fopen");
            return 1;
        }
        len = fread(&buf, 1, 1, f);
        if (len != 1 && ferror(f) != 0) {
            perror("fread");
            return 1;
        }
        fclose(f);
    }

    else {
        fprintf(stderr, "ERROR: Unknown mode %s\n", argv[2]);
        return 1;
    }


    return 0;
}
