# Copyright (C) 2012 Canonical Ltd.
# Author: Martin Pitt <martin.pitt@ubuntu.com>
#
#  umockdev is free software; you can redistribute it and/or modify it
#  under the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation; either version 2.1 of the License, or
#  (at your option) any later version.
#
#  umockdev is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
#  Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public License
#  along with this program; If not, see <http://www.gnu.org/licenses/>.

ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}

bin_SCRIPTS =
lib_LTLIBRARIES =
INSTALL_EXEC_HOOKS =
TESTS =
TESTS_ENVIRONMENT = LD_LIBRARY_PATH=.libs:$$LD_LIBRARY_PATH src/umockdev-wrapper

AM_CFLAGS = -Werror -Wall

# -------------------------------------------------------------
# preload library

lib_LTLIBRARIES += libumockdev-preload.la

libumockdev_preload_la_SOURCES = \
	src/libumockdev-preload.c

libumockdev_preload_la_CFLAGS = \
	$(AM_CFLAGS)		\
	-fvisibility=default	\
	$(NULL)

libumockdev_preload_la_LDFLAGS = \
	$(AM_LDFLAGS)		\
	$(NULL)

# this is an LD_PRELOAD library, so remove static library and libtool wrappers
libumockdev-preload-install-hook:
	rm $(DESTDIR)$(libdir)/libumockdev-preload.a
	rm $(DESTDIR)$(libdir)/libumockdev-preload.so
	rm $(DESTDIR)$(libdir)/libumockdev-preload.la

INSTALL_EXEC_HOOKS += libumockdev-preload-install-hook

# -------------------------------------------------------------
# preload wrapper

bin_SCRIPTS += src/umockdev-wrapper

install-exec-hook: $(INSTALL_EXEC_HOOKS)
