#!/bin/sh
# Rebuild Chromaprint against the sysroot, without FFmpeg.
#
# Debian's libchromaprint is linked to FFmpeg 4.3 for its FFT (av_rdft_*), an
# API removed in FFmpeg 7.  Keeping it would either pull Debian's whole FFmpeg
# tree back into the bundle or fail to link.  Bullseye has no kissfft package,
# so this builds against FFTW3's single precision library instead, keeping
# MPD's fingerprint support without depending on FFmpeg's FFT.
#
# libstdc++ is linked statically here for the same reason as in mpd: GCC 12
# may emit references above Bullseye's GLIBCXX_3.4.28.  Chromaprint's public
# interface is C, so a private C++ runtime inside the library is self-contained.
set -eu
SYSROOT=${SYSROOT:-/sysroot/bullseye-armhf}
VERSION=${CHROMAPRINT_VERSION:-1.5.1}
TRIPLET=arm-linux-gnueabihf
FLAGS='-marm -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard'
# bullseye-sysroot.sh moved GCC 12's C++ headers out of the toolchain's own
# include directory, so point at them the same way the meson cross file does.
CXX_HEADERS='/opt/gcc12-cxx/12'
CXX_FLAGS="$FLAGS -nostdinc++ -isystem $CXX_HEADERS -isystem $CXX_HEADERS/$TRIPLET -isystem $CXX_HEADERS/backward"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL "http://deb.debian.org/debian/pool/main/c/chromaprint/chromaprint_$VERSION.orig.tar.gz" \
    | tar -xz -C "$work"
src=$(find "$work" -maxdepth 1 -mindepth 1 -type d)

cat > "$work/toolchain.cmake" <<TOOLCHAIN
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_SYSROOT $SYSROOT)
set(CMAKE_C_COMPILER $TRIPLET-gcc-12)
set(CMAKE_CXX_COMPILER $TRIPLET-g++-12)
set(CMAKE_C_FLAGS_INIT "$FLAGS")
set(CMAKE_CXX_FLAGS_INIT "$CXX_FLAGS")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
TOOLCHAIN

cmake -S "$src" -B "$work/build" \
    -DCMAKE_TOOLCHAIN_FILE="$work/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DLIB_SUFFIX="/$TRIPLET" \
    -DFFT_LIB=fftw3f \
    -DBUILD_TOOLS=OFF -DBUILD_TESTS=OFF \
    -DCMAKE_SHARED_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build "$work/build" -j "${JOBS:-6}"
rm -f "$SYSROOT"/usr/lib/"$TRIPLET"/libchromaprint.so*
DESTDIR="$SYSROOT" cmake --install "$work/build"

printf 'chromaprint\t%s\tchromaprint\t%s\tsource\n' "$VERSION" "$VERSION" \
    >> "$SYSROOT/PACKAGES.tsv"
for f in "$SYSROOT"/usr/lib/"$TRIPLET"/libchromaprint.so.*; do
    [ -f "$f" ] || continue
    printf '%s\tchromaprint\n' "/usr/lib/$TRIPLET/$(basename "$f")" >> "$SYSROOT/FILES.tsv"
done
echo "=== Chromaprint $VERSION (fftw3f) installed into $SYSROOT ==="
