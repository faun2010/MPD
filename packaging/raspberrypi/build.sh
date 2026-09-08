#!/bin/sh
# Run inside the Debian Bookworm cross-build image; never installs on the host.
set -eu
cd "$(dirname "$0")/../.."
build_dir=${BUILD_DIR:-build/rpi-arm64}
if [ -f "$build_dir/meson-private/coredata.dat" ]; then
    setup_mode=--reconfigure
else
    setup_mode=
fi
meson setup $setup_mode "$build_dir" --cross-file packaging/raspberrypi/aarch64.ini \
    --buildtype=release --wrap-mode=nofallback \
    -Dauto_features=disabled -Dtest=true -Db_ndebug=false \
    -Dsacdiso=enabled -Ddvdaiso=disabled -Ddsd=true -Dalsa=enabled \
    -Dflac=enabled -Dffmpeg=enabled -Dsoxr=enabled -Dicu=enabled \
    -Dpcre=enabled -Dzlib=enabled -Dsqlite=enabled -Dipv6=enabled \
    -Dupnp=disabled -Dzeroconf=disabled -Ddocumentation=disabled
ninja -C "$build_dir" -j "${JOBS:-6}"
meson test -C "$build_dir" --print-errorlogs
