#!/bin/sh
# Isolated installer contract tests. No audio hardware, services, or downloads.
# Root is optional: inside a disposable container it enables a cross-user check.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cc=${CC:-cc}
command -v "$cc" >/dev/null 2>&1 || { printf 'C compiler required: %s\n' "$cc" >&2; exit 1; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/mpd-sacd-install-test.XXXXXXXX")
trap 'rm -rf -- "$tmp"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
passed=0
pass() { passed=$((passed + 1)); printf 'ok %s - %s\n' "$passed" "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
expect_failure() {
    if "$@" > "$tmp/failure.log" 2>&1; then
        cat "$tmp/failure.log" >&2
        fail "unexpected success: $*"
    fi
}
assert_log() { grep -F -- "$1" "$tmp/failure.log" >/dev/null || { cat "$tmp/failure.log" >&2; fail "missing error: $1"; }; }
check_version() {
    actual=$(cd /; "$1" --version)
    [ "$actual" = "$2" ] || fail "wrong version: $actual (wanted $2)"
}
make_binary() {
    printf '#include <stdio.h>\nint main(void) { puts("%s"); return 0; }\n' "$2" > "$tmp/main.c"
    "$cc" "$tmp/main.c" -o "$1"
}
make_manifest() {
    (
        cd "$1"
        find . -type f ! -name SHA256SUMS -print | LC_ALL=C sort |
            while IFS= read -r file; do sha256sum -- "${file#./}"; done > SHA256SUMS
    )
}
mkdir -p "$tmp/source with spaces"
make_binary "$tmp/source with spaces/mpd-one" 'test-mpd one'
make_binary "$tmp/source with spaces/mpd-two" 'test-mpd two'
first=$tmp/source\ with\ spaces/mpd-one
second=$tmp/source\ with\ spaces/mpd-two
prefix=$tmp/prefix\ with\ spaces

"$repo/install.sh" --check --binary "$first" --prefix "$prefix" > "$tmp/check.log"
[ ! -e "$prefix" ] || fail '--check created the prefix'
pass 'preflight validates a real ELF without creating prefix'
"$repo/install.sh" --binary "$first" --prefix "$prefix" > "$tmp/install.log"
check_version "$prefix/bin/mpd" 'test-mpd one'
(cd /; PATH="$prefix/bin:$PATH" mpd --version) | grep -Fx 'test-mpd one' >/dev/null
pass 'install and launcher work through PATH and from a different cwd with spaces'
old_current=$(readlink "$prefix/lib/mpd-sacd/current")
"$repo/install.sh" --binary "$second" --prefix "$prefix" > "$tmp/upgrade.log"
check_version "$prefix/bin/mpd" 'test-mpd two'
[ "$(readlink "$prefix/lib/mpd-sacd/current")" != "$old_current" ] || fail 'upgrade did not switch current'
[ -f "$prefix/lib/mpd-sacd/$old_current/bin/mpd" ] || fail 'upgrade removed retained version'
pass 'upgrade switches current and retains prior payload'
"$prefix/lib/mpd-sacd/current/install.sh" --uninstall --prefix "$prefix" > "$tmp/uninstall.log"
[ ! -e "$prefix/bin/mpd" ] && [ ! -e "$prefix/lib/mpd-sacd" ] || fail 'uninstall left managed files'
pass 'installed copy can uninstall its own payload'

foreign=$tmp/foreign
mkdir -p "$foreign/bin"
printf 'original mpd contents\n' > "$foreign/bin/mpd"
cp "$foreign/bin/mpd" "$tmp/original"
expect_failure "$repo/install.sh" --binary "$first" --prefix "$foreign"
assert_log 'existing unmanaged'
cmp "$foreign/bin/mpd" "$tmp/original" || fail 'foreign executable changed without force'
[ ! -e "$foreign/lib" ] || fail 'refused install wrote lib directory'
pass 'foreign executable is refused before writing prefix'
"$repo/install.sh" --binary "$first" --prefix "$foreign" --force > "$tmp/force.log"
backup=$(cat "$foreign/lib/mpd-sacd/.backup")
cmp "$foreign/bin/$backup" "$tmp/original" || fail 'backup differs from original'
check_version "$foreign/bin/mpd" 'test-mpd one'
"$repo/install.sh" --binary "$second" --prefix "$foreign" > "$tmp/force-upgrade.log"
"$repo/install.sh" --uninstall --prefix "$foreign" > "$tmp/force-uninstall.log"
cmp "$foreign/bin/mpd" "$tmp/original" || fail 'uninstall did not restore original'
pass 'force backs up original and upgrade/uninstall restores it'

foreign_root=$tmp/foreign-root
mkdir -p "$foreign_root/lib/mpd-sacd"
printf 'keep\n' > "$foreign_root/lib/mpd-sacd/important"
expect_failure "$repo/install.sh" --binary "$first" --prefix "$foreign_root" --force
assert_log 'refusing unmanaged directory'
[ "$(cat "$foreign_root/lib/mpd-sacd/important")" = keep ] || fail 'foreign root changed'
expect_failure "$repo/install.sh" --uninstall --prefix "$foreign_root"
pass 'force and uninstall never adopt or delete an unknown payload directory'

modified=$tmp/modified
"$repo/install.sh" --binary "$first" --prefix "$modified" > "$tmp/install.log"
cp "$modified/bin/mpd" "$tmp/launcher-original"
printf '# user change\n' >> "$modified/bin/mpd"
expect_failure "$repo/install.sh" --uninstall --prefix "$modified"
assert_log 'launcher was modified'
expect_failure "$repo/install.sh" --binary "$second" --prefix "$modified" --force
assert_log 'launcher was modified'
cp "$tmp/launcher-original" "$modified/bin/mpd"
release=$modified/lib/mpd-sacd/$(readlink "$modified/lib/mpd-sacd/current")
printf 'user extra\n' > "$release/user-file"
expect_failure "$repo/install.sh" --uninstall --prefix "$modified"
assert_log 'added or removed'
[ -f "$release/user-file" ] || fail 'uninstall deleted user extra'
rm "$release/user-file"
mkdir "$release/user-directory"
expect_failure "$repo/install.sh" --uninstall --prefix "$modified"
assert_log 'added or removed'
rmdir "$release/user-directory"
cp "$release/bin/mpd" "$tmp/payload-original"
printf 'user changed binary\n' >> "$release/bin/mpd"
expect_failure "$repo/install.sh" --binary "$second" --prefix "$modified"
assert_log 'files were modified'
expect_failure "$repo/install.sh" --uninstall --prefix "$modified"
assert_log 'files were modified'
cmp -s "$release/bin/mpd" "$tmp/payload-original" && fail 'modified payload was overwritten'
cp "$tmp/payload-original" "$release/bin/mpd"
printf 'outside known layout\n' > "$modified/lib/mpd-sacd/unknown"
expect_failure "$repo/install.sh" --uninstall --prefix "$modified"
assert_log 'unknown files'
rm "$modified/lib/mpd-sacd/unknown"
"$repo/install.sh" --uninstall --prefix "$modified" > "$tmp/uninstall.log"
pass 'modified launcher/content, added files/directories, and unknown root entries remain untouched'

wrong=$tmp/wrong-architecture
cp "$first" "$wrong"
# Pick the other supported architecture's e_machine, leaving the real ELF otherwise intact.
case $(uname -m) in
    x86_64) printf '\267\000' | dd of="$wrong" bs=1 seek=18 conv=notrunc 2>/dev/null ;;
    aarch64|arm64) printf '\076\000' | dd of="$wrong" bs=1 seek=18 conv=notrunc 2>/dev/null ;;
    *) fail 'tests require an x86_64 or aarch64 Linux host' ;;
esac
expect_failure "$repo/install.sh" --check --binary "$wrong" --prefix "$tmp/wrong-prefix"
assert_log 'ELF architecture does not match'
[ ! -e "$tmp/wrong-prefix" ] || fail 'architecture failure wrote prefix'
printf '#!/bin/sh\necho fake\n' > "$tmp/fake"
chmod +x "$tmp/fake"
expect_failure "$repo/install.sh" --check --binary "$tmp/fake"
assert_log '64-bit Linux ELF'
pass 'wrong architecture and shell masquerading as binary are rejected'

bundle=$tmp/bundle\ with\ spaces
mkdir -p "$bundle/bin" "$bundle/lib" "$bundle/licenses"
cp "$repo/install.sh" "$bundle/install.sh"
printf '{"architecture":"test-fixture"}\n' > "$bundle/BUILD.json"
printf 'fixture license\n' > "$bundle/licenses/fixture.txt"
printf '# example, never installed as user config\n' > "$bundle/mpd.conf.example"
printf 'const char *test_version(void) { return "test-mpd bundled"; }\n' > "$tmp/shared.c"
printf '#include <stdio.h>\nextern const char *test_version(void);\nint main(void) { puts(test_version()); return 0; }\n' > "$tmp/shared-main.c"
"$cc" -fPIC -shared "$tmp/shared.c" -Wl,-soname,libmpd_install_fixture.so -o "$bundle/lib/libmpd_install_fixture.so"
"$cc" "$tmp/shared-main.c" -L"$bundle/lib" -lmpd_install_fixture -o "$bundle/bin/mpd"
make_manifest "$bundle"
bundled_prefix=$tmp/bundled\ prefix
"$bundle/install.sh" --check --prefix "$bundled_prefix" > "$tmp/check.log"
[ ! -e "$bundled_prefix" ] || fail 'bundle --check wrote prefix'
"$bundle/install.sh" --prefix "$bundled_prefix" > "$tmp/bundle-install.log"
check_version "$bundled_prefix/bin/mpd" 'test-mpd bundled'
[ -f "$bundled_prefix/lib/mpd-sacd/current/mpd.conf.example" ] || fail 'example config missing from payload'
pass 'release SHA verification and launcher supply bundled shared libraries'

# Installation must work for a different user, not only for the installer UID.
# The runtime tree is public; existing parent permissions remain user-owned.
check_runtime_modes() {
    [ -z "$(find "$1/lib/mpd-sacd" -type d ! -perm 755 -print)" ] || fail 'runtime directory is not 755'
    [ -z "$(find "$1/lib/mpd-sacd/current/bin" -type f ! -perm 755 -print)" ] || fail 'runtime executable is not 755'
    for component in "$1" "$1/bin" "$1/lib"; do
        [ "$(stat -c %a "$component")" = 755 ] || fail "new prefix component is private: $component"
    done
}
restricted=$tmp/restrictive-source
(umask 077; "$repo/install.sh" --binary "$first" --prefix "$restricted") > "$tmp/restrictive.log"
check_runtime_modes "$restricted"
check_version "$restricted/bin/mpd" 'test-mpd one'
existing_parent=$tmp/private-existing-parent
mkdir -m 700 "$existing_parent"
(umask 077; "$repo/install.sh" --binary "$first" --prefix "$existing_parent/prefix") > "$tmp/restrictive-parent.log"
[ "$(stat -c %a "$existing_parent")" = 700 ] || fail 'installer changed an existing parent directory'
pass 'umask 077 installation publishes accessible directories and preserves existing parent modes'

private_bundle=$tmp/private-bundle
cp -R "$bundle" "$private_bundle"
find "$private_bundle" -type d -exec chmod 700 {} +
find "$private_bundle" -type f -exec chmod 600 {} +
chmod 700 "$private_bundle/bin/mpd" "$private_bundle/install.sh"
private_prefix=$tmp/restrictive-bundle
(umask 077; "$private_bundle/install.sh" --prefix "$private_prefix") > "$tmp/restrictive-bundle.log"
check_runtime_modes "$private_prefix"
[ "$(stat -c %a "$private_prefix/lib/mpd-sacd/current/lib/libmpd_install_fixture.so")" = 644 ] || fail 'bundled library is private'
check_version "$private_prefix/bin/mpd" 'test-mpd bundled'
pass 'private bundle modes are normalized for executable and shared-library access'

# Model an installation published by the old script, including a private releases ancestor.
chmod 700 "$restricted/lib/mpd-sacd" "$restricted/lib/mpd-sacd/releases"
(umask 077; "$repo/install.sh" --binary "$second" --prefix "$restricted") > "$tmp/restrictive-upgrade.log"
check_runtime_modes "$restricted"
check_version "$restricted/bin/mpd" 'test-mpd two'
pass 'upgrade repairs the old private publication root and releases directory'

if [ "$(id -u)" = 0 ] && command -v setpriv >/dev/null 2>&1; then
    # Tests create all files as root, then execute with no root UID/GID/groups.
    chmod 755 "$tmp"
    for entry in "$bundled_prefix|test-mpd bundled" "$private_prefix|test-mpd bundled" "$restricted|test-mpd two"; do
        actual=$(setpriv --reuid 65534 --regid 65534 --clear-groups -- "${entry%%|*}/bin/mpd" --version)
        [ "$actual" = "${entry#*|}" ] || fail 'ordinary user cannot run root-installed payload'
    done
    pass 'ordinary UID runs root-installed launchers and shared libraries after install and upgrade'
fi
printf 'tampered\n' >> "$bundle/licenses/fixture.txt"
expect_failure "$bundle/install.sh" --prefix "$tmp/bad-checksum"
assert_log 'checksum or manifest coverage mismatch'
[ ! -e "$tmp/bad-checksum" ] || fail 'checksum failure wrote prefix'
make_manifest "$bundle"
printf 'not listed\n' > "$bundle/extra"
expect_failure "$bundle/install.sh" --check
assert_log 'checksum or manifest coverage mismatch'
rm "$bundle/extra"
printf '%064d  ../outside\n' 0 >> "$bundle/SHA256SUMS"
expect_failure "$bundle/install.sh" --check
assert_log 'unsafe SHA256SUMS path'
make_manifest "$bundle"
ln -s /etc/passwd "$bundle/lib/not-a-library"
expect_failure "$bundle/install.sh" --check
assert_log 'payload symlink'
rm "$bundle/lib/not-a-library"
pass 'tampering, unlisted files, escaping checksum paths and payload symlinks are refused'

# A checksum-valid package with a missing library must fail its execution preflight.
rm "$bundle/lib/libmpd_install_fixture.so"
make_manifest "$bundle"
expect_failure "$bundle/install.sh" --prefix "$tmp/missing-library"
assert_log 'shared libraries and required GLIBC'
[ ! -e "$tmp/missing-library" ] || fail 'runtime preflight failure wrote prefix'
pass 'missing host/bundled runtime dependencies fail before installation'
"$repo/install.sh" --uninstall --prefix "$bundled_prefix" > "$tmp/uninstall.log"

# Preserve an original symlink exactly, including a dangling target.
symlink_prefix=$tmp/symlink-prefix
mkdir -p "$symlink_prefix/bin"
ln -s /does/not/exist "$symlink_prefix/bin/mpd"
"$repo/install.sh" --binary "$first" --prefix "$symlink_prefix" --force > "$tmp/symlink-install.log"
"$repo/install.sh" --uninstall --prefix "$symlink_prefix" > "$tmp/symlink-uninstall.log"
[ "$(readlink "$symlink_prefix/bin/mpd")" = /does/not/exist ] || fail 'original symlink was not restored'
pass 'force backup/uninstall preserves an original dangling symlink'

# Inject failures around publication without requiring root or filling a disk.
mkdir "$tmp/fault-tools"
real_mktemp=$(command -v mktemp)
real_mv=$(command -v mv)
cat > "$tmp/fault-tools/mktemp" <<'FAULT_MKTEMP'
#!/bin/sh
case $1 in
    */bin/.mpd-sacd-launcher.*)
        if [ "${MPD_INSTALL_TEST_FAULT:-}" = launcher-temp ]; then
            printf 'simulated ENOSPC\n' >&2
            exit 73
        fi ;;
esac
exec "$MPD_INSTALL_TEST_MKTEMP" "$@"
FAULT_MKTEMP
cat > "$tmp/fault-tools/mv" <<'FAULT_MV'
#!/bin/sh
if [ "${1:-}" = -T ]; then
    case ${3:-} in
        */bin/.mpd-sacd-launcher.*)
            [ "${MPD_INSTALL_TEST_FAULT:-}" != launcher-publish ] || exit 74 ;;
        */lib/.mpd-sacd-stage.*)
            [ "${MPD_INSTALL_TEST_FAULT:-}" != payload-publish ] || exit 75 ;;
    esac
fi
if [ "${1:-}" = -Tf ]; then
    case ${3:-} in
        */bin/mpd.before-mpd-sacd.*)
            [ "${MPD_INSTALL_TEST_FAULT:-}" != uninstall-restore ] || exit 76 ;;
    esac
fi
exec "$MPD_INSTALL_TEST_MV" "$@"
FAULT_MV
chmod +x "$tmp/fault-tools/mktemp" "$tmp/fault-tools/mv"
for fault in launcher-temp payload-publish launcher-publish; do
    fault_prefix=$tmp/failure-$fault
    mkdir -p "$fault_prefix/bin"
    cp "$tmp/original" "$fault_prefix/bin/mpd"
    expect_failure env PATH="$tmp/fault-tools:$PATH" MPD_INSTALL_TEST_FAULT="$fault" \
        MPD_INSTALL_TEST_MKTEMP="$real_mktemp" MPD_INSTALL_TEST_MV="$real_mv" \
        "$repo/install.sh" --binary "$first" --prefix "$fault_prefix" --force
    cmp -s "$fault_prefix/bin/mpd" "$tmp/original" || fail "$fault lost original mpd"
    [ ! -e "$fault_prefix/lib/mpd-sacd" ] || fail "$fault left a half installation"
    [ "$(find "$fault_prefix/bin" -mindepth 1 -maxdepth 1 -printf '%f\n')" = mpd ] ||
        fail "$fault left launcher or backup debris"
    [ -z "$(find "$fault_prefix/lib" -mindepth 1 -maxdepth 1 -print)" ] ||
        fail "$fault left staged payload debris"
    pass "$fault failure preserves/restores original and removes staging"
done
restore_prefix=$tmp/failure-uninstall-restore
mkdir -p "$restore_prefix/bin"
cp "$tmp/original" "$restore_prefix/bin/mpd"
"$repo/install.sh" --binary "$first" --prefix "$restore_prefix" --force > "$tmp/install.log"
expect_failure env PATH="$tmp/fault-tools:$PATH" MPD_INSTALL_TEST_FAULT=uninstall-restore \
    MPD_INSTALL_TEST_MKTEMP="$real_mktemp" MPD_INSTALL_TEST_MV="$real_mv" \
    "$repo/install.sh" --uninstall --prefix "$restore_prefix"
check_version "$restore_prefix/bin/mpd" 'test-mpd one'
"$repo/install.sh" --uninstall --prefix "$restore_prefix" > "$tmp/uninstall.log"
cmp -s "$restore_prefix/bin/mpd" "$tmp/original" || fail 'uninstall retry failed to restore original'
pass 'failed uninstall restore keeps the launcher usable and allows retry'
printf '%s installer contract checks passed.\n' "$passed"
