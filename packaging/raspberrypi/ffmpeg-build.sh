#!/bin/sh
# Build FFmpeg into the Bullseye sysroot, replacing Debian's.
#
# Two reasons this is not optional.  Bullseye ships FFmpeg 4.3, but the dvdaiso
# decoder uses AVChannelLayout/ch_layout, which arrived in 5.1, so DVD-Audio
# cannot be built against it at all.  And Debian's FFmpeg links codec2, librsvg,
# x265, aom and (via libxml2) ICU, which drag ~97 MB of video and text
# machinery into a bundle that only ever plays audio.
#
# Nothing is trimmed from FFmpeg itself: every decoder, demuxer, parser, filter
# and protocol stays enabled, so no playable format is lost.  Only the external
# libraries (--disable-autodetect) and the command line tools are dropped.
set -eu
SYSROOT=${SYSROOT:-/sysroot/bullseye-armhf}
VERSION=${FFMPEG_VERSION:-7.1.5}
TRIPLET=arm-linux-gnueabihf
# --cpu=cortex-a7 makes FFmpeg pick its own -mcpu, which conflicts with an
# explicit -march; only the ABI flag it does not set is passed here.
FLAGS='-mfloat-abi=hard'

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" \
    | tar -xJ -C "$work"
cd "$work/ffmpeg-$VERSION"

PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib/$TRIPLET/pkgconfig:$SYSROOT/usr/share/pkgconfig" \
PKG_CONFIG_SYSROOT_DIR="$SYSROOT" \
./configure \
    --prefix=/usr \
    --libdir="/usr/lib/$TRIPLET" \
    --shlibdir="/usr/lib/$TRIPLET" \
    --enable-cross-compile --cross-prefix="$TRIPLET-" \
    --cc="$TRIPLET-gcc-12" --arch=arm --cpu=cortex-a7 --target-os=linux \
    --enable-neon \
    --sysroot="$SYSROOT" \
    --extra-cflags="$FLAGS" --extra-ldflags="$FLAGS" \
    --enable-shared --disable-static \
    --disable-autodetect --enable-zlib --enable-bzlib \
    --disable-programs --disable-doc --disable-avdevice \
    --disable-debug

make -j"${JOBS:-6}"
# Debian's FFmpeg would otherwise stay resolvable through its own soname.
rm -f "$SYSROOT"/usr/lib/"$TRIPLET"/libav*.so* \
      "$SYSROOT"/usr/lib/"$TRIPLET"/libsw*.so* \
      "$SYSROOT"/usr/lib/"$TRIPLET"/libpostproc.so*
# Debian installs the FFmpeg headers under the multiarch include directory,
# which the compiler searches too, so both copies must go.
rm -rf "$SYSROOT"/usr/include/libav*/ "$SYSROOT"/usr/include/libsw*/ \
       "$SYSROOT"/usr/include/libpostproc/ \
       "$SYSROOT/usr/include/$TRIPLET"/libav*/ \
       "$SYSROOT/usr/include/$TRIPLET"/libsw*/ \
       "$SYSROOT/usr/include/$TRIPLET"/libpostproc/
make install DESTDIR="$SYSROOT"

# The packager requires a copyright notice for every bundled runtime package.
mkdir -p "$SYSROOT/usr/share/doc/ffmpeg"
{
    echo "FFmpeg $VERSION, built from https://ffmpeg.org/releases/ with"
    echo "--disable-autodetect, so no external library is linked and no GPL"
    echo "component is enabled.  The result is covered by the LGPL v2.1 or later."
    echo
    cat LICENSE.md
    echo
    cat COPYING.LGPLv2.1
} > "$SYSROOT/usr/share/doc/ffmpeg/copyright"

printf 'ffmpeg\t%s\tffmpeg\t%s\tsource\n' "$VERSION" "$VERSION" \
    >> "$SYSROOT/PACKAGES.tsv"
for f in "$SYSROOT"/usr/lib/"$TRIPLET"/lib[as][vw]*.so.*; do
    [ -f "$f" ] || continue
    printf '%s\tffmpeg\n' "/usr/lib/$TRIPLET/$(basename "$f")" >> "$SYSROOT/FILES.tsv"
done
echo "=== FFmpeg $VERSION installed into $SYSROOT ==="
