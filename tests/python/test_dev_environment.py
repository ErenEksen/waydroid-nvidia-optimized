"""Keep build Python out of runtime commands; never touch real services or sudo."""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'dev'))
from perf.bundle import ARTIFACTS, sha256


class DevEnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nvwd-env-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.work = self.root / 'build work'
        self.build_bin = self.work / 'venv/bin'
        self.build_bin.mkdir(parents=True)
        self.write_script(self.build_bin / 'meson', 'exit 0\n')
        self.write_script(self.build_bin / 'python3',
                          'echo "build Python leaked into runtime" >&2\nexit 90\n')
        self.env = os.environ.copy()
        for key in ('REPO', 'WNV', 'WAYDROID_SRC', 'WAYDROID_BIN', 'BASH_ENV',
                    'PYTHONHOME', 'PYTHONPATH', 'VIRTUAL_ENV'):
            self.env.pop(key, None)
        self.env.update(PATH='/usr/bin:/bin', WNV=str(self.work))
        self.waydroid = self.root / 'waydroid'
        self.waydroid.write_text('#!/usr/bin/env python3\nprint("runtime-python-ok")\n')
        self.waydroid.chmod(0o755)
        self.env['WAYDROID_BIN'] = str(self.waydroid)

    @staticmethod
    def write_script(path, body):
        path.write_text('#!/bin/bash\n' + body)
        path.chmod(0o755)

    def run_script(self, script, *args):
        return subprocess.run(['/bin/bash', str(REPO / 'dev' / script), *args],
                              env=self.env, capture_output=True, text=True, timeout=15)

    def test_shared_environment_preserves_runtime_path(self):
        result = subprocess.run(
            ['/bin/bash', '-c', 'source "$1"; printf "%s" "$PATH"',
             'test', str(REPO / 'dev/env.sh')],
            env=self.env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, self.env['PATH'])

    def test_packaged_waydroid_does_not_use_build_python(self):
        result = self.run_script('wdu', '--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'runtime-python-ok')

    def test_opt_in_checkout_does_not_use_build_python(self):
        checkout = self.root / 'runtime checkout'
        checkout.mkdir()
        shutil.copyfile(self.waydroid, checkout / 'waydroid.py')
        self.env['WAYDROID_SRC'] = str(checkout)
        result = self.run_script('wdu', '--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'runtime-python-ok')

    def test_build_still_selects_pinned_tools(self):
        # A fake build recipe records tool resolution without compiling anything.
        repo = self.root / 'fake repo'
        (repo / 'packaging/ci').mkdir(parents=True)
        for name in ('pins.env', 'hwc-pins.env'):
            shutil.copyfile(REPO / 'packaging/ci' / name, repo / 'packaging/ci' / name)
        recipe = repo / 'build/virglrenderer/build.sh'
        recipe.parent.mkdir(parents=True)
        self.write_script(recipe, 'command -v meson\ncommand -v python3\n')
        self.env['REPO'] = str(repo)
        result = self.run_script('build', 'virgl')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines()[-2:],
                         [str(self.build_bin / 'meson'), str(self.build_bin / 'python3')])

    def prepare_installer(self):
        self.events = self.root / 'events'
        self.env['TEST_EVENTS'] = str(self.events)
        mock_bin = self.root / 'mock-bin'
        mock_bin.mkdir()
        for name in ('sudo', 'systemctl'):
            self.write_script(mock_bin / name,
                              f'echo "{name} $*" >> "$TEST_EVENTS"\nexit 97\n')
        self.env['PATH'] = str(mock_bin) + ':' + self.env['PATH']
        self.write_script(self.waydroid,
                          'echo "waydroid $*" >> "$TEST_EVENTS"\n'
                          'echo "ModuleNotFoundError: No module named dbus" >&2\nexit 1\n')
        bundle = self.root / 'bundle'
        bundle.mkdir()
        manifest = {'schema_version': 1, 'status': 'unit-test-only', 'files': {}}
        for role, (_, abi, machine, _) in ARTIFACTS.items():
            blob = bytearray(80)
            if abi:
                blob[:6] = b'\x7fELF' + bytes([abi, 1])
                struct.pack_into('<H', blob, 18, machine)
            (bundle / role).write_bytes(blob)
            manifest['files'][role] = {'sha256': sha256(blob)}
        (bundle / 'manifest.json').write_text(json.dumps(manifest))
        return bundle

    @unittest.skipIf(os.geteuid() == 0, 'user-level coordinator intentionally refuses root')
    def test_failed_runtime_preflight_precedes_sudo_and_service_control(self):
        bundle = self.prepare_installer()
        for mode in ('--apply', '--rollback'):
            with self.subTest(mode=mode):
                self.events.write_text('')
                result = self.run_script('install-bundle', mode, str(bundle))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Waydroid CLI preflight failed', result.stderr)
                self.assertEqual(self.events.read_text().splitlines(), ['waydroid --help'])

    @unittest.skipIf(os.geteuid() == 0, 'user-level coordinator intentionally refuses root')
    def test_successful_preflight_reaches_authentication_without_stopping_services(self):
        bundle = self.prepare_installer()
        self.write_script(self.waydroid,
                          'echo "waydroid $*" >> "$TEST_EVENTS"\nexit 0\n')
        result = self.run_script('install-bundle', '--apply', str(bundle))
        self.assertNotEqual(result.returncode, 0)  # Mock sudo intentionally refuses.
        self.assertEqual(self.events.read_text().splitlines(),
                         ['waydroid --help', 'sudo -n lxc-info --version'])

    def test_check_remains_offline_without_working_runtime(self):
        bundle = self.prepare_installer()
        result = self.run_script('install-bundle', '--check', str(bundle))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('Validated complete checksummed set', result.stdout)
        self.assertFalse(self.events.exists())
