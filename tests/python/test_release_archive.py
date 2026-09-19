import importlib.util
import io
import json
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release_extract', ROOT/'packaging/releases/extract-bundle.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleaseArchiveTests(unittest.TestCase):
    def archive(self, path, bad=None):
        blobs = {}
        manifest = {'schema_version': 1, 'files': {}}
        for role, (_, abi, machine, _) in module.ARTIFACTS.items():
            blob = bytearray(80)
            if abi:
                blob[:6] = b'\x7fELF' + bytes([abi, 1])
                struct.pack_into('<H', blob, 18, machine)
            blobs[role] = bytes(blob)
            import hashlib
            manifest['files'][role] = {'sha256': hashlib.sha256(blob).hexdigest()}
        blobs['manifest.json'] = json.dumps(manifest).encode()
        with tarfile.open(path, 'w:gz') as tar:
            for name, blob in blobs.items():
                info = tarfile.TarInfo(name)
                info.size = len(blob)
                if bad == 'link' and name == 'virgl_test_server':
                    info.type = tarfile.SYMTYPE
                    info.linkname = '/etc/passwd'
                    info.size = 0
                if bad == 'traversal' and name == 'virgl_test_server':
                    info.name = '../escape'
                if bad == 'checksum' and name == 'virgl_test_server':
                    blob = b'x' * len(blob)
                tar.addfile(info, io.BytesIO(blob))
            if bad == 'duplicate':
                tar.addfile(tarfile.TarInfo('manifest.json'), io.BytesIO())

    def test_valid(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            self.archive(d/'a.tar.gz')
            module.extract(d/'a.tar.gz', d/'out')
            module.load_bundle(d/'out')

    def test_reject_unsafe_or_corrupt(self):
        for bad in ('link', 'traversal', 'checksum', 'duplicate'):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as d:
                d = Path(d)
                self.archive(d/'a.tar.gz', bad)
                with self.assertRaises(ValueError):
                    module.extract(d/'a.tar.gz', d/'out')
                self.assertFalse((d/'escape').exists())
