#!/bin/sh
set -eu

cd "$(dirname "$0")"
build_dir=${BUILD_DIR:-build/sacd-debug}

if [ -f "$build_dir/meson-private/coredata.dat" ]; then
    meson setup --reconfigure "$build_dir" --buildtype=debugoptimized \
        -Db_ndebug=false -Ddocumentation=disabled -Dtest=true \
        -Dsacdiso=enabled -Ddvdaiso=enabled -Ddsd=true --wrap-mode=nofallback "$@"
else
    meson setup "$build_dir" --buildtype=debugoptimized \
        -Db_ndebug=false -Ddocumentation=disabled -Dtest=true \
        -Dsacdiso=enabled -Ddvdaiso=enabled -Ddsd=true --wrap-mode=nofallback "$@"
fi

ninja -C "$build_dir" -j "${JOBS:-6}"
