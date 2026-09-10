#!/bin/sh
# Run inside a cross-build image; never installs on the host.
# CROSS_FILE selects the target: aarch64.ini (default), armhf.ini or
# armhf-bullseye.ini.
set -eu
cd "$(dirname "$0")/../.."
cross_file=${CROSS_FILE:-packaging/raspberrypi/aarch64.ini}

# The Bullseye sysroot carries the same dependencies as the x86_64 release build
# in .github/workflows/release.yml, so it uses that build's options and lets auto
# features follow what is installed: same source, same feature set.  The Bookworm
# images ship a deliberately narrow dependency set and keep the reduced profile;
# widening them means extending their package lists first, or -Dsystemd=enabled
# and its neighbours fail outright.
case $cross_file in
    *armhf-bullseye*)
        default_build_dir=build/rpi-armhf-bullseye
        set -- --buildtype=release -Db_ndebug=true \
            -Ddocumentation=disabled -Dtest=true \
            -Dsystemd=enabled -Dpcre=enabled -Ddsd=true \
            -Dsacdiso=enabled -Ddvdaiso=enabled \
            --wrap-mode nofallback
        ;;
    *)
        case $cross_file in
            *armhf*) default_build_dir=build/rpi-armhf ;;
            *)       default_build_dir=build/rpi-arm64 ;;
        esac
        set -- --buildtype=release --wrap-mode=nofallback \
            -Dauto_features=disabled -Dtest=true -Db_ndebug=false \
            -Dsacdiso=enabled -Ddvdaiso=disabled -Ddsd=true -Dalsa=enabled \
            -Dflac=enabled -Dffmpeg=enabled -Dsoxr=enabled -Dicu=enabled \
            -Dpcre=enabled -Dzlib=enabled -Dsqlite=enabled -Dipv6=enabled \
            -Dupnp=disabled -Dzeroconf=disabled -Ddocumentation=disabled
        ;;
esac

build_dir=${BUILD_DIR:-$default_build_dir}
if [ -f "$build_dir/meson-private/coredata.dat" ]; then
    setup_mode=--reconfigure
else
    setup_mode=
fi
meson setup $setup_mode "$build_dir" --cross-file "$cross_file" "$@"
ninja -C "$build_dir" -j "${JOBS:-6}"

# These shell-script tests run the cross-built binaries directly instead of
# through meson's exe_wrapper, so they cannot work under QEMU.  Skip them by
# name rather than let them fail; verify.py records them as untested.
skip_tests='test_archive_bzip2|test_archive_iso9660|test_archive_zzip'
set -- $(meson test -C "$build_dir" --list | grep -Ev "^($skip_tests)$")
meson test -C "$build_dir" --print-errorlogs "$@"
