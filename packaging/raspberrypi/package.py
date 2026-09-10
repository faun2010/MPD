#!/usr/bin/env python3
"""Create an ARM runtime bundle, matching source archive and relative checksums.

Run in the cross-build image after committing the release sources and passing
verify.py. Host glibc and the ALSA configuration data remain system dependencies.
"""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATHS = ["AUTHORS", "COPYING", "LICENSES", "NEWS", "README.md", "install.sh",
                "run-local.sh", "mybuild.sh",
                "meson.build", "meson_options.txt", "mpd.svg", "valgrind.suppressions",
                "src", "test", "subprojects", "python", "doc", "systemd", "android",
                "win32", "packaging/raspberrypi"]
SYSTEM_LIBS = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
               "libresolv.so.2", "libutil.so.1", "libanl.so.1"}
# Everything that differs between the 64-bit and 32-bit Raspberry Pi targets.
TARGETS = {
    "arm64": {
        "name": "mpd-sacd-arm64-bookworm", "build_dir": "build/rpi-arm64",
        "triplet": "aarch64-linux-gnu", "deb_arch": "arm64",
        "elf_machine": 183, "machine": "aarch64",
        "loader": "/lib/ld-linux-aarch64.so.1",
        "qemu": ["qemu-aarch64", "-cpu", "cortex-a53"],
        "target": "aarch64-linux-gnu, ARMv8-A, little endian",
        "source_archive": "mpd-sacd-arm64-source.tar.gz",
        "checksums": "SHA256SUMS-arm64",
        "readme": "packaging/raspberrypi/README-运行说明.md",
        "suite": "Bookworm", "glibc": (2, 36),
    },
    "armhf": {
        "name": "mpd-sacd-armhf-bookworm", "build_dir": "build/rpi-armhf",
        "triplet": "arm-linux-gnueabihf", "deb_arch": "armhf",
        "elf_machine": 40, "machine": "armv7l",
        "loader": "/lib/ld-linux-armhf.so.3",
        "qemu": ["qemu-arm", "-cpu", "cortex-a7"],
        "target": "arm-linux-gnueabihf, ARMv7-A hard-float, little endian",
        "source_archive": "mpd-sacd-armhf-source.tar.gz",
        "checksums": "SHA256SUMS-armhf",
        "readme": "packaging/raspberrypi/README-运行说明-armhf.md",
        "suite": "Bookworm", "glibc": (2, 36),
    },
    # Raspbian Bullseye.  The libraries are extracted into a sysroot rather than
    # installed, so provenance comes from the manifests bullseye-sysroot.sh
    # writes instead of dpkg-query.
    "armhf-bullseye": {
        "name": "mpd-sacd-armhf-bullseye", "build_dir": "build/rpi-armhf-bullseye",
        "triplet": "arm-linux-gnueabihf", "deb_arch": "armhf",
        "elf_machine": 40, "machine": "armv7l",
        "loader": "/lib/ld-linux-armhf.so.3",
        "qemu": ["qemu-arm", "-cpu", "cortex-a7"],
        "target": "arm-linux-gnueabihf, ARMv7-A hard-float, little endian",
        "source_archive": "mpd-sacd-armhf-bullseye-source.tar.gz",
        "checksums": "SHA256SUMS-armhf-bullseye",
        "readme": "packaging/raspberrypi/README-运行说明-armhf-bullseye.md",
        "suite": "Bullseye", "glibc": (2, 31),
        "sysroot": "/sysroot/bullseye-armhf",
    },
}


def output(command):
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(data)
    return digest.hexdigest()


def check_elf(path, spec):
    # 32-bit ARM ELFs use ELFCLASS32; AArch64 uses ELFCLASS64.
    elf_class = b"\x01" if spec["elf_machine"] == 40 else b"\x02"
    with path.open("rb") as stream:
        header = stream.read(20)
    if (header[:5] != b"\x7fELF" + elf_class or header[5:6] != b"\x01" or len(header) != 20
            or struct.unpack("<H", header[18:20])[0] != spec["elf_machine"]):
        raise RuntimeError(f"Not a little-endian {spec['machine']} ELF: {path.name}")


def runpath_dirs(path, triplet, sysroot):
    """Directories a library names itself, e.g. libpulse's private pulseaudio/."""
    dump = subprocess.check_output([triplet + "-readelf", "-d", str(path)], text=True)
    dirs = []
    for value in re.findall(r"Library (?:runpath|rpath): \[(.+?)\]", dump):
        for entry in value.split(":"):
            if not entry:
                continue
            if "$ORIGIN" in entry or "${ORIGIN}" in entry:
                entry = entry.replace("${ORIGIN}", "$ORIGIN").replace("$ORIGIN", str(path.parent))
                dirs.append(Path(entry))
            elif entry.startswith("/"):
                dirs.append(Path(str(sysroot or "")) / entry.lstrip("/"))
    return dirs


def read_sysroot_index(sysroot):
    index = {}
    for line in (sysroot / "FILES.tsv").read_text().splitlines():
        path, _, package = line.partition("\t")
        if package:
            index[path] = package
    return index


def owning_package(source, deb_arch):
    # Debian package records may use /lib or /usr/lib on a merged-/usr image.
    paths = [str(source), str(source.resolve())]
    paths.extend(path[4:] for path in list(paths) if path.startswith("/usr/lib/"))
    paths.extend("/usr" + path for path in list(paths) if path.startswith("/lib/"))
    for path in dict.fromkeys(paths):
        result = subprocess.run(["dpkg-query", "-S", path], capture_output=True, text=True)
        if result.returncode == 0:
            for line in result.stdout.splitlines():
                package, separator, _ = line.partition(": ")
                if separator and package.endswith(":" + deb_arch):
                    return package
    raise RuntimeError(f"Cannot identify Debian package for bundled {source.name}")


def write_checksums(bundle):
    files = sorted(path for path in bundle.rglob("*") if path.is_file()
                   and path != bundle / "SHA256SUMS")
    (bundle / "SHA256SUMS").write_text("".join(
        f"{sha256(path)}  {path.relative_to(bundle).as_posix()}\n" for path in files))


def archive_tree(directory, archive, timestamp):
    def normalize(info):
        info.uid = info.gid = 0
        info.uname = info.gname = "root"
        info.mtime = timestamp
        return info
    with archive.open("wb") as stream, gzip.GzipFile(fileobj=stream, mode="wb", mtime=timestamp) as gz:
        with tarfile.open(fileobj=gz, mode="w") as tar:
            tar.add(directory, arcname=directory.name, filter=normalize)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=sorted(TARGETS), default="arm64")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/releases")
    parser.add_argument("--validation", type=Path)
    parser.add_argument("--replace", action="store_true", help="Replace a prior bundle from this packager")
    args = parser.parse_args()
    spec = TARGETS[args.arch]
    triplet = spec["triplet"]
    sysroot = Path(spec["sysroot"]) if spec.get("sysroot") else None
    sysroot_index = read_sysroot_index(sysroot) if sysroot else None
    system_libs = SYSTEM_LIBS | {Path(spec["loader"]).name}
    build = (args.build_dir or ROOT / spec["build_dir"]).resolve()
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    name = spec["name"]
    target = destination / name
    if target.exists():
        marker = target / "BUILD.json"
        if (not args.replace or not marker.is_file()
                or json.loads(marker.read_text()).get("package") != name):
            parser.error(f"Output already exists; --replace only replaces a prior {name} bundle")

    # Archive committed release content; exclude workstation demo and IDE state.
    revision = output(["git", "rev-parse", "HEAD"])
    subprocess.run(["git", "diff", "--exit-code", "HEAD", "--", *SOURCE_PATHS], cwd=ROOT, check=True)
    for required in ("install.sh", "packaging/raspberrypi/package.py"):
        subprocess.run(["git", "cat-file", "-e", revision + ":" + required], cwd=ROOT, check=True)
    # meson's vcs_tag records `git describe --dirty=+`, so this is a describe
    # string rather than a bare hash; a trailing '+' marks a dirty work tree.
    compiled_version = re.search(r'GIT_VERSION\[\] = "([^"]*)"',
                                 (build / "GitVersion.cxx").read_text())
    if not compiled_version or not compiled_version[1] or compiled_version[1].endswith("+"):
        raise RuntimeError("Build must identify an unmodified Git source revision")
    compiled_revision = output(["git", "rev-parse", compiled_version[1] + "^{commit}"])
    # Packaging/docs may be committed after compilation; executable source may not change.
    subprocess.run(["git", "diff", "--exit-code", compiled_revision, revision, "--",
                    "src", "meson.build", "meson_options.txt", "subprojects", "python",
                    ":(glob)test/**/*.cxx", ":(glob)test/**/*.hxx", ":(glob)test/**/meson.build"],
                   cwd=ROOT, check=True)
    timestamp = int(os.environ.get("SOURCE_DATE_EPOCH", output(["git", "show", "-s", "--format=%ct", revision])))
    validation_path = args.validation or build / "verification/VALIDATION.json"
    validation = json.loads(validation_path.read_text())
    if validation.get("binary_sha256") != sha256(build / "mpd"):
        raise RuntimeError("VALIDATION.json does not identify this build's executable")
    if not validation.get("unit_test_groups_passed") or not validation.get("decode_checks"):
        raise RuntimeError("Unit-test and decoder verification are required before packaging")

    with tempfile.TemporaryDirectory(prefix=f".mpd-{args.arch}-package-", dir=destination) as temporary:
        stage = Path(temporary)
        bundle = stage / name
        for directory in ("bin", "lib", "licenses", "source"):
            (bundle / directory).mkdir(parents=True)
        shutil.copy2(build / "mpd", bundle / "bin/mpd")
        check_elf(bundle / "bin/mpd", spec)
        subprocess.run([triplet + "-strip", str(bundle / "bin/mpd")], check=True)
        pending = [bundle / "bin/mpd"]
        copied = set()
        runtime_packages = set()
        prefix = sysroot or Path("/")
        search = [prefix / "lib" / triplet, prefix / "usr/lib" / triplet]
        glibc_versions = set()
        while pending:
            file = pending.pop()
            check_elf(file, spec)
            symbols = output([triplet + "-readelf", "--version-info", str(file)])
            glibc_versions.update(tuple(map(int, v.split("."))) for v in
                                  re.findall(r"\bGLIBC_([0-9]+\.[0-9]+(?:\.[0-9]+)?)\b", symbols))
            dynamic = output([triplet + "-readelf", "-d", str(file)])
            for dependency in re.findall(r"Shared library: \[(.+?)\]", dynamic):
                if "/" in dependency:
                    raise RuntimeError(f"Non-relocatable ELF dependency: {dependency}")
                if dependency in system_libs or dependency in copied:
                    continue
                # A library may point at a private directory of its own.
                lookup = runpath_dirs(file, triplet, sysroot) + search
                source = next((directory / dependency for directory in lookup
                               if (directory / dependency).is_file()), None)
                if source is None:
                    raise RuntimeError(f"Unresolved dependency: {dependency}")
                copied.add(dependency)
                if sysroot_index is not None:
                    key = "/" + str(source.resolve().relative_to(sysroot))
                    owner = sysroot_index.get(key)
                    if owner is None:
                        raise RuntimeError(f"Cannot identify package for bundled {dependency}")
                    runtime_packages.add(owner)
                else:
                    runtime_packages.add(owning_package(source, spec["deb_arch"]))
                target_lib = bundle / "lib" / dependency
                shutil.copy2(source.resolve(), target_lib)
                pending.append(target_lib)
        required_glibc = max(glibc_versions)
        if required_glibc > spec["glibc"]:
            raise RuntimeError(f"Dependencies require glibc {required_glibc}, "
                               f"exceeding the {spec['suite']} baseline {spec['glibc']}")

        (bundle / "mpd").write_text('''#!/bin/sh
set -eu
bundle_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$bundle_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$bundle_dir/bin/mpd" "$@"
''')
        (bundle / "mpd").chmod(0o755)
        shutil.copy2(ROOT / "install.sh", bundle / "install.sh")
        (bundle / "install.sh").chmod(0o755)
        shutil.copy2(ROOT / spec["readme"], bundle / "README.md")
        shutil.copy2(ROOT / "packaging/raspberrypi/mpd.conf.example", bundle / "mpd.conf.example")
        shutil.copy2(ROOT / "COPYING", bundle / "licenses/MPD-COPYING")
        shutil.copytree(ROOT / "LICENSES", bundle / "licenses/MPD")
        if sysroot:
            arm_packages = [line for line in
                            (sysroot / "PACKAGES.tsv").read_text().splitlines()[1:] if line]
        else:
            packages = output(["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}\t${Architecture}\n"])
            arm_packages = [line for line in packages.splitlines()
                            if line.endswith("\t" + spec["deb_arch"])]
        (bundle / "BUILD-PACKAGES.tsv").write_text(
            "binary_package\tversion\tsource_package\tsource_version\tarchitecture\n"
            + "\n".join(arm_packages) + "\n")
        doc_root = (sysroot / "usr/share/doc") if sysroot else Path("/usr/share/doc")
        for line in arm_packages:
            package = line.split("\t")[0].split(":")[0]
            notice = doc_root / package / "copyright"
            if notice.is_file():
                shutil.copy2(notice, bundle / "licenses" / (package + "-copyright"))
            elif line.split("\t")[0] in runtime_packages:
                raise RuntimeError(f"Missing copyright notice for bundled runtime package: {package}")
        # Debian copyright notices refer to these standard license texts.
        shutil.copytree("/usr/share/common-licenses", bundle / "licenses/common-licenses", symlinks=False)
        (bundle / "source/DEPENDENCIES.md").write_text(
            f"The bundled libraries are unmodified Debian {spec['suite']} {spec['deb_arch']} binaries.\n"
            "BUILD-PACKAGES.tsv records binary and source package names and exact versions.\n"
            "Corresponding Debian source packages (including Debian patches) are available from\n"
            "https://deb.debian.org/debian/pool/ and https://snapshot.debian.org/package/ .\n"
            "Use the source_package/source_version columns, retaining Debian epoch/version suffixes.\n"
            "MPD corresponding source and build/install scripts are included in mpd-source.tar.gz.\n")
        source_archive = bundle / "source/mpd-source.tar.gz"
        with source_archive.open("wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=timestamp) as gz:
            archive = subprocess.run(["git", "archive", "--format=tar", "--prefix=mpd-source/",
                                      revision, "--", *SOURCE_PATHS], cwd=ROOT, check=True, stdout=subprocess.PIPE)
            gz.write(archive.stdout)

        # Validate dynamic linking using only the relocated bundle's non-glibc libraries.
        loader = subprocess.run([*spec["qemu"], "-L", str(sysroot or "/"), "-E",
                                 "LD_LIBRARY_PATH=" + str(bundle / "lib"),
                                 str(sysroot or "") + spec["loader"],
                                 "--list", str(bundle / "bin/mpd")],
                                check=True, capture_output=True, text=True, timeout=60)
        if "not found" in loader.stdout + loader.stderr:
            raise RuntimeError("Bundle dependency resolution failed")
        resolved = dict(re.findall(r"^\s*(\S+) => (.+?) \(0x[0-9a-fA-F]+\)\s*$",
                                   loader.stdout, re.MULTILINE))
        for dependency in copied:
            if resolved.get(dependency) != str(bundle / "lib" / dependency):
                raise RuntimeError(f"Dependency escaped the bundle: {dependency}")
        validation["bundled_dynamic_loader_check"] = "passed"
        validation["tested_build_binary_sha256"] = validation.pop("binary_sha256")
        validation["binary_sha256"] = sha256(bundle / "bin/mpd")
        validation["max_required_glibc"] = ".".join(map(str, required_glibc))
        (bundle / "VALIDATION.json").write_text(json.dumps(validation, indent=2) + "\n")
        manifest = {
            "package": name, "machine": spec["machine"], "elf_machine": spec["elf_machine"],
            "version": json.loads((build / "meson-info/intro-projectinfo.json").read_text())["version"],
            "target": spec["target"],
            "minimum_glibc": ".".join(map(str, spec["glibc"])),
            "compiler": output(["clang++-16", "--version"]).splitlines()[0],
            "source_commit": revision, "compiled_source_commit": compiled_revision,
            "source_archive_sha256": sha256(source_archive),
            "binary_sha256": sha256(bundle / "bin/mpd"), "bundled_libraries": sorted(copied),
            "runtime_packages": sorted(runtime_packages),
            "system_dependencies": ["glibc >= " + ".".join(map(str, spec["glibc"])),
                                    "libasound2-data (ALSA configuration)"],
            "hardware_playback_verified": False,
        }
        (bundle / "BUILD.json").write_text(json.dumps(manifest, indent=2) + "\n")
        write_checksums(bundle)
        subprocess.run(["sha256sum", "--check", "--quiet", "SHA256SUMS"], cwd=bundle, check=True)
        release_archive = stage / (name + ".tar.gz")
        archive_tree(bundle, release_archive, timestamp)
        source_release = stage / spec["source_archive"]
        shutil.copy2(source_archive, source_release)
        if target.exists():
            shutil.rmtree(target)
        shutil.move(str(bundle), target)
        for file in (release_archive, source_release):
            os.replace(file, destination / file.name)
        archives = [destination / (name + ".tar.gz"), destination / spec["source_archive"]]
        (destination / spec["checksums"]).write_text("".join(f"{sha256(p)}  {p.name}\n" for p in archives))
    print(json.dumps(manifest, indent=2))
    print(f"Bundle: {target}")


if __name__ == "__main__":
    main()
