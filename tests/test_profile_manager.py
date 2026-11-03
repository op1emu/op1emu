"""Owned synthetic disk data only; real files/processes exercise the public workflow."""
import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

TOOL = Path(__file__).resolve().parent / 'profile_manager.py'
if not TOOL.exists():
    TOOL = Path(__file__).resolve().parents[1] / 'tools/profile_manager.py'
spec = importlib.util.spec_from_file_location('profile_manager', TOOL)
pm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pm)


class Profiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='op1-profile-fixture-')
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / 'source'; self.source.mkdir(mode=0o700)
        # Independent main/OOB/erased-byte oracle, not a proprietary image.
        self.nand_bytes = (bytes(range(256)) * 8 + b'owned-oob' * 8) * 8 + b'\xff' * 4096
        self.otp_bytes = b'owned-otp-fixture' * 64
        (self.source / 'nand').write_bytes(self.nand_bytes)
        (self.source / 'otp').write_bytes(self.otp_bytes)
        os.chmod(self.source / 'nand', 0o400); os.chmod(self.source / 'otp', 0o400)
        self.store = pm.Store.create(self.root / 'store')

    def tearDown(self):
        self.temp.cleanup()

    def base(self, force_copy=False):
        return self.store.import_pair('base', self.source / 'nand', self.source / 'otp', 'owned-v1', 'fixture-build', True, force_copy)

    def workspace(self):
        self.base(); self.store.restore('base', 'work', 'owned-v1')
        return self.store.root / 'workspaces/work'

    def writer(self, code):
        return [sys.executable, '-c', code, '{nand}', '{otp}']

    def cli(self, *args, **kwargs):
        return subprocess.Popen([sys.executable, str(TOOL), '--store', str(self.store.root), *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)

    def wait_for(self, predicate, child, seconds=10):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate(): return
            if child.poll() is not None:
                out, err = child.communicate(); self.fail(f'Child exited early: {out!r} {err!r}')
            time.sleep(0.02)
        self.fail('Timed out waiting for synthetic child')

    def test_complete_persistent_workflow_isolation_and_erase_oob(self):
        self.workspace()
        code="from pathlib import Path; import sys; p=Path(sys.argv[1]); a=bytearray(p.read_bytes()); a[2048:2112]=b'changed-oob'*5+b'123456789'; a[-4096:]=b'\\xff'*4096; p.write_bytes(a); q=Path(sys.argv[2]); b=bytearray(q.read_bytes()); b[0]=42; q.write_bytes(b)"
        self.assertEqual(self.store.run('work', self.writer(code), 'owned-v1'), 0)
        self.store.checkpoint('work', 'saved', 'owned-v1')
        self.store.restore('saved', 'restored', 'owned-v1')
        saved = self.store.root / 'checkpoints/saved'
        restored = self.store.root / 'workspaces/restored'
        self.assertEqual((saved / 'nandflash.img').read_bytes(), (restored / 'nandflash.img').read_bytes())
        self.assertEqual((restored / 'otp.bin').read_bytes()[0], 42)
        self.assertEqual((restored / 'nandflash.img').read_bytes()[-4096:], b'\xff' * 4096)
        self.assertEqual((self.source / 'nand').read_bytes(), self.nand_bytes)
        self.assertEqual((self.source / 'otp').read_bytes(), self.otp_bytes)
        self.assertEqual((self.store.root / 'checkpoints/base/nandflash.img').read_bytes(), self.nand_bytes)
        saved_manifest=self.store.verify('checkpoints','saved')[1]
        self.assertEqual(saved_manifest['source_run']['returncode'],0)
        self.assertEqual(saved_manifest['source_run']['argv'][0],str(Path(sys.executable).resolve()))

    def test_native_cow_or_explicit_supported_fallback_has_distinct_inodes(self):
        m = self.base()
        self.assertIn(m['copy_methods']['nandflash.img'], ('apfs-clone', 'linux-reflink', 'copy'))
        if sys.platform == 'darwin': self.assertEqual(m['copy_methods']['nandflash.img'], 'apfs-clone')
        self.store.restore('base', 'work', 'owned-v1')
        a = self.store.root / 'checkpoints/base/nandflash.img'
        b = self.store.root / 'workspaces/work/nandflash.img'
        self.assertNotEqual((a.stat().st_dev, a.stat().st_ino), (b.stat().st_dev, b.stat().st_ino))
        with b.open('r+b') as stream: stream.write(b'X')
        self.assertEqual(a.read_bytes(), self.nand_bytes)

    def test_copy_fallback(self):
        m = self.base(True)
        self.assertEqual(set(m['copy_methods'].values()), {'copy'})
        with patch.object(pm, 'native_clone', side_effect=OSError(errno.EXDEV, 'cross volume')):
            m = self.store.restore('base', 'cross-volume', 'owned-v1')
        self.assertEqual(set(m['copy_methods'].values()), {'copy'})

    def test_clone_io_and_permission_errors_do_not_fallback_or_publish(self):
        for number in (errno.EACCES, errno.ENOSPC, errno.EIO):
            with self.subTest(number=number), patch.object(pm, 'native_clone', side_effect=OSError(number, 'injected')):
                with self.assertRaises(OSError): self.base()
            self.assertFalse((self.store.root / 'checkpoints/base').exists())
        self.assertEqual(self.store.listing(), [])

    def test_partial_failed_clone_is_recreated_for_fallback(self):
        def partial(_fd, dst):
            dst.write_bytes(b'partial'); raise OSError(errno.ENOTSUP, 'unsupported')
        with patch.object(pm, 'native_clone', side_effect=partial): m = self.base()
        self.assertEqual(set(m['copy_methods'].values()), {'copy'})
        self.assertEqual((self.store.root / 'checkpoints/base/nandflash.img').read_bytes(), self.nand_bytes)

    def test_immutable_source_flags_never_changed(self):
        if not hasattr(os, 'chflags'): self.skipTest('Native immutable flags require macOS')
        source = self.source / 'nand'
        os.chflags(source, stat.UF_IMMUTABLE)
        try:
            self.base(); self.store.restore('base', 'work', 'owned-v1')
            self.assertTrue(source.stat().st_flags & stat.UF_IMMUTABLE)
            self.assertEqual(source.read_bytes(), self.nand_bytes)
            self.assertFalse((self.store.root / 'workspaces/work/nandflash.img').stat().st_flags & stat.UF_IMMUTABLE)
        finally: os.chflags(source, 0)

    def test_existing_and_dangling_destinations_refused(self):
        self.base()
        with self.assertRaises(pm.ProfileError): self.base()
        dangling = self.store.root / 'workspaces/work'; dangling.symlink_to(self.root / 'missing')
        with self.assertRaises(pm.ProfileError): self.store.restore('base', 'work', 'owned-v1')

    def test_bad_names_and_compatibility(self):
        self.base()
        for name in ('../outside', '.hidden', 'Mixed', 'a/b', 'a'*65, 'é'):
            with self.subTest(name=name), self.assertRaises(pm.ProfileError): self.store.restore('base', name, 'owned-v1')
        with self.assertRaises(pm.ProfileError): self.store.restore('base', 'work', 'other-layout')

    def test_external_import_requires_offline_and_disjoint_directories(self):
        with self.assertRaises(pm.ProfileError): self.store.import_pair('base', self.source / 'nand', self.source / 'otp', 'owned-v1', 'fixture')
        nested = pm.Store.create(self.source / 'nested-store')
        with self.assertRaises(pm.ProfileError): nested.import_pair('base', self.source / 'nand', self.source / 'otp', 'owned-v1', 'fixture', True)

    def test_symlinks_hardlinks_and_fifos_are_refused(self):
        linked = self.source / 'linked'; linked.symlink_to(self.source / 'nand')
        with self.assertRaises(pm.ProfileError): self.store.import_pair('base', linked, self.source / 'otp', 'owned-v1', 'fixture', True)
        hard = self.source / 'hard'; os.link(self.source / 'nand', hard)
        with self.assertRaises(pm.ProfileError): self.base()
        hard.unlink()
        fifo = self.source / 'fifo'; os.mkfifo(fifo)
        with self.assertRaises(pm.ProfileError): self.store.import_pair('base', fifo, self.source / 'otp', 'owned-v1', 'fixture', True)

    def test_corruption_bad_manifest_and_writable_checkpoint_fail(self):
        self.base()
        p = self.store.root / 'checkpoints/base/nandflash.img'; os.chmod(p, 0o600)
        with self.assertRaises(pm.ProfileError): self.store.restore('base', 'work', 'owned-v1')
        with p.open('r+b') as stream: stream.write(b'corrupt')
        os.chmod(p, 0o400)
        with self.assertRaises(pm.ProfileError): self.store.restore('base', 'work', 'owned-v1')
        profile = self.store.root / 'checkpoints/base/PROFILE.json'; os.chmod(profile, 0o600); profile.write_text('{}')
        with self.assertRaises(pm.ProfileError): self.store.verify('checkpoints', 'base')

    def test_unsupported_store_marker(self):
        pm.atomic_json(self.store.root / 'STORE.json', {'schema': 2, 'format': 'op1emu-profile-store'})
        with self.assertRaises(pm.ProfileError): pm.Store(self.store.root)

    def test_out_of_band_data_change_refuses_run_and_checkpoint(self):
        p = self.workspace() / 'otp.bin'
        with p.open('r+b') as stream: stream.write(b'X')
        with self.assertRaises(pm.ProfileError): self.store.run('work', self.writer('pass'), 'owned-v1')
        with self.assertRaises(pm.ProfileError): self.store.checkpoint('work', 'bad', 'owned-v1')

    def test_stray_entry_preflight_does_not_taint(self):
        p = self.workspace(); (p / '.DS_Store').write_bytes(b'owned synthetic stray')
        with self.assertRaises(pm.ProfileError): self.store.run('work', self.writer('pass'), 'owned-v1')
        self.assertEqual(self.store.read('workspaces', 'work')[1]['state'], 'STOPPED')

    def test_reserved_manifest_temps_only_cleaned_under_lock(self):
        p = self.workspace(); reserved=p / ('.pm-tmp-'+'a'*32); reserved.write_text('partial manifest')
        self.assertEqual(self.store.run('work', self.writer('pass'), 'owned-v1'), 0)
        self.assertFalse(reserved.exists())
        target = p / ('.pm-tmp-'+'b'*32); target.symlink_to(self.source / 'otp')
        with self.assertRaises(pm.ProfileError): self.store.run('work', self.writer('pass'), 'owned-v1')
        self.assertEqual((self.source / 'otp').read_bytes(), self.otp_bytes)

    def test_failed_run_and_stale_running_cannot_be_laundered(self):
        p = self.workspace()
        self.assertEqual(self.store.run('work', self.writer('import sys; sys.exit(3)'), 'owned-v1'), 3)
        with self.assertRaises(pm.ProfileError): self.store.run('work', self.writer('pass'), 'owned-v1')
        with self.assertRaises(pm.ProfileError): self.store.checkpoint('work', 'bad', 'owned-v1')
        m = self.store.read('workspaces', 'work')[1]; m['state']='RUNNING'; pm.atomic_json(p/'PROFILE.json', m)
        with self.assertRaises(pm.ProfileError): self.store.run('work', self.writer('pass'), 'owned-v1')

    def test_unknown_generated_state_or_size_change_taints_run(self):
        self.workspace()
        with self.assertRaises(pm.ProfileError): self.store.run('work', self.writer("from pathlib import Path; Path('extra-state').write_text('owned')"), 'owned-v1')
        self.assertEqual(self.store.read('workspaces', 'work')[1]['state'], 'FAILED')
        self.store.restore('base', 'other', 'owned-v1')
        with self.assertRaises(pm.ProfileError): self.store.run('other', self.writer("import sys; open(sys.argv[1],'ab').write(b'x')"), 'owned-v1')
        self.assertEqual(self.store.read('workspaces', 'other')[1]['state'], 'FAILED')

    def test_replaced_symlink_data_fails(self):
        self.workspace()
        code="from pathlib import Path; import sys; p=Path(sys.argv[2]); p.unlink(); p.symlink_to(sys.argv[1])"
        with self.assertRaises(OSError): self.store.run('work', self.writer(code), 'owned-v1')
        self.assertEqual(self.store.read('workspaces', 'work')[1]['state'], 'FAILED')

    def test_snapshot_mode_forms_refused(self):
        self.workspace()
        for flag in ('--nand-snapshot', '--nand-snapshot=true'):
            with self.subTest(flag=flag), self.assertRaises(pm.ProfileError): self.store.run('work', self.writer('pass')+[flag], 'owned-v1')

    def test_atomic_state_interruption_and_staging_never_publish(self):
        with patch.object(pm, 'atomic_json', side_effect=OSError(errno.EIO, 'injected manifest failure')):
            with self.assertRaises(OSError): self.base()
        self.assertEqual(self.store.listing(), [])
        self.assertFalse((self.store.root/'checkpoints/base').exists())
        self.base()
        with patch.object(pm.os, 'rename', side_effect=OSError(errno.EIO, 'injected restore publication failure')):
            with self.assertRaises(OSError): self.store.restore('base','work','owned-v1')
        self.assertFalse((self.store.root/'workspaces/work').exists())

    def test_state_replace_failure_preserves_previous_json(self):
        p = self.workspace(); before = (p/'PROFILE.json').read_bytes()
        with patch.object(pm.os, 'replace', side_effect=OSError(errno.EIO, 'interrupted state replace')):
            with self.assertRaises(OSError): pm.atomic_json(p/'PROFILE.json', {'state':'RUNNING'})
        self.assertEqual((p/'PROFILE.json').read_bytes(),before)
        self.assertEqual(self.store.run('work',self.writer('pass'),'owned-v1'),0)

    def test_post_publication_sync_error_is_reported_and_name_preserved(self):
        real = pm.sync_dir
        def fail_parent(path):
            if path == self.store.root/'checkpoints': raise OSError(errno.EIO,'injected parent sync')
            return real(path)
        with patch.object(pm,'sync_dir',side_effect=fail_parent):
            with self.assertRaisesRegex(pm.ProfileError,'durability is uncertain'): self.base()
        self.store.verify('checkpoints','base')
        with self.assertRaises(pm.ProfileError): self.base()

    def test_publication_lock_competing_process_refuses(self):
        self.base()
        with self.store.lock('publication'):
            child = self.cli('restore','base','work','--compatibility','owned-v1')
            _,err=child.communicate(timeout=10)
            self.assertEqual(child.returncode,2); self.assertIn(b'busy',err)
        self.assertFalse((self.store.root/'workspaces/work').exists())

    def test_cli_normal_ctrl_c_flushes_and_checkpoint_succeeds(self):
        self.workspace(); ready=self.root/'ready'
        code="""import signal,time,sys,os
from pathlib import Path
def stop(*_):
 with open(sys.argv[1],'r+b') as f: f.write(b'Z'); f.flush(); os.fsync(f.fileno())
 sys.exit(0)
signal.signal(signal.SIGINT,stop)
Path(sys.argv[3]).write_text('ready')
while True: time.sleep(.05)
"""
        child=self.cli('run','--compatibility','owned-v1','work','--',*self.writer(code),str(ready),start_new_session=True)
        try:
            self.wait_for(ready.exists,child)
            os.killpg(child.pid,signal.SIGINT)
            out,err=child.communicate(timeout=10)
            self.assertEqual(child.returncode,0,(out,err))
            m=self.store.read('workspaces','work')[1]
            self.assertEqual(m['state'],'STOPPED'); self.assertFalse(m['run']['forced'])
            self.assertEqual(m['run']['guest_filesystem_clean'],'NOT CLAIMED')
            self.store.checkpoint('work','interrupted-cleanly','owned-v1')
            self.assertEqual((self.store.root/'checkpoints/interrupted-cleanly/nandflash.img').read_bytes()[0],ord('Z'))
        finally:
            if child.poll() is None: child.kill(); child.wait()

    def test_manager_death_leaves_child_lock_and_sticky_running(self):
        self.workspace(); ready=self.root/'ready'; release=self.root/'release'
        code="""import os,time,sys
from pathlib import Path
Path(sys.argv[3]).write_text(str(os.getpid()))
while not Path(sys.argv[4]).exists(): time.sleep(.02)
"""
        child=self.cli('run','--compatibility','owned-v1','work','--',*self.writer(code),str(ready),str(release),start_new_session=True)
        pid=None
        try:
            self.wait_for(ready.exists,child); pid=int(ready.read_text())
            child.kill(); child.wait(timeout=10)
            with self.assertRaisesRegex(pm.ProfileError,'busy'): self.store.checkpoint('work','bad','owned-v1')
            with self.assertRaisesRegex(pm.ProfileError,'busy'): self.store.run('work',self.writer('pass'),'owned-v1')
            release.write_text('exit')
            deadline=time.monotonic()+10
            while True:
                try:
                    with self.store.lock('workspace-work'): break
                except pm.ProfileError:
                    if time.monotonic()>deadline: self.fail('Orphan did not release inherited lock')
                    time.sleep(.02)
            with self.assertRaisesRegex(pm.ProfileError,'Only STOPPED'): self.store.run('work',self.writer('pass'),'owned-v1')
        finally:
            release.touch()
            if pid:
                try: os.kill(pid,signal.SIGKILL)
                except ProcessLookupError: pass
            if child.poll() is None: child.kill(); child.wait()
            child.stdout.close(); child.stderr.close()

    def test_gui_asset_binding_not_copied_as_private_checkpoint_state(self):
        self.workspace(); assets=self.root/'public-gui'; assets.mkdir(); (assets/'ui.json').write_text('{}')
        self.assertEqual(self.store.run('work',self.writer("from pathlib import Path; assert Path('gui/ui.json').exists()"),'owned-v1',assets),0)
        self.store.checkpoint('work','saved','owned-v1')
        self.assertEqual({p.name for p in (self.store.root/'checkpoints/saved').iterdir()},set(pm.FILES)|{'PROFILE.json'})

    def test_missing_gui_assets_do_not_prevent_checkpoint(self):
        self.workspace(); assets=self.root/'public-gui'; assets.mkdir()
        self.assertEqual(self.store.run('work',self.writer('pass'),'owned-v1',assets),0)
        assets.rmdir()
        self.store.checkpoint('work','saved','owned-v1')
        with self.assertRaisesRegex(pm.ProfileError,'unavailable for launch'):
            self.store.run('work',self.writer('pass'),'owned-v1')
        self.assertEqual(self.store.read('workspaces','work')[1]['state'],'STOPPED')

    def test_gui_assets_removed_during_run_preserve_success(self):
        self.workspace(); assets=self.root/'public-gui'; assets.mkdir()
        command=self.writer("import shutil,sys; shutil.rmtree(sys.argv[3])")+[str(assets)]
        self.assertEqual(self.store.run('work',command,'owned-v1',assets),0)
        self.assertEqual(self.store.read('workspaces','work')[1]['state'],'STOPPED')
        self.store.checkpoint('work','saved','owned-v1')

    def test_failed_exec_preserves_prior_successful_unsaved_run(self):
        self.workspace()
        command=self.writer("import sys; f=open(sys.argv[2],'r+b'); f.write(b'X'); f.close()")
        self.assertEqual(self.store.run('work',command,'owned-v1'),0)
        before=self.store.read('workspaces','work')[1]
        with patch.object(pm.subprocess,'Popen',side_effect=OSError(errno.ENOEXEC,'invalid executable')):
            with self.assertRaises(OSError): self.store.run('work',self.writer('pass'),'owned-v1')
        after=self.store.read('workspaces','work')[1]
        self.assertEqual(after,before)
        self.store.checkpoint('work','saved','owned-v1')
        self.assertEqual((self.store.root/'checkpoints/saved/otp.bin').read_bytes()[0],ord('X'))

    def test_checkpoint_parent_remains_resolvable_after_later_workspace_run(self):
        self.workspace()
        base_hash=pm.content(self.store.root/'checkpoints/base/PROFILE.json')['sha256']
        self.assertEqual(self.store.run('work',self.writer('pass'),'owned-v1'),0)
        workspace_hash=pm.content(self.store.root/'workspaces/work/PROFILE.json')['sha256']
        m=self.store.checkpoint('work','saved','owned-v1')
        self.assertEqual(m['parent'],base_hash)
        self.assertEqual(m['workspace_manifest_sha256'],workspace_hash)
        self.assertEqual(self.store.run('work',self.writer('pass'),'owned-v1'),0)
        self.assertEqual(pm.content(self.store.root/'checkpoints/base/PROFILE.json')['sha256'],m['parent'])

    def test_interrupted_asset_manifest_binding_preserves_checkpointability(self):
        self.workspace(); assets=self.root/'public-gui'; assets.mkdir()
        with patch.object(pm,'atomic_json',side_effect=OSError(errno.EIO,'binding failed')):
            with self.assertRaises(OSError): self.store.run('work',self.writer('pass'),'owned-v1',assets)
        self.assertFalse(os.path.lexists(self.store.root/'workspaces/work/gui'))
        self.store.checkpoint('work','saved','owned-v1')


if __name__ == '__main__': unittest.main()
