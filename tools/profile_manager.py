#!/usr/bin/env python3
"""Native disk-profile checkpoints. Restores reboot; this is not a live save state."""
import argparse
import copy
from contextlib import contextmanager
import ctypes
import errno
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
import time
import uuid

FILES = ('nandflash.img', 'otp.bin')
NAME = re.compile(r'[a-z0-9][a-z0-9._-]{0,63}\Z')
TEMP = re.compile(r'\.pm-tmp-[0-9a-f]{32}\Z')
SHA = re.compile(r'[0-9a-f]{64}\Z')
UNSUPPORTED = {errno.ENOTSUP, errno.EOPNOTSUPP, errno.EXDEV, errno.ENOSYS}
if sys.platform.startswith('linux'):
    UNSUPPORTED |= {errno.EINVAL, errno.ENOTTY}


class ProfileError(RuntimeError):
    pass


def checked_name(name):
    if not isinstance(name, str) or not NAME.fullmatch(name):
        raise ProfileError('Use 1–64 lowercase ASCII letters, digits, dots, underscores or hyphens; start with a letter/digit')
    return name


def sync_dir(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def guard(s):
    return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)


def regular_fd(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    s = os.fstat(fd)
    if not stat.S_ISREG(s.st_mode) or s.st_nlink != 1:
        os.close(fd)
        raise ProfileError(f'Expected an independent regular file: {path}')
    return fd


def snapshot(path):
    fd = regular_fd(path)
    try:
        before = os.fstat(fd)
        digest = hashlib.sha256()
        while chunk := os.read(fd, 1024 * 1024):
            digest.update(chunk)
        after = os.fstat(fd)
        named = Path(path).lstat()
        if guard(before) != guard(after) or guard(named) != guard(after) or stat.S_ISLNK(named.st_mode):
            raise ProfileError(f'File changed during verification: {path}')
        return {'size': after.st_size, 'sha256': digest.hexdigest()}, guard(after)
    finally:
        os.close(fd)


def content(path):
    return snapshot(path)[0]


def load_json(path):
    fd = regular_fd(path)
    try:
        with os.fdopen(fd, 'r', encoding='utf-8') as stream:
            text = stream.read(262145)
        if len(text) > 262144:
            raise ProfileError('Manifest exceeds size bound')
        value = json.loads(text)
        if not isinstance(value, dict):
            raise ProfileError('Expected a JSON object')
        return value
    except (ValueError, UnicodeError) as exc:
        raise ProfileError('Invalid manifest') from exc


def atomic_json(path, value):
    temporary = path.parent / ('.pm-tmp-' + uuid.uuid4().hex)
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    sync_dir(path.parent)


def directory(path):
    s = path.lstat()
    if not stat.S_ISDIR(s.st_mode) or s.st_uid != os.getuid() or s.st_mode & 0o077:
        raise ProfileError(f'Expected a private user-owned directory: {path}')


def native_clone(source_fd, destination):
    if sys.platform == 'darwin':
        lib = ctypes.CDLL(None, use_errno=True)
        function = getattr(lib, 'fclonefileat', None)
        if function is None:
            raise OSError(errno.ENOSYS, 'fclonefileat unavailable')
        function.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        function.restype = ctypes.c_int
        parent = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            if function(source_fd, parent, os.fsencode(destination.name), 0):
                number = ctypes.get_errno()
                raise OSError(number, os.strerror(number))
        finally:
            os.close(parent)
        return 'apfs-clone'
    if sys.platform.startswith('linux'):
        fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.ioctl(fd, 0x40049409, source_fd)  # Linux UAPI FICLONE
        finally:
            os.close(fd)
        return 'linux-reflink'
    raise OSError(errno.ENOSYS, 'No native file clone API')


def clone_file(source, destination, expected, mode, force_copy=False):
    initial = snapshot(source)
    if initial[0] != expected:
        raise ProfileError('Source identity differs from recorded profile')
    fd = regular_fd(source)
    try:
        if guard(os.fstat(fd)) != initial[1]:
            raise ProfileError('Source changed before cloning')
        method = 'copy'
        if not force_copy:
            try:
                method = native_clone(fd, destination)
            except OSError as exc:
                if exc.errno not in UNSUPPORTED:
                    raise
                if os.path.lexists(destination):
                    s = destination.lstat()
                    if not stat.S_ISREG(s.st_mode) or s.st_nlink != 1 or s.st_uid != os.getuid():
                        raise ProfileError('Unexpected failed-clone destination')
                    destination.unlink()
        if method == 'copy':
            os.lseek(fd, 0, os.SEEK_SET)
            out = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(out, 'wb') as target:
                while chunk := os.read(fd, 1024 * 1024):
                    target.write(chunk)
                target.flush()
                os.fsync(target.fileno())
        if source.stat().st_ino == destination.lstat().st_ino and source.stat().st_dev == destination.lstat().st_dev:
            raise ProfileError('Clone must have an independent inode')
        # Native clones inherit immutable flags. Change only the new owned inode.
        if hasattr(os, 'chflags') and getattr(destination.lstat(), 'st_flags', 0):
            os.chflags(destination, 0, follow_symlinks=False)
        os.chmod(destination, mode, follow_symlinks=False)
        target_fd = regular_fd(destination)
        try:
            os.fsync(target_fd)
        finally:
            os.close(target_fd)
        if snapshot(source) != initial or guard(os.fstat(fd)) != initial[1] or content(destination) != expected:
            raise ProfileError('Clone integrity or source stability check failed')
        return method
    finally:
        os.close(fd)


def ancestors(path):
    return {(p.stat().st_dev, p.stat().st_ino) for p in (path, *path.parents)}


class Store:
    def __init__(self, root):
        self.root = Path(root).expanduser().resolve(strict=True)
        directory(self.root)
        marker = load_json(self.root / 'STORE.json')
        if marker != {'schema': 1, 'format': 'op1emu-profile-store'}:
            raise ProfileError('Unsupported or incomplete store')
        for name in ('checkpoints', 'workspaces', '.pending', '.locks'):
            directory(self.root / name)

    @classmethod
    def create(cls, root):
        supplied = Path(root).expanduser().absolute()
        if os.path.lexists(supplied):
            raise ProfileError('Refuse an existing store destination')
        supplied.mkdir(mode=0o700, parents=True, exist_ok=False)
        root = supplied.resolve(strict=True)
        for name in ('checkpoints', 'workspaces', '.pending', '.locks'):
            (root / name).mkdir(mode=0o700)
        atomic_json(root / 'STORE.json', {'schema': 1, 'format': 'op1emu-profile-store'})
        sync_dir(root.parent)
        return cls(root)

    @contextmanager
    def lock(self, name):
        path = self.root / '.locks' / name
        fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        try:
            s = os.fstat(fd)
            if not stat.S_ISREG(s.st_mode) or s.st_nlink != 1 or s.st_uid != os.getuid():
                raise ProfileError('Invalid lock file')
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise ProfileError('Profile or publication is busy') from exc
            yield fd
        finally:
            os.close(fd)

    def path(self, kind, name):
        if kind not in ('checkpoints', 'workspaces'):
            raise ProfileError('Unknown profile kind')
        path = self.root / kind / checked_name(name)
        directory(path)
        return path

    def clean_temps(self, path):
        for entry in path.iterdir():
            if TEMP.fullmatch(entry.name):
                s = entry.lstat()
                if not stat.S_ISREG(s.st_mode) or s.st_uid != os.getuid() or s.st_nlink != 1:
                    raise ProfileError('Unsafe interrupted manifest temporary file')
                entry.unlink()
        sync_dir(path)

    def read(self, kind, name):
        path = self.path(kind, name)
        m = load_json(path / 'PROFILE.json')
        fields = {'schema', 'kind', 'name', 'compatibility', 'build_id', 'parent', 'workspace_manifest_sha256', 'files', 'copy_methods', 'created_ns', 'state', 'assets_dir', 'run', 'source_run'}
        if set(m) != fields:
            raise ProfileError('Incomplete or unexpected profile manifest fields')
        if (m.get('schema'), m.get('kind'), m.get('name')) != (1, kind, name):
            raise ProfileError('Unsupported/mismatched profile manifest')
        if not isinstance(m.get('compatibility'), str) or not 1 <= len(m['compatibility']) <= 256:
            raise ProfileError('Missing compatibility identity')
        if not isinstance(m.get('build_id'), str) or not 1 <= len(m['build_id']) <= 256:
            raise ProfileError('Missing build identity')
        if m.get('state') not in (('SEALED',) if kind == 'checkpoints' else ('STOPPED', 'RUNNING', 'FAILED')):
            raise ProfileError('Invalid profile state')
        parent = m.get('parent')
        if parent is not None and (not isinstance(parent, str) or not SHA.fullmatch(parent)):
            raise ProfileError('Invalid parent manifest identity')
        if kind == 'workspaces' and parent is None:
            raise ProfileError('Workspace needs a checkpoint parent identity')
        workspace_hash = m['workspace_manifest_sha256']
        if workspace_hash is not None and (kind != 'checkpoints' or not isinstance(workspace_hash, str) or not SHA.fullmatch(workspace_hash)):
            raise ProfileError('Invalid originating workspace manifest identity')
        if type(m['created_ns']) is not int or m['created_ns'] <= 0:
            raise ProfileError('Invalid creation time')
        methods = m['copy_methods']
        if not isinstance(methods, dict) or set(methods) != set(FILES) or any(x not in ('copy', 'apfs-clone', 'linux-reflink') for x in methods.values()):
            raise ProfileError('Invalid copy-method provenance')
        for run in (m['run'], m['source_run']):
            if run is not None:
                if not isinstance(run, dict) or not isinstance(run.get('argv'), list) or not run['argv'] or any(not isinstance(x, str) for x in run['argv']) or not isinstance(run.get('executable_sha256'), str) or not SHA.fullmatch(run['executable_sha256']) or type(run.get('started_ns')) is not int or run.get('guest_filesystem_clean') != 'NOT CLAIMED':
                    raise ProfileError('Invalid run provenance')
        if m['source_run'] is not None and (m['source_run'].get('returncode') != 0 or m['source_run'].get('forced') is not False):
            raise ProfileError('Checkpoint source run was not a successful managed shutdown')
        files = m.get('files')
        if not isinstance(files, dict) or set(files) != set(FILES):
            raise ProfileError('Missing profile data identities')
        for item in files.values():
            if not isinstance(item, dict) or type(item.get('size')) is not int or item['size'] <= 0 or not isinstance(item.get('sha256'), str) or not SHA.fullmatch(item['sha256']):
                raise ProfileError('Malformed data identity')
        return path, m

    def allowlist(self, path, m):
        allowed = set(FILES) | {'PROFILE.json'}
        assets = m.get('assets_dir')
        if assets is not None:
            if m['kind'] != 'workspaces' or not isinstance(assets, str) or not Path(assets).is_absolute():
                raise ProfileError('Invalid GUI asset directory')
            link = path / 'gui'
            if os.path.lexists(link) and (not link.is_symlink() or os.readlink(link) != assets):
                raise ProfileError('GUI asset link differs from recorded directory')
            allowed.add('gui')
        unknown = {p.name for p in path.iterdir()} - allowed
        if unknown:
            raise ProfileError('Unrecognised profile entries: ' + ', '.join(sorted(unknown)))

    def _verify(self, kind, name, compatibility=None):
        path, m = self.read(kind, name)
        self.allowlist(path, m)
        if compatibility is not None and m['compatibility'] != compatibility:
            raise ProfileError('Incompatible profile')
        if kind == 'checkpoints' and (path / 'PROFILE.json').stat().st_mode & 0o222:
            raise ProfileError('Checkpoint manifest is writable')
        for filename in FILES:
            if content(path / filename) != m['files'][filename]:
                raise ProfileError(f'Profile data identity mismatch: {filename}')
            if kind == 'checkpoints' and (path / filename).stat().st_mode & 0o222:
                raise ProfileError('Checkpoint data is writable')
        return path, m

    def verify(self, kind, name, compatibility=None):
        if kind == 'workspaces':
            with self.lock('workspace-' + checked_name(name)):
                self.clean_temps(self.path(kind, name))
                return self._verify(kind, name, compatibility)
        return self._verify(kind, name, compatibility)

    def build(self, kind, name, sources, identities, compatibility, build_id, parent=None, force_copy=False, source_run=None, workspace_manifest_sha256=None):
        checked_name(name)
        if kind not in ('checkpoints', 'workspaces') or set(sources) != set(FILES) or set(identities) != set(FILES):
            raise ProfileError('Invalid construction role or data pair')
        if not isinstance(compatibility, str) or not 1 <= len(compatibility) <= 256 or not isinstance(build_id, str) or not 1 <= len(build_id) <= 256:
            raise ProfileError('Compatibility and build identity are required (max 256 characters)')
        destination = self.root / kind / name
        if os.path.lexists(destination):
            raise ProfileError('Refuse an existing profile destination')
        before = {n: snapshot(p) for n, p in sources.items()}
        if any(before[n][0] != identities[n] for n in FILES):
            raise ProfileError('Source differs from recorded identity')
        stage = self.root / '.pending' / uuid.uuid4().hex
        stage.mkdir(mode=0o700)
        methods = {n: clone_file(sources[n], stage / n, identities[n], 0o400 if kind == 'checkpoints' else 0o600, force_copy) for n in FILES}
        if any(snapshot(sources[n]) != before[n] for n in FILES):
            raise ProfileError('Source pair changed during checkpoint construction')
        m = {'schema': 1, 'kind': kind, 'name': name, 'compatibility': compatibility, 'build_id': build_id,
             'parent': parent, 'workspace_manifest_sha256': workspace_manifest_sha256, 'files': identities, 'copy_methods': methods, 'created_ns': time.time_ns(),
             'state': 'SEALED' if kind == 'checkpoints' else 'STOPPED', 'assets_dir': None, 'run': None, 'source_run': source_run}
        atomic_json(stage / 'PROFILE.json', m)
        if kind == 'checkpoints':
            os.chmod(stage / 'PROFILE.json', 0o400)
        sync_dir(stage)
        with self.lock('publication'):
            if os.path.lexists(destination):
                raise ProfileError('Refuse an existing profile destination')
            os.rename(stage, destination)
            try:
                sync_dir(destination.parent)
            except OSError as exc:
                raise ProfileError(f'Published but directory durability is uncertain; verify without overwriting: {destination}') from exc
        return m

    def import_pair(self, name, nand, otp, compatibility, build_id, offline=False, force_copy=False):
        if not offline:
            raise ProfileError('External import requires --offline: stop all source writers first')
        sources = {}
        for filename, supplied in zip(FILES, (nand, otp)):
            source = Path(supplied).expanduser().absolute()
            if source.is_symlink():
                raise ProfileError('Source file must not be a symlink')
            source = source.resolve(strict=True)
            store_inode = (self.root.stat().st_dev, self.root.stat().st_ino)
            parent_inode = (source.parent.stat().st_dev, source.parent.stat().st_ino)
            if store_inode in ancestors(source.parent) or parent_inode in ancestors(self.root):
                raise ProfileError('Store and source directories must be disjoint')
            sources[filename] = source
        identities = {n: content(p) for n, p in sources.items()}
        return self.build('checkpoints', name, sources, identities, compatibility, build_id, force_copy=force_copy)

    def restore(self, checkpoint, name, compatibility, force_copy=False):
        source, m = self.verify('checkpoints', checkpoint, compatibility)
        return self.build('workspaces', name, {n: source / n for n in FILES}, m['files'], compatibility, m['build_id'], content(source / 'PROFILE.json')['sha256'], force_copy, m['source_run'])

    def checkpoint(self, workspace, name, compatibility, force_copy=False):
        with self.lock('workspace-' + checked_name(workspace)):
            path = self.path('workspaces', workspace)
            self.clean_temps(path)
            path, m = self._verify('workspaces', workspace, compatibility)
            if m['state'] != 'STOPPED':
                raise ProfileError('Incomplete/failed workspace cannot be checkpointed; restore a fresh workspace')
            return self.build('checkpoints', name, {n: path / n for n in FILES}, m['files'], compatibility, m['build_id'], m['parent'], force_copy, m['run'] or m['source_run'], content(path / 'PROFILE.json')['sha256'])

    def run(self, workspace, argv, compatibility, assets=None):
        if not argv or not any('{nand}' in arg for arg in argv):
            raise ProfileError('Command argv must include {nand}')
        if any(arg == '--nand-snapshot' or arg.startswith('--nand-snapshot=') for arg in argv):
            raise ProfileError('Disposable NAND snapshot mode cannot persist a profile checkpoint')
        with self.lock('workspace-' + checked_name(workspace)) as lock_fd:
            path = self.path('workspaces', workspace)
            self.clean_temps(path)
            path, m = self._verify('workspaces', workspace, compatibility)
            if m['state'] != 'STOPPED':
                raise ProfileError('Only STOPPED workspaces may run; incomplete state cannot be cleared by rerunning')
            if assets is not None:
                resolved = Path(assets).expanduser().resolve(strict=True)
                if not resolved.is_dir():
                    raise ProfileError('GUI assets must be a directory')
                if m.get('assets_dir') not in (None, str(resolved)):
                    raise ProfileError('Workspace GUI asset binding is already set')
                if m.get('assets_dir') is None:
                    m['assets_dir'] = str(resolved)
                    atomic_json(path / 'PROFILE.json', m)
            if m.get('assets_dir') is not None:
                if not Path(m['assets_dir']).is_dir():
                    raise ProfileError('GUI assets unavailable for launch; disk checkpointing remains available')
                if not os.path.lexists(path / 'gui'):
                    (path / 'gui').symlink_to(m['assets_dir'], target_is_directory=True)
            command = [arg.replace('{nand}', str(path / FILES[0])).replace('{otp}', str(path / FILES[1])) for arg in argv]
            executable = shutil.which(command[0])
            if executable is None:
                raise ProfileError('Executable not found')
            command[0] = str(Path(executable).resolve(strict=True))
            executable_hash = content(Path(command[0]))['sha256']
            prior_manifest = copy.deepcopy(m)
            requests = []
            child = None
            forced = False
            def stop(signum, _frame):
                nonlocal forced
                requests.append(signum)
                if child is not None and child.poll() is None:
                    forced = len(requests) > 1
                    try:
                        os.killpg(child.pid, signal.SIGKILL if forced else signal.SIGINT)
                    except ProcessLookupError:
                        pass
            previous = {sig: signal.getsignal(sig) for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
            for sig in previous:
                signal.signal(sig, stop)
            m['state'] = 'RUNNING'
            m['run'] = {'argv': command, 'executable_sha256': executable_hash, 'started_ns': time.time_ns(), 'guest_filesystem_clean': 'NOT CLAIMED'}
            try:
                atomic_json(path / 'PROFILE.json', m)
                if requests:
                    raise ProfileError('Stop requested before child launch')
                child = subprocess.Popen(command, cwd=path, stdin=subprocess.DEVNULL, start_new_session=True, pass_fds=(lock_fd,))
                # A signal can arrive during Popen before child is assigned.
                if requests and child.poll() is None:
                    forced = len(requests) > 1
                    os.killpg(child.pid, signal.SIGKILL if forced else signal.SIGINT)
                returncode = child.wait()
                self.allowlist(path, m)
                updated = {}
                for filename in FILES:
                    data_fd = regular_fd(path / filename)
                    try:
                        os.fchmod(data_fd, 0o600)
                        os.fsync(data_fd)
                    finally:
                        os.close(data_fd)
                    updated[filename] = content(path / filename)
                    if updated[filename]['size'] != m['files'][filename]['size']:
                        raise ProfileError('Emulator changed a fixed profile image size')
                m['files'] = updated
                m['state'] = 'STOPPED' if returncode == 0 and not forced else 'FAILED'
                m['run'].update({'returncode': returncode, 'stop_requests': requests, 'forced': forced, 'finished_ns': time.time_ns()})
                atomic_json(path / 'PROFILE.json', m)
                return returncode if m['state'] == 'STOPPED' else (returncode or 1)
            except BaseException:
                if child is not None and child.poll() is None:
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait()
                # No child ever existed: retain prior progress if its bytes and
                # entry set are still verified. Never clear taint after a writer.
                unchanged = False
                if child is None:
                    try:
                        self.allowlist(path, prior_manifest)
                        unchanged = all(content(path / n) == prior_manifest['files'][n] for n in FILES)
                    except (OSError, ProfileError):
                        pass
                if unchanged:
                    m = prior_manifest
                else:
                    m['state'] = 'FAILED'
                try:
                    atomic_json(path / 'PROFILE.json', m)
                except OSError:
                    pass  # The durable prior RUNNING state also fails closed.
                raise
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)

    def listing(self):
        result = []
        for kind in ('checkpoints', 'workspaces'):
            for path in sorted((self.root / kind).iterdir()):
                try:
                    _, m = self.read(kind, path.name)
                    result.append({'kind': kind, 'name': path.name, 'state': m['state'], 'compatibility': m['compatibility'], 'hashes_verified': False})
                except (OSError, ProfileError):
                    result.append({'kind': kind, 'name': path.name, 'state': 'INVALID', 'hashes_verified': False})
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--store', required=True, type=Path)
    sub = parser.add_subparsers(dest='action', required=True)
    sub.add_parser('init')
    sub.add_parser('list')
    imp = sub.add_parser('import')
    imp.add_argument('name'); imp.add_argument('--nand', required=True, type=Path); imp.add_argument('--otp', required=True, type=Path)
    imp.add_argument('--offline', action='store_true'); imp.add_argument('--build-id', required=True)
    for action in ('restore', 'checkpoint'):
        child = sub.add_parser(action); child.add_argument('source'); child.add_argument('name')
    verify = sub.add_parser('verify'); verify.add_argument('kind', choices=('checkpoints', 'workspaces')); verify.add_argument('name')
    run = sub.add_parser('run'); run.add_argument('name'); run.add_argument('--assets', type=Path); run.add_argument('command', nargs=argparse.REMAINDER)
    for action in ('import', 'restore', 'checkpoint'):
        sub.choices[action].add_argument('--copy', action='store_true', help='Use a verified ordinary copy instead of native CoW')
    for action in ('import', 'restore', 'checkpoint', 'run'):
        sub.choices[action].add_argument('--compatibility', required=True)
    args = parser.parse_args()
    try:
        store = Store.create(args.store) if args.action == 'init' else Store(args.store)
        if args.action == 'init': result = {'store': str(store.root), 'schema': 1}
        elif args.action == 'list': result = store.listing()
        elif args.action == 'import': result = store.import_pair(args.name, args.nand, args.otp, args.compatibility, args.build_id, args.offline, args.copy)
        elif args.action == 'restore': result = store.restore(args.source, args.name, args.compatibility, args.copy)
        elif args.action == 'checkpoint': result = store.checkpoint(args.source, args.name, args.compatibility, args.copy)
        elif args.action == 'verify': result = store.verify(args.kind, args.name)[1]
        else:
            command = args.command[1:] if args.command[:1] == ['--'] else args.command
            return store.run(args.name, command, args.compatibility, args.assets)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (ProfileError, OSError, ValueError) as exc:
        print('Profile manager: ' + str(exc), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
