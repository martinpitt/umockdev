#!/bin/sh
ROOT=$(dirname $(dirname $(realpath "$0")))
if [ -n "${MESON_SOURCE_ROOT:-}/.git" ] && VER=$(git -C "$MESON_SOURCE_ROOT" describe); then
    # make version number distribution friendly
    VER=$(echo "$VER" | sed 's/-/./g')
    # when invoked as dist script, write the stamp; this is false when invoked from project.version()
    [ -z "${MESON_DIST_ROOT:-}" ] || echo "$VER" > "${MESON_DIST_ROOT}/.version"
    echo "$VER"
# when invoked from a tarball, it should be in the source root
elif [ -e "${ROOT}/.version" ]; then
    cat "${ROOT}/.version"
else
    echo "ERROR: Neither a git checkout nor .version, cannot determine version" >&2
    exit 1
fi
