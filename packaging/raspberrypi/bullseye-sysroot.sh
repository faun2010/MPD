#!/bin/sh
# Build a Debian Bullseye armhf sysroot so MPD can be linked against glibc 2.31
# (Raspbian Bullseye), then displace the cross toolchain's own Bookworm glibc so
# only this sysroot can satisfy a link.  Runs at image build time.
set -eu
SYSROOT=${SYSROOT:-/sysroot/bullseye-armhf}
APT=/tmp/aptroot
KEYRING=/usr/share/keyrings/debian-archive-keyring.gpg
# Mirrors the dependency list of the x86_64 release build in
# .github/workflows/release.yml, so the Pi gets the same feature set from the
# same source.  apt resolves the runtime packages and the rest of the closure.
PACKAGES="libc6-dev linux-libc-dev
 libgtest-dev libpcre2-dev libfmt-dev
 libsystemd-dev libdbus-1-dev libicu-dev
 libcurl4-gnutls-dev libavahi-client-dev
 libid3tag0-dev libflac-dev libvorbis-dev libopus-dev libogg-dev
 libsndfile1-dev libfaad-dev libmpcdec-dev libwavpack-dev
 libavcodec-dev libavformat-dev libavutil-dev libswresample-dev
 libsamplerate0-dev libsoxr-dev
 libbz2-dev libcdio-paranoia-dev libiso9660-dev libzzip-dev
 libexpat1-dev libasound2-dev libpulse-dev libshout3-dev
 libmpdclient-dev libnfs-dev libupnp-dev libsqlite3-dev
 libchromaprint-dev libgcrypt20-dev libfftw3-dev
 zlib1g-dev"

rm -rf "$APT"; mkdir -p "$APT/etc/apt/apt.conf.d" "$APT/etc/apt/preferences.d" \
    "$APT/etc/apt/sources.list.d" "$APT/var/lib/apt/lists/partial" \
    "$APT/var/cache/apt/archives/partial" "$APT/var/lib/dpkg" "$APT/debs"
: > "$APT/var/lib/dpkg/status"
cat > "$APT/etc/apt/sources.list" <<LIST
deb [arch=armhf signed-by=$KEYRING] http://deb.debian.org/debian bullseye main
LIST
set -- -o "Dir=$APT" -o "Dir::State=$APT/var/lib/apt" \
    -o "Dir::State::status=$APT/var/lib/dpkg/status" \
    -o "Dir::Cache=$APT/var/cache/apt" -o "Dir::Etc=$APT/etc/apt" \
    -o APT::Architecture=armhf -o APT::Architectures::=armhf \
    -o Acquire::Languages=none -o Acquire::Check-Valid-Until=false
apt-get "$@" update
# --download-only resolves the full dependency closure, so the sysroot also
# carries FFmpeg's own indirect dependencies and the link stays fully checked.
# shellcheck disable=SC2086
apt-get "$@" -y --no-install-recommends --download-only install $PACKAGES
rm -rf "$SYSROOT"; mkdir -p "$SYSROOT"
for deb in "$APT"/var/cache/apt/archives/*.deb; do dpkg -x "$deb" "$SYSROOT"; done

# These packages are extracted rather than installed, so dpkg-query cannot say
# where a bundled library came from.  Record it now: PACKAGES.tsv lists binary
# and source package versions, FILES.tsv maps each extracted path to its package.
python3 - "$APT/var/cache/apt/archives" "$SYSROOT" <<'PY'
import subprocess, sys, glob, os
archives, sysroot = sys.argv[1], sys.argv[2]
packages, files = [], []
for deb in sorted(glob.glob(archives + "/*.deb")):
    fields = subprocess.check_output(
        ["dpkg-deb", "-f", deb, "Package", "Version", "Source", "Architecture"],
        text=True)
    info = {}
    for line in fields.splitlines():
        key, _, value = line.partition(": ")
        info[key] = value.strip()
    source = info.get("Source", info["Package"])
    source_version = info["Version"]
    if "(" in source:
        source, _, rest = source.partition(" (")
        source_version = rest.rstrip(")")
    packages.append("\t".join((info["Package"], info["Version"], source,
                               source_version, info["Architecture"])))
    for line in subprocess.check_output(["dpkg-deb", "-c", deb], text=True).splitlines():
        parts = line.split(None, 5)
        if len(parts) >= 6 and parts[0][0] == "-":
            # dpkg-deb -c prints "./usr/...", which already starts with "/"
            # once the leading dot is gone; prepending another gives "//usr",
            # and POSIX normpath keeps exactly two leading slashes.
            path = os.path.normpath(parts[5].split(" -> ")[0].lstrip("."))
            files.append(path + "\t" + info["Package"])
with open(os.path.join(sysroot, "PACKAGES.tsv"), "w") as out:
    out.write("binary_package\tversion\tsource_package\tsource_version\tarchitecture\n")
    out.write("\n".join(sorted(set(packages))) + "\n")
with open(os.path.join(sysroot, "FILES.tsv"), "w") as out:
    out.write("\n".join(sorted(set(files))) + "\n")
PY
# The sysroot is not a chroot, so absolute .so symlinks must become relative.
python3 - "$SYSROOT" <<'PY'
import os, sys
root = os.path.realpath(sys.argv[1])
for base, _, names in os.walk(root):
    for name in names:
        path = os.path.join(base, name)
        if not os.path.islink(path):
            continue
        target = os.readlink(path)
        if not target.startswith("/"):
            continue
        os.remove(path)
        os.symlink(os.path.relpath(root + target, base), path)
PY
# MPD needs fmt >= 9 but Bullseye only has 7.1.  fmt's headers are architecture
# independent and the build uses FMT_HEADER_ONLY, so Bookworm's fmt 9 headers
# are used with no library: the .pc must therefore advertise no Libs.
rm -rf "$SYSROOT/usr/include/fmt"
cp -a /usr/include/fmt "$SYSROOT/usr/include/fmt"
mkdir -p "$SYSROOT/usr/lib/arm-linux-gnueabihf/pkgconfig"
sed -e 's|^prefix=.*|prefix=/usr|' \
    -e 's|^Libs:.*|Libs:|' \
    -e 's|^Cflags:.*|Cflags: -I${includedir} -DFMT_HEADER_ONLY|' \
    /usr/lib/arm-linux-gnueabihf/pkgconfig/fmt.pc \
    > "$SYSROOT/usr/lib/arm-linux-gnueabihf/pkgconfig/fmt.pc"
# The cross toolchain's built-in /usr/arm-linux-gnueabihf is searched before any
# --sysroot, and its libc.so linker script names Bookworm's glibc 2.36 by
# absolute path.  Empty it so the sysroot is the only libc; keep the GCC 12 C++
# headers, which the cross file re-adds with -isystem.
if [ -d /usr/arm-linux-gnueabihf/include/c++/12 ]; then
    mkdir -p /opt/gcc12-cxx
    mv /usr/arm-linux-gnueabihf/include/c++/12 /opt/gcc12-cxx/12
fi
rm -rf /usr/arm-linux-gnueabihf/include/* /usr/arm-linux-gnueabihf/lib/*

# Two libstdc++ symbols postdate glibc 2.31; see glibc231-compat.c.
mkdir -p /opt/glibc231-compat
clang-16 --target=arm-linux-gnueabihf --sysroot="$SYSROOT" \
    -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -O2 -c \
    "$(dirname "$0")/glibc231-compat.c" -o /opt/glibc231-compat/compat.o

echo "=== sysroot ready: $SYSROOT ==="
ls "$SYSROOT/usr/lib/arm-linux-gnueabihf/" | grep -E "libicuuc|libavcodec|libFLAC|libasound|libixml" | head
