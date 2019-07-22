#!/bin/sh

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

set -e

mkdir -p m4
if type gtkdocize > /dev/null; then
    gtkdocize --docdir docs/
    args="--enable-gtk-doc"
else
    echo "gtk-doc not installed, you will not be able to generate documentation."
    echo 'EXTRA_DIST =' > docs/gtk-doc.make
fi

ax_coverage="$(aclocal --print)/ax_code_coverage.m4"
if ! cp --verbose "$ax_coverage" m4; then
    echo "ax_code_coverage.m4 (from autoconf-archive) not available, using dummy"
    echo 'AC_DEFUN([AX_CODE_COVERAGE],[[CODE_COVERAGE_RULES=''] AC_SUBST([CODE_COVERAGE_RULES])])' > m4/ax_code_coverage.m4
fi

if type lcov >/dev/null 2>&1; then
    args="$args --enable-code-coverage"
else
    echo "lcov not installed, not enabling code coverage"
fi

autoreconf --install
[ -n "$NOCONFIGURE" ] || ./configure $args "$@"
