/*
 * dlopen-ctor.c -- library for test-umockdev: Its constructor runs inside
 * dlopen(), i.e. with glibc's dynamic loader lock held, and calls back into
 * the test program.
 *
 * Copyright (C) 2026 Martin Pitt <martin@piware.de>
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

extern void dlopen_ctor_hook(void);

__attribute__((constructor))
static void
init(void)
{
    dlopen_ctor_hook();
}
