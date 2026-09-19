import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'dev'))
from perf.bundle import ARTIFACTS, backup_set, destinations, install_set, load_bundle, restore_set, sha256


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.bundle = self.root/'bundle'
        self.bundle.mkdir()
        manifest = {'schema_version': 1, 'files': {}}
        for role, (_, abi, machine, _) in ARTIFACTS.items():
            blob = bytearray(80)
            if abi:
                blob[:6] = b'\x7fELF'+bytes([abi, 1])
                struct.pack_into('<H', blob, 18, machine)
            else:
                blob = b'[Service]\n'
            (self.bundle/role).write_bytes(blob)
            manifest['files'][role] = {'sha256': sha256(blob)}
        (self.bundle/'manifest.json').write_text(json.dumps(manifest))
        self.targets = destinations(self.root/'home', self.root/'system')
        for role, target in self.targets.items():
            if role.endswith('.conf'):
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text('old '+role)
            target.chmod(0o640)

    def tearDown(self):
        self.tmp.cleanup()

    def test_complete_set_and_restore(self):
        _, data = load_bundle(self.bundle)
        backup = self.root/'backup'
        backup_set(backup, self.targets)
        uid, gid = os.getuid(), os.getgid()
        install_set(data, self.targets, uid, gid, uid, gid)
        self.assertTrue(self.targets['50-renderer-cache.conf'].exists())
        restore_set(backup, self.targets)
        self.assertFalse(self.targets['50-renderer-cache.conf'].exists())
        for role, target in self.targets.items():
            if not role.endswith('.conf'):
                self.assertEqual(target.read_text(), 'old '+role)
                self.assertEqual(target.stat().st_mode & 0o777, 0o640)

    def test_corruption_refused(self):
        (self.bundle/'vulkan-x86.so').write_text('corrupt')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            load_bundle(self.bundle)

    def test_wrong_abi_even_with_valid_checksum(self):
        manifest = json.loads((self.bundle/'manifest.json').read_text())
        data = (self.bundle/'vulkan-x86_64.so').read_bytes()
        (self.bundle/'vulkan-x86.so').write_bytes(data)
        manifest['files']['vulkan-x86.so']['sha256'] = sha256(data)
        (self.bundle/'manifest.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'ABI'):
            load_bundle(self.bundle)

    def test_incomplete_set_refused(self):
        manifest = json.loads((self.bundle/'manifest.json').read_text())
        del manifest['files']['vulkan-x86.so']
        (self.bundle/'manifest.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'set mismatch'):
            load_bundle(self.bundle)

    def test_symlink_target_refused_before_write(self):
        target = self.targets['virgl_test_server']
        target.unlink()
        target.symlink_to(self.bundle/'virgl_test_server')
        with self.assertRaisesRegex(ValueError, 'symlink'):
            backup_set(self.root/'backup', self.targets)

    def test_corrupt_backup_does_not_partially_restore(self):
        backup = self.root/'backup'
        backup_set(backup, self.targets)
        self.targets['virgl_test_server'].write_text('candidate')
        (backup/'vulkan-x86.so').write_text('corrupt')
        with self.assertRaisesRegex(ValueError, 'checksum'):
            restore_set(backup, self.targets)
        self.assertEqual(self.targets['virgl_test_server'].read_text(), 'candidate')
