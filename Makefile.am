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

pkgconfiglibdir=$(libdir)/pkgconfig

SUBDIRS = .
bin_SCRIPTS =
lib_LTLIBRARIES =
pkgconfiglib_DATA =
noinst_PROGRAMS =
INSTALL_EXEC_HOOKS =
UNINSTALL_EXEC_HOOKS =
CLEANFILES =
TESTS =

EXTRA_DIST = COPYING Makefile.vapigen
DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc
TESTS_ENVIRONMENT = LD_LIBRARY_PATH=.libs:$$LD_LIBRARY_PATH $(srcdir)/src/umockdev-wrapper

# use this for running Python tests
PYTEST = env LD_LIBRARY_PATH=.libs GI_TYPELIB_PATH=src/ $(srcdir)/src/umockdev-wrapper $(PYTHON) -Wd -Werror::PendingDeprecationWarning -Werror::DeprecationWarning

AM_CFLAGS = 	\
	-Werror=missing-prototypes	\
	-Werror=strict-prototypes	\
	-Werror=nested-externs		\
	-Werror=pointer-arith		\
	-Werror=implicit-function-declaration \
	-Werror=pointer-arith		\
	-Werror=init-self		\
	-Werror=format-security		\
	-Werror=format=2		\
	-Werror=missing-include-dirs	\
	-Werror=unused-variable		\
	-Wall

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
	-ldl			\
	$(NULL)

# this is an LD_PRELOAD library, so remove static library and libtool wrappers
libumockdev-preload-install-hook:
	rm $(DESTDIR)$(libdir)/libumockdev-preload.a
	rm $(DESTDIR)$(libdir)/libumockdev-preload.so
	rm $(DESTDIR)$(libdir)/libumockdev-preload.la

libumockdev-preload-uninstall-hook:
	rm $(DESTDIR)$(libdir)/libumockdev-preload.so.*

INSTALL_EXEC_HOOKS += libumockdev-preload-install-hook
UNINSTALL_EXEC_HOOKS += libumockdev-preload-uninstall-hook

# -------------------------------------------------------------
# preload wrapper

bin_SCRIPTS += src/umockdev-wrapper

EXTRA_DIST += src/umockdev-wrapper

# -------------------------------------------------------------
# main library

lib_LTLIBRARIES += libumockdev.la

libumockdev_includedir = $(includedir)/umockdev-1.0/

libumockdev_include_HEADERS = src/umockdev.h

libumockdev_la_SOURCES =	\
	src/umockdev.c		\
	src/uevent_sender.h	\
	src/uevent_sender.c	\
	$(NULL)

libumockdev_la_CFLAGS =		\
	$(AM_CFLAGS)		\
	$(GLIB_CFLAGS)		\
	$(LIBUDEV_CFLAGS)	\
	$(NULL)

libumockdev_la_LIBADD =		\
	$(GLIB_LIBS)		\
	$(LIBUDEV_LIBS)		\
	$(NULL)

# see http://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html
libumockdev_la_LDFLAGS =	\
	$(AM_LDFLAGS)		\
	-version-info 0:0:0	\
	-export-dynamic -no-undefined \
	-export-symbols-regex '^umockdev_.*' \
	$(NULL)

pkgconfiglib_DATA += src/umockdev-1.0.pc
EXTRA_DIST += src/umockdev-1.0.pc.in
CLEANFILES += src/umockdev-1.0.pc

# -------------------------------------------------------------
# introspection for main library

if HAVE_INTROSPECTION
include $(INTROSPECTION_MAKEFILE)

INTROSPECTION_GIRS = src/UMockdev-1.0.gir
INTROSPECTION_SCANNER_ARGS = --add-include-path=$(srcdir)/src
INTROSPECTION_COMPILER_ARGS = --includedir=$(srcdir)/src

src/UMockdev-1.0.gir: libumockdev.la
src_UMockdev_1_0_gir_INCLUDES = GObject-2.0
src_UMockdev_1_0_gir_CFLAGS = 
src_UMockdev_1_0_gir_LIBS = libumockdev.la

src_UMockdev_1_0_gir_FILES =	\
	$(libumockdev_include_HEADERS)	\
	src/umockdev.c			\
	$(NULL)

src_UMockdev_1_0_gir_SCANNERFLAGS =	\
	--pkg-export=umockdev-1.0	\
	--symbol-prefix=umockdev	\
	--warn-all			\
	$(NULL)

girdir = $(datadir)/gir-1.0
gir_DATA = $(INTROSPECTION_GIRS)
typelibdir = $(libdir)/girepository-1.0
typelib_DATA = $(INTROSPECTION_GIRS:.gir=.typelib)
CLEANFILES += $(gir_DATA) $(typelib_DATA)

endif

# -------------------------------------------------------------
# .vapi build

if HAVE_INTROSPECTION
if HAVE_VAPIGEN
-include $(srcdir)/Makefile.vapigen

EXTRA_DIST += src/UMockdev-1.0.metadata
VAPIGEN_VAPIS = src/umockdev-1.0.vapi

src_umockdev_1_0_vapi_GIR = $(INTROSPECTION_GIRS)
src_umockdev_1_0_vapi_FILES = $(src_umockdev_1_0_vapi_GIR)

vapidir = $(datarootdir)/vala/vapi/
vapi_DATA = $(VAPIGEN_VAPIS)

CLEANFILES += $(VAPIGEN_VAPIS)
endif
endif

# -------------------------------------------------------------
# tools
if HAVE_PYTHON

bin_SCRIPTS += src/umockdump
EXTRA_DIST += src/umockdump

# adjust shebang line to detected PYTHON
umockdump-install-hook:
	sed -i '1 s_[^/]\+$$_$(PYTHON)_' $(DESTDIR)$(bindir)/umockdump

INSTALL_EXEC_HOOKS += umockdump-install-hook
endif

# -------------------------------------------------------------
# tests

noinst_PROGRAMS += test-umockdev

test_umockdev_SOURCES = src/test-umockdev.c

test_umockdev_CFLAGS =		\
	$(AM_CFLAGS)		\
	$(GLIB_CFLAGS)		\
	$(GUDEV_CFLAGS)		\
	$(NULL)

test_umockdev_LDADD =		\
	libumockdev.la		\
	$(GLIB_LIBS)		\
	$(GUDEV_LIBS)		\
	$(NULL)

TESTS += test-umockdev

EXTRA_DIST += src/test-umockdev.py src/test-umockdump

if HAVE_INTROSPECTION
if HAVE_PYTHON
check-local: $(INTROSPECTION_GIRS)
	@echo "  Running gobject-introspection test with $(PYTHON)"
	$(PYTEST) $(srcdir)/src/test-umockdev.py
	$(PYTEST) $(srcdir)/src/test-umockdump
endif

if HAVE_VALAC

noinst_PROGRAMS += test-umockdev-vala

test_umockdev_vala_vala.stamp: $(VAPIGEN_VAPIS)

test_umockdev_vala_SOURCES = src/test-umockdev-vala.vala
test_umockdev_vala_CFLAGS = $(GLIB_CFLAGS) $(GUDEV_CFLAGS) -I $(srcdir)/src -I src
test_umockdev_vala_LDADD = $(GLIB_LIBS) $(GUDEV_LIBS) libumockdev.la
test_umockdev_vala_VALAFLAGS =	\
	--vapidir src		\
	--pkg umockdev-1.0	\
	--pkg glib-2.0		\
	--pkg gudev-1.0		\
	--pkg assertions	\
	$(NULL)


CLEANFILES += \
	$(test_umockdev_vala_SOURCES:.vala=.c)	\
	*_vala.stamp				\
	$(NULL)

TESTS += test-umockdev-vala

EXTRA_DIST += src/assertions.vapi

endif
endif

# -------------------------------------------------------------
if ENABLE_GTK_DOC
SUBDIRS += \
        docs/reference
endif


install-exec-hook: $(INSTALL_EXEC_HOOKS)
uninstall-hook: $(UNINSTALL_EXEC_HOOKS)
