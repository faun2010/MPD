#!/bin/sh
# All paths are relative to this script, never to the caller's working directory.
exec python3 - "$0" "$@" <<'PY'
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote, urlparse

CACHE_DIRS = ('__pycache__', '.pytest_cache', '.mypy_cache', '.ruff_cache')
ARTIFACT_DIRS = ('build', 'output', '_dl', '.cache', *CACHE_DIRS[1:],
                 'android/.gradle', 'android/build', 'android/app/build')
# Personal tool directories that the cache search never enters.
SKIP_WALK = ('.git', '.agents', '.codex', '.aws')
# Local Git configuration replayed by the extreme-mode recovery script.
CONFIG_PATTERN = r'^(remote|branch)\.'
# Clone fetches only branches and tags; notes are fetched explicitly.
NOTES_REFSPEC = '+refs/notes/*:refs/notes/*'


def fail(message):
    raise RuntimeError(message)


def run(*args, capture=False, check=True):
    return subprocess.run(args, check=check,
                          stdout=subprocess.PIPE if capture else None).stdout


def git(*args, check=True):
    return run('git', '-C', str(ROOT), *args, capture=True, check=check)


def git_text(*args):
    return git(*args).decode().strip()


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as inp:
        for block in iter(lambda: inp.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def present(path):
    return path.exists() or path.is_symlink()


def make_writable(top):
    # Top-down, so each directory is opened up before os.walk lists it.
    os.chmod(top, os.stat(top).st_mode | stat.S_IRWXU)
    for base, dirs, _ in os.walk(top, followlinks=False):
        for name in dirs:
            path = os.path.join(base, name)
            if not os.path.islink(path):
                os.chmod(path, os.stat(path).st_mode | stat.S_IRWXU)


def remove(path):
    if path.is_symlink() or not path.is_dir():
        path.unlink()
        return
    try:
        shutil.rmtree(path)
    except PermissionError:
        # Read-only directories (e.g. extracted dependency caches) block unlinking.
        make_writable(path)
        shutil.rmtree(path)


def tree_names(entries):
    names = {str(p.relative_to(ROOT)) for p in entries}
    for path in entries:
        if path.is_dir() and not path.is_symlink():
            for base, dirs, files in os.walk(path, followlinks=False):
                names.update(str(Path(base, name).relative_to(ROOT)) for name in dirs + files)
    return names


def split_z(data):
    return [os.fsdecode(item) for item in data.split(b'\0') if item]


def clean_targets():
    # Protect every tracked file and its ancestors, including build/pkg-config.sh.
    tracked = set(split_z(git('ls-files', '-z')))
    parents = {str(p) for name in tracked for p in Path(name).parents}
    targets = []

    def collect(path):
        rel = str(path.relative_to(ROOT))
        if rel in tracked or not present(path):
            return
        if rel in parents:
            # Never traverse a symlink, even if it replaces a tracked directory.
            if path.is_dir() and not path.is_symlink():
                for child in sorted(path.iterdir()):
                    collect(child)
        else:
            targets.append(path)

    for name in ARTIFACT_DIRS:
        collect(ROOT / name)
    # Meson's ignored downloaded/extracted dependencies, preserving .wrap sources.
    for name in split_z(git('ls-files', '--others', '--ignored', '--exclude-standard',
                            '--directory', '-z', '--', 'subprojects')):
        collect(ROOT / name.rstrip('/'))
    blocked = set(targets)
    for base, dirs, _ in os.walk(ROOT, followlinks=False):
        dirs[:] = [d for d in dirs if d not in SKIP_WALK and Path(base, d) not in blocked]
        for name in [d for d in dirs if d in CACHE_DIRS]:
            collect(Path(base, name))
            dirs.remove(name)
    # Collapse nested candidates to prevent attempting to delete them twice.
    unique = set(targets)
    return sorted(p for p in unique if not any(a in unique for a in p.parents))


RESTORE_CODE = r'''
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

DATA = None

def run(*args, capture=False, check=True):
    return subprocess.run(args, check=check, text=True,
                          stdout=subprocess.PIPE if capture else None).stdout

def check_archive(archive):
    digest = hashlib.sha256()
    with archive.open('rb') as inp:
        for block in iter(lambda: inp.read(1024 * 1024), b''):
            digest.update(block)
    if digest.hexdigest() != DATA['sha256']:
        raise RuntimeError('归档 SHA256 不匹配，拒绝恢复')

def clone(stage):
    run('git', 'clone', '--no-local', '--no-checkout', '--origin', DATA['remote'],
        '--', DATA['url'], str(stage))
    run('git', '-C', str(stage), 'fetch', '--quiet', DATA['remote'], DATA['notes_refspec'])
    for ref, oid in DATA['refs']:
        run('git', '-C', str(stage), 'update-ref', ref, oid)
    if DATA['branch']:
        run('git', '-C', str(stage), 'checkout', DATA['branch'], '--')
    else:
        run('git', '-C', str(stage), 'checkout', '--detach', DATA['head'], '--')
    if run('git', '-C', str(stage), 'rev-parse', 'HEAD', capture=True).strip() != DATA['head']:
        raise RuntimeError('恢复的 HEAD 与记录不符')
    # Replace clone's defaults with the original remotes and branch tracking.
    keys = run('git', '-C', str(stage), 'config', '--local', '-z', '--name-only',
               '--get-regexp', DATA['config_pattern'], capture=True, check=False)
    for section in sorted({key.rsplit('.', 1)[0] for key in keys.split('\0') if key}):
        run('git', '-C', str(stage), 'config', '--local', '--remove-section', section)
    for key, value in DATA['config']:
        run('git', '-C', str(stage), 'config', '--local', '--add', key, value)
    cleaner = stage / DATA['cleaner_name']
    cleaner.write_text(DATA['cleaner'])
    cleaner.chmod(0o755)

try:
    here = Path(os.path.realpath(sys.argv[1])).parent
    if len(sys.argv) > 3 or (len(sys.argv) == 3 and sys.argv[2] in ('-h', '--help')):
        print('用法: myrestore.sh [恢复目标目录]；默认恢复到脚本所在目录。')
        sys.exit(0 if len(sys.argv) == 3 else 2)
    target = Path(sys.argv[2]).absolute() if len(sys.argv) == 3 else here
    allowed = {'myrestore.sh'}
    if DATA['mode'] == 'compress':
        allowed.add(DATA['archive'])
        check_archive(here / DATA['archive'])
    if target.exists() and (target.is_symlink() or not target.is_dir()):
        raise RuntimeError('恢复目标必须为普通目录')
    if target.exists():
        if target.resolve() != here:
            allowed = set()
        extra = sorted(p.name for p in target.iterdir() if p.name not in allowed)
        if extra:
            raise RuntimeError('目标含有其他内容 (' + extra[0] + ')；请选择空目录，避免覆盖现有文件')
    created = not target.exists()
    target.mkdir(parents=True, exist_ok=True)
    try:
        # Stage inside the target: no writable parent needed, and moves stay on one filesystem.
        with tempfile.TemporaryDirectory(prefix='.mpd-restore-', dir=target) as tmp:
            stage = Path(tmp) / 'tree'
            if DATA['mode'] == 'compress':
                stage.mkdir()
                run('tar', '-xpzf', str(here / DATA['archive']), '-C', str(stage))
            else:
                clone(stage)
            for child in stage.iterdir():
                os.replace(child, target / child.name)
    except BaseException:
        if created:
            try:
                target.rmdir()
            except OSError:
                pass
        raise
    print('已恢复:', target)
    print('恢复成功后可自行删除 myrestore.sh 和归档，以释放备份空间。')
except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
    print('恢复失败:', exc, file=sys.stderr)
    sys.exit(1)
'''


def write_restore(data):
    # The generated script is self-contained and can also restore into a new directory.
    payload = RESTORE_CODE.replace('DATA = None', 'DATA = ' + repr(data), 1)
    with tempfile.NamedTemporaryFile(mode='w', dir=ROOT, prefix='.myrestore-',
                                     delete=False) as out:
        temp = Path(out.name)
        out.write('#!/bin/sh\nexec python3 - "$0" "$@" <<\'PY\'\n')
        out.write(payload)
        out.write('\nPY\n')
    temp.chmod(0o755)
    os.replace(temp, RESTORE)


def require_standalone_repo():
    # A linked worktree or external Git dir cannot be archived or removed safely.
    dot_git = ROOT / '.git'
    if (not dot_git.is_dir() or dot_git.is_symlink()
            or Path(os.fsdecode(git('rev-parse', '--absolute-git-dir')).strip()).resolve()
            != dot_git):
        fail('压缩/极致模式要求独立仓库，不能使用外部 .git 或 linked worktree')
    worktrees = [e for e in git('worktree', 'list', '--porcelain', '-z').split(b'\0')
                 if e.startswith(b'worktree ')]
    if len(worktrees) != 1:
        fail('还有其他 linked worktree；不能删除它们共用的 .git')
    if present(RESTORE):
        fail('已有 myrestore.sh，请先移走，避免覆盖恢复入口')


def do_clean(dry_run):
    targets = clean_targets()
    print('清理构建、发行产物及依赖缓存（含 build 下的本地测试运行数据）：', flush=True)
    for path in targets:
        print('  ' + str(path.relative_to(ROOT)), flush=True)
    if not targets:
        print('  (无)')
    if dry_run:
        print('预览完成。')
        return
    for path in targets:
        remove(path)
    print('清理完成；源码和 Git 历史已保留。')


def do_compress(dry_run):
    print('完整归档所有内容（含 .git），校验后仅保留 tar.gz 和 myrestore.sh。', flush=True)
    dot_git = ROOT / '.git'
    if ((dot_git / 'objects').is_symlink()
            or os.environ.get('GIT_ALTERNATE_OBJECT_DIRECTORIES')
            or any(p.read_bytes().strip() for p in dot_git.glob('**/objects/info/alternates'))):
        fail('Git 借用了外部对象，归档无法完整恢复；请先解除对象共享')
    if dry_run:
        print('仅预览；未写入归档。')
        return
    entries = list(ROOT.iterdir())
    # The new temporary directory is excluded from the snapshot entries.
    with tempfile.TemporaryDirectory(prefix='.mpd-archive-', dir=ROOT) as tmp:
        pending = Path(tmp) / 'backup.tar.gz'
        inventory = tree_names(entries)
        run('tar', '-czf', str(pending), '-C', str(ROOT), '--', *(p.name for p in entries))
        # Full byte comparison, not just checking that the gzip stream is readable.
        run('tar', '-dzf', str(pending), '-C', str(ROOT))
        if (tree_names(entries) != inventory
                or set(ROOT.iterdir()) != set(entries) | {Path(tmp)}):
            fail('归档期间新增或删除了文件，拒绝清空目录')
        digest = sha256(pending)
        archive = ROOT / ('myclean-backup-' + time.strftime('%Y%m%d-%H%M%S')
                          + '-' + str(os.getpid()) + '.tar.gz')
        if present(archive):
            fail('归档文件名已存在')
        os.replace(pending, archive)
        write_restore({'mode': 'compress', 'archive': archive.name, 'sha256': digest})
        for path in entries:
            remove(path)
    print('压缩清理完成。归档:', archive)


def local_state():
    return (git('status', '--porcelain=v1', '-z', '--untracked-files=all'),
            git('ls-files', '--others', '-z'),
            git('for-each-ref', '--format=%(refname) %(objectname)'),
            git('rev-parse', 'HEAD'),
            git('config', '--local', '-z', '--list'),
            frozenset(ROOT.iterdir()))


def require_remote_recoverable_worktree():
    if git('status', '--porcelain', '--untracked-files=no'):
        fail('存在未提交的源码修改；请先保存，或改用 --compress')
    if git('stash', 'list'):
        fail('存在 stash；请先保存，或改用 --compress')
    if git('submodule', 'status'):
        fail('存在子模块；请改用 --compress 以保留本地状态')
    hooks = sorted(p.name for p in (ROOT / '.git/hooks').glob('*')
                   if not p.name.endswith('.sample'))
    if hooks:
        fail('存在自定义 Git hook (.git/hooks/' + hooks[0] + ')；请备份或改用 --compress')
    targets = clean_targets()
    # Untracked and ignored files alike, except build artifacts and this script.
    for name in split_z(git('ls-files', '--others', '-z')):
        path = ROOT / name
        if path == SCRIPT or any(path == p or p in path.parents for p in targets):
            continue
        fail('有无法从远程恢复的本地文件: ' + name + '；请保存或改用 --compress')


def choose_remote(requested, branch):
    names = git_text('remote').splitlines()
    preferred = None
    if branch:
        preferred = git('config', '--get', 'branch.' + branch + '.remote',
                        check=False).decode().strip() or None
    remote = requested or next((n for n in (preferred, 'mygithub', 'origin', *names)
                                if n in names), None)
    if remote not in names:
        fail('没有可用的恢复远端；请指定 --remote 名称')
    return remote


def resolve_remote_url(remote):
    url = git_text('remote', 'get-url', remote)
    parsed = urlparse(url)
    # Git's rule: "host:path" is scp-like only when no '/' precedes the first ':'.
    if parsed.scheme == 'file':
        source = Path(unquote(parsed.path))
    elif ':' not in url or ('://' not in url and '/' in url.partition(':')[0]):
        source = Path(url)
    else:
        return url
    # Make local fallback remotes independent of the caller's cwd; never
    # delete a repository that is itself the only recovery source.
    source = (ROOT / source).resolve()
    if source == ROOT or ROOT in source.parents:
        fail('恢复来源在将被删除的仓库内部，拒绝极致清理')
    return str(source)


def verify_remote(remote, url, refs):
    with tempfile.TemporaryDirectory(prefix='mpd-remote-check-') as tmp:
        mirror = Path(tmp) / 'remote.git'
        # Use precisely the same fetch scope as the generated recovery script.
        run('git', 'clone', '--no-local', '--no-checkout', '--origin', remote,
            '--', url, str(mirror))
        run('git', '-C', str(mirror), 'fetch', '--quiet', remote, NOTES_REFSPEC)
        for ref, oid in refs:
            result = subprocess.run(['git', '-C', str(mirror), 'cat-file', '-e', oid],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if result.returncode:
                fail('远端无法恢复 ' + ref + ' (' + oid + ')；拒绝删除，请改用 --compress')


def do_extreme(dry_run, requested_remote):
    entries = list(ROOT.iterdir())
    before = local_state()
    require_remote_recoverable_worktree()
    head = git_text('rev-parse', 'HEAD')
    branch = git_text('branch', '--show-current')
    remote = choose_remote(requested_remote, branch)
    url = resolve_remote_url(remote)
    # Every local ref (branches, tags, notes, ...) except remote-tracking ones.
    refs = [line.split('\t') for line in git_text(
                'for-each-ref', '--format=%(refname)\t%(objectname)').splitlines()
            if not line.startswith('refs/remotes/')]
    config = []
    for item in split_z(git('config', '--local', '-z', '--get-regexp', CONFIG_PATTERN,
                            check=False)):
        key, newline, value = item.partition('\n')
        config.append((key, value if newline else 'true'))
    print('远程恢复来源:', remote, url, flush=True)
    print('恢复提交:', head, '分支:', branch or '(detached)', flush=True)
    print('极致模式会删除源码、构建产物和 .git，仅保留 myrestore.sh。', flush=True)
    if dry_run:
        print('仅预览；实际执行还会克隆远端，验证全部本地引用。')
        return
    verify_remote(remote, url, [('HEAD', head), *refs])
    # Refuse newly added files, commits, branches or stashes while verifying.
    if local_state() != before:
        fail('验证期间本地文件或 Git 状态发生变化，拒绝删除')
    write_restore({'mode': 'extreme', 'remote': remote, 'url': url,
                   'head': head, 'branch': branch, 'refs': refs,
                   'config_pattern': CONFIG_PATTERN, 'config': config,
                   'notes_refspec': NOTES_REFSPEC,
                   'cleaner_name': SCRIPT.name, 'cleaner': SCRIPT.read_text()})
    for path in entries:
        remove(path)
    print('极致清理完成，仅保留:', RESTORE)


def parse_args(argv):
    parser = argparse.ArgumentParser(prog='myclean.sh',
        description='清理脚本所在的 MPD 仓库；请先停止构建和本地测试 MPD。',
        epilog='压缩/极致模式会删除 myclean.sh；之后使用留下的 myrestore.sh 恢复。')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('-c', '--compress', action='store_true',
                       help='完整 tar.gz 归档（含 .git 和本地文件），校验后仅保留归档及恢复脚本')
    modes.add_argument('-x', '--extreme', action='store_true',
                       help='仅保留远程恢复脚本；拒绝未保存的源码、stash、hook 和未发布的引用')
    parser.add_argument('-n', '--dry-run', action='store_true',
                        help='只预览，不写入或删除；极致预览不联网')
    parser.add_argument('--remote', help='极致模式的 Git 远端名；默认分支上游、mygithub、origin')
    args = parser.parse_args(argv)
    if args.remote and not args.extreme:
        parser.error('--remote 仅用于 --extreme')
    return args


def main():
    args = parse_args(sys.argv[2:])
    try:
        if git_text('rev-parse', '--show-toplevel') != str(ROOT):
            fail('脚本必须放在仓库根目录')
        print('仓库:', ROOT, flush=True)
        if not (args.compress or args.extreme):
            do_clean(args.dry_run)
            return
        require_standalone_repo()
        if args.compress:
            do_compress(args.dry_run)
        else:
            do_extreme(args.dry_run, args.remote)
        if not args.dry_run:
            print('从任意目录执行恢复脚本即可恢复: ' + shlex.quote(str(RESTORE)))
    except (RuntimeError, OSError, subprocess.CalledProcessError) as exc:
        print('清理失败:', exc, file=sys.stderr)
        sys.exit(1)


SCRIPT = Path(os.path.realpath(sys.argv[1]))
ROOT = SCRIPT.parent
RESTORE = ROOT / 'myrestore.sh'
main()
PY
