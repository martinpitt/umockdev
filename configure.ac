# Copyright (C) 2012-2013 Canonical Ltd.
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

AC_PREREQ([2.64])
AC_INIT([umockdev],
        m4_esyscmd([head -n1 NEWS | cut -f1 -d' ' | tr -d '\n']),
        [https://github.com/martinpitt/umockdev/issues],
        [umockdev],
	[https://github.com/martinpitt/umockdev])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_SRCDIR([src/umockdev-wrapper])
AC_CONFIG_HEADERS([config.h])

AC_SYS_LARGEFILE

AM_SILENT_RULES([yes])
AM_INIT_AUTOMAKE([foreign 1.11 -Wall -Werror -Wno-portability silent-rules subdir-objects tar-pax no-dist-gzip dist-xz])
AM_PROG_CC_C_O
AC_PROG_CC_C99
AM_PROG_AR

LT_PREREQ(2.2)
LT_INIT

# glibc's ioctl takes an 'unsigned long' instead of the POSIX 'int' for the request parameter
AC_CACHE_CHECK([ioctl request parameter type],
    [gl_cv_func_ioctl_request_type],
    [AC_COMPILE_IFELSE(
        [AC_LANG_PROGRAM(
        [[#include <sys/ioctl.h>]],
        [[extern
            #ifdef __cplusplus
            "C"
            #endif
            int ioctl (int, int, ...);
        ]])
        ],
        [gl_cv_func_ioctl_request_type=int],
        [gl_cv_func_ioctl_request_type="unsigned long"]
    ])
AC_DEFINE_UNQUOTED([IOCTL_REQUEST_TYPE], [$gl_cv_func_ioctl_request_type], [ioctl request parameter type]))

# check for gtk-doc
m4_ifdef([GTK_DOC_CHECK], [
GTK_DOC_CHECK([1.14],[--flavour no-tmpl])
],[
AM_CONDITIONAL([ENABLE_GTK_DOC], false)
])

# check for gobject-introspection
m4_ifdef([GOBJECT_INTROSPECTION_CHECK],
	 [GOBJECT_INTROSPECTION_CHECK([1.32])],
	 [AM_CONDITIONAL([HAVE_INTROSPECTION], [false])])


AC_SEARCH_LIBS([dlsym], [dl], [], [AC_MSG_ERROR([Dynamic linking loader library not found])])
PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.32.0 gobject-2.0 >= 2.32.0 gio-2.0 >= 2.32.0 gio-unix-2.0 >= 2.32.0])
PKG_CHECK_MODULES([LIBUDEV], [libudev])
PKG_CHECK_MODULES([GUDEV], [gudev-1.0])


# if we have Python 2 or 3, run the GI test (they will skip if gi.repository is
# not available)
AC_CHECK_PROGS(PYTHON, [python3 python])
AM_CONDITIONAL(HAVE_PYTHON, [test x$PYTHON != x])

AC_CHECK_PROGS(VALAC, [valac])
AM_PROG_VALAC([0.16.1])

# code coverage support
AC_MSG_CHECKING(for GNOME code coverage support)
m4_ifdef([GNOME_CODE_COVERAGE],
         [AC_MSG_RESULT(yes)
          GNOME_CODE_COVERAGE],
         [AC_MSG_RESULT(no)
          GNOME_CODE_COVERAGE_RULES=''
          AC_SUBST([GNOME_CODE_COVERAGE_RULES])
          enable_code_coverage="no"])

CFLAGS="$CFLAGS $CODE_COVERAGE_CFLAGS"
LDFLAGS="$LDFLAGS $CODE_COVERAGE_CFLAGS"

AC_CONFIG_FILES([
	Makefile
	src/umockdev-1.0.pc
	docs/reference/Makefile
	docs/reference/version.xml
])

AC_OUTPUT

AC_MSG_RESULT([
  $PACKAGE_NAME $VERSION
  
  CPPFLAGS:				${CPPFLAGS}
  CFLAGS:				${CFLAGS}
  LDFLAGS:				${LDFLAGS}

  gobject-introspection support:	${enable_introspection}
  code coverage support:		${enable_code_coverage}])

if test x$PYTHON != x; then
  AC_MSG_RESULT([  Python:				${PYTHON}])
else
  AC_MSG_RESULT([
  WARNING! No Python interpreter found. Some tests cannot be run.])
fi
