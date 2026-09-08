#!/bin/sh
# Install a local MPD SACD build or a verified release bundle. Never elevates privileges.
set -eu

owner='mpd-sacd-install-v1'
prefix=${HOME:?HOME is not set}/.local
binary=
force=no
operation=install
scratch=
stage=
launcher_tmp=

fail() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }
cleanup() {
    [ -z "$launcher_tmp" ] || rm -f -- "$launcher_tmp"
    [ -z "$stage" ] || rm -rf -- "$stage"
    [ -z "$scratch" ] || rm -rf -- "$scratch"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

usage() {
    cat <<'USAGE'
Usage: ./install.sh [--prefix DIR] [--binary FILE] [--check] [--force]
       ./install.sh --uninstall [--prefix DIR]

Default prefix: $HOME/.local. Release bundles supply bin/mpd and SHA256SUMS;
from a source checkout, use --binary build/DIRECTORY/mpd.

  --check       Verify bundle hashes, Linux/ELF architecture and --version only.
  --force       Back up an existing, unmanaged PREFIX/bin/mpd before replacing it.
  --uninstall   Verify and remove this install; restore its saved original mpd.
  --prefix DIR  Install under DIR (for example /usr/local with your own sudo).
  --help        Show this help.

Files live in PREFIX/lib/mpd-sacd; PREFIX/bin/mpd is a launcher with bundled
library support. Upgrades retain verified prior versions. Modified installations
are left untouched. Backups use PREFIX/bin/mpd.before-mpd-sacd.TIMESTAMP.PID.
No downloads, dependency installation, service changes, or user config changes
are performed. Installing a binary does not restart any running MPD process.
USAGE
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --prefix|--binary)
            [ "$#" -ge 2 ] || fail "$1 requires a value"
            [ -n "$2" ] || fail "$1 requires a nonempty value"
            if [ "$1" = --prefix ]; then prefix=$2; else binary=$2; fi
            shift 2 ;;
        --force) force=yes; shift ;;
        --check)
            [ "$operation" = install ] || fail 'choose only one operation'
            operation=check; shift ;;
        --uninstall)
            [ "$operation" = install ] || fail 'choose only one operation'
            operation=uninstall; shift ;;
        --help|-h) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done
[ "$operation" != uninstall ] || {
    [ -z "$binary" ] && [ "$force" = no ] || fail '--uninstall cannot use --binary or --force'
}
case $prefix in /*) ;; *) prefix=$(pwd -P)/$prefix ;; esac
while [ "$prefix" != / ] && [ "${prefix%/}" != "$prefix" ]; do prefix=${prefix%/}; done
root=$prefix/lib/mpd-sacd
launcher=$prefix/bin/mpd
for utility in sha256sum find sort cmp od tr mktemp readlink; do
    command -v "$utility" >/dev/null 2>&1 || fail "required utility is missing: $utility"
done
scratch=$(mktemp -d "${TMPDIR:-/tmp}/mpd-sacd-install.XXXXXXXX")

safe_path() {
    case $1 in ''|/*|.|..|../*|*/../*|*/..|*/./*|./*|*/.|*//*|*[!a-zA-Z0-9_./+-]*) return 1 ;; esac
}

# Inventory includes empty directories, and rejects symlinks and special files.
# Payload names are deliberately restricted; the bundle/prefix path may contain spaces.
inventory() {
    (
        cd "$1"
        find . -mindepth 1 ! -path './.install-files' ! -path './.install-sha256' -print |
            LC_ALL=C sort > "$scratch/find-list"
        while IFS= read -r entry; do
            relative=${entry#./}
            safe_path "$relative" || fail "unsupported payload path: $relative"
            [ ! -L "$entry" ] || fail "payload symlink is not supported: $relative"
            if [ -f "$entry" ]; then printf 'f %s\n' "$relative"
            elif [ -d "$entry" ]; then printf 'd %s\n' "$relative"
            else fail "unsupported payload file: $relative"; fi
        done < "$scratch/find-list"
    )
}

hash_payload() {
    (
        cd "$1"
        while IFS= read -r entry; do
            case $entry in 'f '*) sha256sum -- "${entry#f }" ;; esac
        done < "$2"
    )
}

verify_bundle() {
    bundle_dir=$1
    [ -f "$bundle_dir/BUILD.json" ] || fail 'release bundle is missing BUILD.json'
    [ -f "$bundle_dir/SHA256SUMS" ] || fail 'release bundle is missing SHA256SUMS'
    [ -f "$bundle_dir/bin/mpd" ] || fail 'release bundle is missing bin/mpd'
    [ ! -e "$bundle_dir/.install-files" ] && [ ! -L "$bundle_dir/.install-files" ] &&
        [ ! -e "$bundle_dir/.install-sha256" ] && [ ! -L "$bundle_dir/.install-sha256" ] ||
        fail 'release bundle contains reserved installer metadata'
    inventory "$bundle_dir" > "$scratch/bundle-files"
    : > "$scratch/bundle-expected"
    while IFS= read -r line || [ -n "$line" ]; do
        digest=${line%% *}
        [ "${#digest}" -eq 64 ] || fail 'invalid SHA256SUMS digest'
        case $digest in *[!0-9a-f]*) fail 'invalid SHA256SUMS digest' ;; esac
        case $line in "$digest  "*) relative=${line#"$digest  "} ;; *) fail 'invalid SHA256SUMS format' ;; esac
        relative=${relative#./}
        safe_path "$relative" || fail "unsafe SHA256SUMS path: $relative"
        [ "$relative" != SHA256SUMS ] || fail 'SHA256SUMS must not hash itself'
        printf '%s  %s\n' "$digest" "$relative" >> "$scratch/bundle-expected"
    done < "$bundle_dir/SHA256SUMS"
    LC_ALL=C sort "$scratch/bundle-expected" > "$scratch/bundle-expected-sorted"
    # Recompute the entire tree: extra, missing and duplicated manifest entries fail too.
    sed '/^f SHA256SUMS$/d' "$scratch/bundle-files" > "$scratch/bundle-hashed-files"
    hash_payload "$bundle_dir" "$scratch/bundle-hashed-files" | LC_ALL=C sort > "$scratch/bundle-actual"
    cmp -s "$scratch/bundle-actual" "$scratch/bundle-expected-sorted" || fail 'release checksum or manifest coverage mismatch'
}

verify_release() {
    release_dir=$1
    [ -d "$release_dir" ] && [ ! -L "$release_dir" ] || fail "unmanaged release directory: $release_dir"
    [ -f "$release_dir/.install-files" ] && [ ! -L "$release_dir/.install-files" ] &&
        [ -f "$release_dir/.install-sha256" ] && [ ! -L "$release_dir/.install-sha256" ] ||
        fail "missing installer metadata: $release_dir"
    inventory "$release_dir" > "$scratch/installed-files"
    cmp -s "$scratch/installed-files" "$release_dir/.install-files" || fail "installed files were added or removed: $release_dir"
    hash_payload "$release_dir" "$scratch/installed-files" > "$scratch/installed-sha256"
    cmp -s "$scratch/installed-sha256" "$release_dir/.install-sha256" || fail "installed files were modified: $release_dir"
}

verify_install() {
    [ -d "$root" ] && [ ! -L "$root" ] || fail "refusing unmanaged path: $root"
    [ -f "$root/.owner" ] && [ ! -L "$root/.owner" ] &&
        [ "$(cat "$root/.owner")" = "$owner" ] || fail "refusing unmanaged directory: $root"
    printf '%s\n' .backup .launcher .owner current releases | LC_ALL=C sort > "$scratch/root-expected"
    find "$root" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort > "$scratch/root-actual"
    cmp -s "$scratch/root-expected" "$scratch/root-actual" || fail "unknown files in managed directory: $root"
    [ -d "$root/releases" ] && [ ! -L "$root/releases" ] || fail 'invalid releases directory'
    [ -f "$root/.backup" ] && [ ! -L "$root/.backup" ] || fail 'invalid backup metadata'
    [ -f "$root/.launcher" ] && [ ! -L "$root/.launcher" ] || fail 'invalid launcher metadata'
    [ -f "$launcher" ] && [ ! -L "$launcher" ] && cmp -s "$root/.launcher" "$launcher" ||
        fail "installed launcher was modified or removed: $launcher"
    [ -L "$root/current" ] || fail 'installed current link was modified'
    current=$(readlink "$root/current")
    case $current in releases/*) current_id=${current#releases/} ;; *) fail 'invalid current release link' ;; esac
    case $current_id in ''|*[!0-9.-]*) fail 'invalid current release name' ;; esac
    [ -d "$root/$current" ] || fail 'current release is missing'
    find "$root/releases" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort > "$scratch/releases"
    while IFS= read -r release; do
        case $release in ''|*[!0-9.-]*) fail "unmanaged release: $release" ;; esac
        verify_release "$root/releases/$release"
    done < "$scratch/releases"
    backup=$(cat "$root/.backup")
    if [ -n "$backup" ]; then
        case $backup in mpd.before-mpd-sacd.*) suffix=${backup#mpd.before-mpd-sacd.} ;; *) fail 'invalid backup name' ;; esac
        case $suffix in ''|*[!0-9.]* ) fail 'invalid backup name' ;; esac
        [ -e "$prefix/bin/$backup" ] || [ -L "$prefix/bin/$backup" ] || fail 'saved original mpd is missing'
    fi
}

managed=no
backup=
if [ -e "$root" ] || [ -L "$root" ]; then
    verify_install
    managed=yes
fi
if [ "$operation" = uninstall ]; then
    [ "$managed" = yes ] || fail "no managed installation at $root"
    if [ -n "$backup" ]; then
        # Restore by rename so a failed restore leaves the installed launcher usable.
        mv -Tf -- "$prefix/bin/$backup" "$launcher"
        printf 'Restored original: %s\n' "$launcher"
    else
        rm -- "$launcher"
    fi
    rm -rf -- "$root"
    printf 'Removed MPD SACD installation from %s (services and configuration untouched).\n' "$prefix"
    exit 0
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
bundle=no
if [ -z "$binary" ]; then
    bundle=yes
    verify_bundle "$script_dir"
    binary=$script_dir/bin/mpd
else
    [ -f "$binary" ] || fail "binary does not exist: $binary"
    binary=$(CDPATH= cd -- "$(dirname -- "$binary")" && pwd -P)/$(basename -- "$binary")
fi
[ "$(uname -s)" = Linux ] || fail 'only Linux is supported'
if command -v getconf >/dev/null 2>&1 && [ "$(getconf LONG_BIT 2>/dev/null || :)" = 32 ]; then
    fail '32-bit userland is unsupported; Raspberry Pi requires a 64-bit OS'
fi
magic=$(od -An -tx1 -N4 "$binary" | tr -d ' \n')
elf_class=$(od -An -tu1 -j4 -N1 "$binary" | tr -d ' \n')
elf_endian=$(od -An -tu1 -j5 -N1 "$binary" | tr -d ' \n')
machine=$(od -An -tx1 -j18 -N2 "$binary" | tr -d ' \n')
[ "$magic" = 7f454c46 ] && [ "$elf_class" = 2 ] && [ "$elf_endian" = 1 ] ||
    fail 'binary must be a little-endian 64-bit Linux ELF executable'
case $(uname -m) in
    x86_64) expected_machine=3e00; architecture=x86_64 ;;
    aarch64|arm64) expected_machine=b700; architecture=aarch64 ;;
    *) fail 'unsupported host architecture (32-bit Raspberry Pi OS needs a 64-bit OS)' ;;
esac
[ "$machine" = "$expected_machine" ] || fail "ELF architecture does not match this $architecture host"
[ -x "$binary" ] || fail "binary is not executable: $binary"
if [ "$bundle" = yes ] && [ -d "$script_dir/lib" ]; then
    version_lib=$script_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    if ! LD_LIBRARY_PATH=$version_lib "$binary" --version > "$scratch/version.log" 2>&1; then
        cat "$scratch/version.log" >&2
        fail 'binary cannot run here; check its shared libraries and required GLIBC version'
    fi
else
    if ! "$binary" --version > "$scratch/version.log" 2>&1; then
        cat "$scratch/version.log" >&2
        fail 'binary cannot run here; check its shared libraries and required GLIBC version'
    fi
fi
if [ "$managed" = no ] && { [ -e "$launcher" ] || [ -L "$launcher" ]; }; then
    [ "$force" = yes ] || fail "existing unmanaged $launcher; use --force to back it up and replace it"
    [ ! -d "$launcher" ] || fail "refusing to replace a directory: $launcher"
fi
printf 'Verified Linux %s executable: ' "$architecture"
sed -n '1p' "$scratch/version.log"
if [ "$operation" = check ]; then
    printf 'Preflight passed; no installation changes made.\n'
    exit 0
fi

# New prefix components must be traversable after a root install, even with
# umask 077. mkdir leaves existing prefix/home directory permissions alone.
(umask 022; mkdir -p -- "$prefix/bin" "$prefix/lib")
stage=$(mktemp -d "$prefix/lib/.mpd-sacd-stage.XXXXXXXX")
release_id=$(date +%s).$$
payload=$stage/releases/$release_id
mkdir -p -- "$payload"
if [ "$bundle" = yes ]; then
    cp -R -- "$script_dir/." "$payload/"
    verify_bundle "$payload"
else
    mkdir -- "$payload/bin"
    cp -L -- "$binary" "$payload/bin/mpd"
    cp -- "$script_dir/install.sh" "$payload/install.sh"
fi
# Copied bundle modes and the caller's umask must not make installed programs
# or libraries private to the installing user. Keep staging itself private
# until publication; only normalize this new payload, never retained versions.
find "$payload" -type d -exec chmod 755 {} +
find "$payload" -type f ! -perm /111 -exec chmod 644 {} +
find "$payload" -type f -perm /111 -exec chmod 755 {} +
chmod 755 "$payload/bin/mpd" "$payload/install.sh"
chmod 755 "$stage/releases"
inventory "$payload" > "$scratch/new-files"
cp -- "$scratch/new-files" "$payload/.install-files"
hash_payload "$payload" "$scratch/new-files" > "$payload/.install-sha256"
chmod 644 "$payload/.install-files" "$payload/.install-sha256"
cat > "$stage/.launcher" <<'LAUNCHER'
#!/bin/sh
# Managed by mpd-sacd install.sh; do not edit this launcher.
mpd_sacd_bin_dir=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd) || exit 1
mpd_sacd_payload=$mpd_sacd_bin_dir/../lib/mpd-sacd/current
if [ -d "$mpd_sacd_payload/lib" ]; then
    LD_LIBRARY_PATH=$mpd_sacd_payload/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    export LD_LIBRARY_PATH
fi
exec "$mpd_sacd_payload/bin/mpd" "$@"
LAUNCHER
chmod 755 "$stage/.launcher"
if [ "$managed" = yes ]; then
    # Validate once more immediately before publishing the new release.
    verify_install
    cmp -s "$stage/.launcher" "$root/.launcher" || fail 'installed launcher format differs; uninstall the old version first'
    # Repair the 0700 publication root left by the original installer.
    chmod 755 "$root" "$root/releases"
    mv -- "$payload" "$root/releases/$release_id"
    ln -s "releases/$release_id" "$stage/next-current"
    mv -Tf -- "$stage/next-current" "$root/current"
else
    printf '%s\n' "$owner" > "$stage/.owner"
    if [ -e "$launcher" ] || [ -L "$launcher" ]; then
        backup=mpd.before-mpd-sacd.$release_id
        [ ! -e "$prefix/bin/$backup" ] && [ ! -L "$prefix/bin/$backup" ] || fail 'backup path already exists'
    fi
    printf '%s\n' "$backup" > "$stage/.backup"
    ln -s "releases/$release_id" "$stage/current"
    # Prepare the launcher before moving either the user's original or the payload.
    launcher_tmp=$(mktemp "$prefix/bin/.mpd-sacd-launcher.XXXXXXXX")
    cp -- "$stage/.launcher" "$launcher_tmp"
    chmod 755 "$launcher_tmp"
    chmod 644 "$stage/.owner" "$stage/.backup"
    chmod 755 "$stage"
    if [ -n "$backup" ]; then mv -- "$launcher" "$prefix/bin/$backup"; fi
    if ! mv -T -- "$stage" "$root"; then
        [ -z "$backup" ] || mv -- "$prefix/bin/$backup" "$launcher"
        fail 'could not publish installation'
    fi
    stage=
    # A temporary file beside the destination makes the launcher replacement atomic.
    if ! mv -T -- "$launcher_tmp" "$launcher"; then
        rm -f -- "$launcher_tmp"
        rm -rf -- "$root"
        [ -z "$backup" ] || mv -- "$prefix/bin/$backup" "$launcher"
        fail 'could not publish launcher; original installation restored'
    fi
    launcher_tmp=
fi
printf 'Installed: %s\n' "$launcher"
[ -z "$backup" ] || printf 'Original mpd backup: %s/bin/%s\n' "$prefix" "$backup"
printf 'No services were restarted. Use this path explicitly or put %s/bin on PATH.\n' "$prefix"
printf 'Uninstall: "%s/lib/mpd-sacd/current/install.sh" --uninstall --prefix "%s"\n' "$prefix" "$prefix"
