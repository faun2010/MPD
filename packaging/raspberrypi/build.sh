#!/bin/sh
# Run inside the Debian Bookworm cross-build image; never installs on the host.
# CROSS_FILE selects the target: aarch64.ini (default), armhf.ini or
# armhf-bullseye.ini.
set -eu
cd "$(dirname "$0")/../.."
cross_file=${CROSS_FILE:-packaging/raspberrypi/aarch64.ini}
case $cross_file in
    *armhf-bullseye*) default_build_dir=build/rpi-armhf-bullseye ;;
    *armhf*)          default_build_dir=build/rpi-armhf ;;
    *)                default_build_dir=build/rpi-arm64 ;;
esac
build_dir=${BUILD_DIR:-$default_build_dir}
if [ -f "$build_dir/meson-private/coredata.dat" ]; then
    setup_mode=--reconfigure
else
    setup_mode=
fi
meson setup $setup_mode "$build_dir" --cross-file "$cross_file" \
    --buildtype=release --wrap-mode=nofallback \
    -Dauto_features=disabled -Dtest=true -Db_ndebug=false \
    -Dsacdiso=enabled -Ddvdaiso=disabled -Ddsd=true -Dalsa=enabled \
    -Dflac=enabled -Dffmpeg=enabled -Dsoxr=enabled -Dicu=enabled \
    -Dpcre=enabled -Dzlib=enabled -Dsqlite=enabled -Dipv6=enabled \
    -Dupnp=disabled -Dzeroconf=disabled -Ddocumentation=disabled
ninja -C "$build_dir" -j "${JOBS:-6}"
meson test -C "$build_dir" --print-errorlogs
