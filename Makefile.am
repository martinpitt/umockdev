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
bin_PROGRAMS =
lib_LTLIBRARIES =
pkgconfiglib_DATA =
noinst_PROGRAMS =
INSTALL_EXEC_HOOKS =
UNINSTALL_EXEC_HOOKS =
CLEANFILES =
DISTCLEANFILES =
TESTS =

EXTRA_DIST = COPYING README.rst NEWS devices
DISTCHECK_CONFIGURE_FLAGS = --enable-gtk-doc
TESTS_ENVIRONMENT = TOP_BUILDDIR=$(top_builddir) \
		    TOP_SRCDIR=$(top_srcdir) \
		    LD_LIBRARY_PATH=$(top_builddir)/.libs:$$LD_LIBRARY_PATH \
		    GI_TYPELIB_PATH=$(builddir) \
		    $(srcdir)/src/umockdev-wrapper

# use this for running Python tests
PYTEST = $(TESTS_ENVIRONMENT) $(PYTHON) -Wd -Werror::PendingDeprecationWarning -Werror::DeprecationWarning

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
	-Werror=return-type		\
	-Werror=uninitialized		\
	-Wall

# this allows us to use regexp literals without warnings
AM_VALAFLAGS = --enable-experimental

# -------------------------------------------------------------
# preload library

lib_LTLIBRARIES += libumockdev-preload.la

libumockdev_preload_la_SOURCES = \
	src/ioctl_tree.c	 \
	src/ioctl_tree.h	 \
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
# main library

lib_LTLIBRARIES += libumockdev.la

libumockdev_includedir = $(includedir)/umockdev-1.0/

libumockdev_include_HEADERS = src/umockdev.h src/uevent_sender.h

libumockdev_la_SOURCES =	\
	src/umockdev.vala	\
	src/uevent_sender.vapi	\
	src/uevent_sender.c	\
	$(NULL)

libumockdev_la_VALAFLAGS =		\
	--pkg gio-2.0			\
	-H src/umockdev.h		\
	--library=UMockdev-1.0		\
	--vapi=src/umockdev-1.0.vapi	\
	--gir=UMockdev-1.0.gir		\
	$(NULL)

libumockdev_la_CFLAGS =		\
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

typelibdir = $(libdir)/girepository-1.0
typelib_DATA = UMockdev-1.0.typelib

girdir = $(datadir)/gir-1.0
gir_DATA = UMockdev-1.0.gir

vapidir = $(datadir)/vala/vapi
vapi_DATA = src/umockdev-1.0.vapi

# no way to insert "Rename to:" into annotations, so hack it
UMockdev-1.0-hacked.gir: $(srcdir)/UMockdev-1.0.gir
	sed '/name="add_devicev"/ s/icev">$$/icev" shadows="add_device">/' $< > $@

UMockdev-1.0.typelib: UMockdev-1.0-hacked.gir
	g-ir-compiler -l libumockdev.so -o $@ $<

pkgconfiglib_DATA += src/umockdev-1.0.pc
EXTRA_DIST += src/umockdev-1.0.pc.in src/umockdev-1.0.vapi UMockdev-1.0.gir

CLEANFILES += UMockdev-1.0.typelib src/umockdev-1.0.pc UMockdev-1.0-hacked.gir
DISTCLEANFILES += src/umockdev.h src/umockdev.c src/umockdev-1.0.vapi UMockdev-1.0.gir 

# this really wants to say "Build the .vapi and .gir first", in a way that
# automake understands; without that, a parallel build of all consumers of
# these two will fail
BUILT_SOURCES = libumockdev.la

# -------------------------------------------------------------
# tools

bin_SCRIPTS += src/umockdev-wrapper
EXTRA_DIST += src/umockdev-wrapper

bin_PROGRAMS += src/umockdev-run src/umockdev-record

src_umockdev_run_SOURCES = 		\
	src/umockdev-run.vala		\
	src/posix_extra.vapi		\
	$(NULL)

src_umockdev_run_CFLAGS = $(GLIB_CFLAGS) -I $(srcdir)/src -I src -w
src_umockdev_run_LDADD = $(GLIB_LIBS) $(GUDEV_LIBS) libumockdev.la
src_umockdev_run_VALAFLAGS =		\
	--vapidir $(top_builddir)/src	\
	--pkg umockdev-1.0		\
	--pkg glib-2.0			\
	--pkg posix			\
	$(NULL)

src_umockdev_record_SOURCES = src/umockdev-record.vala src/posix_extra.vapi
src_umockdev_record_CFLAGS = $(GLIB_CFLAGS) -w
src_umockdev_record_LDADD = $(GLIB_LIBS)
src_umockdev_record_VALAFLAGS =		\
	--pkg glib-2.0			\
	--pkg posix			\
	$(NULL)

DISTCLEANFILES += \
	$(srcdir)/umockdev-run.c			\
	$(srcdir)/umockdev-record.c			\
	$(srcdir)/src_umockdev_run_vala.stamp		\
	$(srcdir)/src_umockdev_record_vala.stamp	\
	$(NULL)

# -------------------------------------------------------------
# tests

noinst_PROGRAMS += tests/test-umockdev tests/test-ioctl-tree

tests_test_umockdev_SOURCES = tests/test-umockdev.c

tests_test_umockdev_CFLAGS =	\
	$(AM_CFLAGS)		\
	$(GLIB_CFLAGS)		\
	$(LIBUDEV_CFLAGS)	\
	$(GUDEV_CFLAGS)		\
	-I $(top_srcdir)/src	\
	$(NULL)

tests_test_umockdev_LDADD =	\
	libumockdev.la		\
	$(GLIB_LIBS)		\
	$(LIBUDEV_LIBS)		\
	$(GUDEV_LIBS)		\
	$(NULL)

tests_test_ioctl_tree_SOURCES =	\
	src/ioctl_tree.c	\
	tests/test-ioctl-tree.c	\
	$(NULL)

tests_test_ioctl_tree_CFLAGS =	\
	$(AM_CFLAGS)		\
	$(GLIB_CFLAGS)		\
	-I $(top_srcdir)/src	\
	$(NULL)

tests_test_ioctl_tree_LDADD =	\
	$(GLIB_LIBS)		\
	$(NULL)

TESTS += tests/test-umockdev tests/test-ioctl-tree

EXTRA_DIST += tests/test-umockdev.py tests/xorg-dummy.conf

if HAVE_INTROSPECTION
if HAVE_PYTHON
check-local: UMockdev-1.0.typelib
	@echo "  Running gobject-introspection test with $(PYTHON)"
	$(PYTEST) $(srcdir)/tests/test-umockdev.py
endif


noinst_PROGRAMS += tests/test-umockdev-vala tests/test-integration tests/test-umockdev-record

tests_test_umockdev_vala_SOURCES = 	\
	tests/test-umockdev-vala.vala	\
	tests/assertions.vapi		\
	tests/ioctl.vapi		\
	$(NULL)

tests_test_umockdev_vala_CFLAGS = $(GLIB_CFLAGS) $(GUDEV_CFLAGS) -I $(srcdir)/src -I src -w
tests_test_umockdev_vala_LDADD = $(GLIB_LIBS) $(GUDEV_LIBS) libumockdev.la
tests_test_umockdev_vala_VALAFLAGS =		\
	--vapidir tests			\
	--vapidir $(top_builddir)/src	\
	--pkg umockdev-1.0		\
	--pkg glib-2.0			\
	--pkg gudev-1.0			\
	$(NULL)

tests_test_integration_SOURCES = 	\
	tests/test-integration.vala	\
	tests/assertions.vapi		\
	$(NULL)

tests_test_integration_CFLAGS = $(GLIB_CFLAGS) -w
tests_test_integration_LDADD = $(GLIB_LIBS)
tests_test_integration_VALAFLAGS =	\
	--vapidir tests			\
	--pkg glib-2.0			\
	--pkg posix			\
	$(NULL)

tests_test_umockdev_record_SOURCES = 	\
	tests/test-umockdev-record.vala	\
	tests/assertions.vapi		\
	src/posix_extra.vapi		\
	$(NULL)

tests_test_umockdev_record_CFLAGS = $(GLIB_CFLAGS) -I $(srcdir)/src -I src -w
tests_test_umockdev_record_LDADD = $(GLIB_LIBS) libumockdev.la
tests_test_umockdev_record_VALAFLAGS =	\
	--vapidir src			\
	--vapidir tests			\
	--pkg glib-2.0			\
	--pkg posix			\
	--pkg umockdev-1.0		\
	$(NULL)

DISTCLEANFILES += \
	tests/test-umockdev-vala.c	\
	tests/test-integration.c	\
	tests/test-umockdev-record.c	\
	$(srcdir)/tests_test_umockdev_vala_vala.stamp	\
	$(srcdir)/tests_test_integration_vala.stamp	\
	$(srcdir)/tests_test_umockdev_record_vala.stamp	\
	$(NULL)

TESTS += tests/test-umockdev-vala tests/test-integration tests/test-umockdev-record

endif

# -------------------------------------------------------------
# documentation
if ENABLE_GTK_DOC
SUBDIRS += \
        docs/reference
endif

doc_DATA = \
	 README.rst	\
	 $(NULL)

EXAMPLES = \
	docs/examples/battery.c 	\
	docs/examples/battery.py	\
	$(NULL)

examplesdir = $(docdir)/examples
examples_DATA = $(EXAMPLES)
EXTRA_DIST += $(EXAMPLES)

# -------------------------------------------------------------
# developer tools

indent:
	indent -linux -ppi4 -psl -l120 -i4 src/*.h src/*.c tests/*.c
	find -name '*~' -exec rm '{}' \;

install-exec-hook: $(INSTALL_EXEC_HOOKS)
uninstall-hook: $(UNINSTALL_EXEC_HOOKS)
