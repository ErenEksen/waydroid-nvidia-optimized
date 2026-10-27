#!/usr/bin/env python3
# Unit tests for dev/driver-mode's pure parts (no root, no container, no GPU).
#   python3 -m pytest -q dev/test_driver_mode.py
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
loader = importlib.machinery.SourceFileLoader('driver_mode_under_test', str(HERE / 'driver-mode'))
spec = importlib.util.spec_from_loader(loader.name, loader)
dm = importlib.util.module_from_spec(spec)
loader.exec_module(dm)
RENDER = '/dev/dri/renderD129'


class SwitchedTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        top = Path(os.path.realpath(self.tmp.name))
        self.source = top / 'source'
        self.source.mkdir()
        (self.source / 'source-rm.json').write_text('{}')
        self.blob = top / 'blob'
        self.blob.mkdir()
        self.links = {'nvidia-prolapse': str(top / 'nvidia-prolapse'), 'glibc-shim': str(top / 'nvidia-glibc-shim')}
        os.symlink(self.source, self.links['nvidia-prolapse'])
        os.symlink(self.blob, self.links['glibc-shim'])
        self.cfg = top / 'waydroid.cfg'
        for p in (mock.patch.object(dm.native, 'nvidia_render_node', lambda *a: RENDER),
                  mock.patch.object(dm.native, 'ROOT_LINKS', self.links)):
            p.start()
            self.addCleanup(p.stop)

    def write(self, waydroid, properties):
        self.cfg.write_text('[waydroid]\n' + ''.join(f'{k} = {v}\n' for k, v in waydroid.items()) +
                            '\n[properties]\n' + ''.join(f'{k} = {v}\n' for k, v in properties.items()))
        return dm.read_cfg(self.cfg)

    def test_live_cfg_with_path_keys_is_stripped(self):
        said = []
        cp = self.write({'drm_device': RENDER, 'nvidia_guest_layout': '2', 'nvidia_driver_mode': 'nvidia-prolapse',
                         'nvidia_rust_driver_root': self.source, 'nvidia_glibc_shim_root': self.blob,
                         'nvidia_venus_socket': '/run/waydroid-venus/venus.sock', 'nvidia_native_gl': 'off'},
                        {'ro.hardware.egl': 'angle', 'debug.hwui.renderer': 'skiavk'})
        new = dm.switched(cp, 'nvidia-prolapse', None, say=said.append)
        w = new['waydroid']
        self.assertEqual(dict(w), {'drm_device': RENDER, 'nvidia_guest_layout': '2', 'nvidia_prolapse_install': 'dev-root',
                                   'nvidia_driver_mode': 'nvidia-prolapse', 'nvidia_native_gl': 'off'})
        self.assertTrue(any('predates the install shapes' in line for line in said))
        self.assertEqual(dict(new['properties']), {'debug.hwui.renderer': 'skiavk'})
        for key in ('nvidia_rust_driver_root', 'nvidia_glibc_shim_root', 'nvidia_venus_socket', 'ro.hardware.egl'):
            self.assertTrue(any(key in line for line in said), key)
        self.assertTrue(any(f'{self.links["nvidia-prolapse"]} -> {self.source}' in line for line in said))
        self.assertFalse([line for line in said if line.startswith('NOTE')])   # links already point there
        self.assertIn('nvidia_rust_driver_root', cp['waydroid'])                # the input is not modified
        dm.native.driver_mode(w)                                                # and the result is valid

    def test_key_naming_another_root_says_how_to_keep_it(self):
        other = self.source.parent / 'other-source'
        other.mkdir()
        said = []
        cp = self.write({'nvidia_native_root': other}, {})
        new = dm.switched(cp, 'glibc-shim', 'on', say=said.append)
        self.assertNotIn('nvidia_native_root', new['waydroid'])
        self.assertEqual(new['waydroid']['nvidia_native_gl'], 'on')
        self.assertTrue(any(line.startswith('NOTE') and f'driver-mode link glibc-shim {other}' in line for line in said))

    def test_gl_kept_unless_given(self):
        cp = self.write({'nvidia_driver_mode': 'glibc-shim', 'nvidia_native_gl': 'on'}, {})
        self.assertEqual(dm.switched(cp, 'glibc-shim', None, say=lambda _: None)['waydroid']['nvidia_native_gl'], 'on')
        self.assertEqual(dm.switched(cp, 'nvidia-prolapse', 'off', say=lambda _: None,
                                     install='dev-root')['waydroid']['nvidia_native_gl'], 'off')
        self.assertEqual(dm.switched(self.write({}, {}), 'venus', None, say=lambda _: None)['waydroid']['nvidia_native_gl'],
                         'off')

    def test_venus_sets_the_nvidia_render_node(self):
        said = []
        cp = self.write({'drm_device': '/dev/dri/renderD128', 'nvidia_venus_socket': '/run/waydroid-venus/venus.sock'},
                        {'mesa.vn.debug': 'vtest'})
        new = dm.switched(cp, 'venus', None, say=said.append)
        self.assertEqual(new['waydroid']['drm_device'], RENDER)
        self.assertNotIn('nvidia_venus_socket', new['waydroid'])
        self.assertNotIn('mesa.vn.debug', new['properties'])
        self.assertTrue(any('renderD128 -> ' + RENDER in s for s in said))

    def test_identity_changes_with_mode_root_and_gl(self):
        ids = {}
        for mode, gl in (('glibc-shim', 'off'), ('glibc-shim', 'on'), ('nvidia-prolapse', 'off'), ('venus', 'off')):
            ids[(mode, gl)] = dm.identity(dm.render(dm.switched(self.write({}, {}), mode, gl, say=lambda _: None,
                                                                install='dev-root' if mode == 'nvidia-prolapse' else None)))
        self.assertEqual(len(set(ids.values())), 4)
        self.assertEqual(ids[('glibc-shim', 'on')], ('glibc-shim', str(self.blob), 'nvnative'))
        self.assertEqual(ids[('nvidia-prolapse', 'off')], ('nvidia-prolapse', str(self.source), 'angle'))
        os.unlink(self.links['nvidia-prolapse'])                      # activate repointed the link: a driver change
        os.symlink(self.blob, self.links['nvidia-prolapse'])
        self.assertNotEqual(dm.identity(dm.render(dm.switched(self.write({}, {}), 'nvidia-prolapse', 'off',
                                                              say=lambda _: None, install='dev-root'))),
                            ids[('nvidia-prolapse', 'off')])

    def test_install_shape(self):
        said = []
        with self.assertRaises(RuntimeError) as cm:                  # a fresh cfg names no shape: never guessed
            dm.switched(self.write({}, {}), 'nvidia-prolapse', 'off', say=said.append)
        self.assertIn('--install vendor-image', str(cm.exception))
        new = dm.switched(self.write({}, {}), 'nvidia-prolapse', 'on', say=said.append, install='vendor-image')
        self.assertEqual(new['waydroid']['nvidia_prolapse_install'], 'vendor-image')
        self.assertEqual(dm.identity(dm.render(new)), ('nvidia-prolapse', 'vendor-image', 'nvrm'))
        kept = dm.switched(self.write(dict(new['waydroid']), {}), 'nvidia-prolapse', None, say=said.append)
        self.assertEqual(kept['waydroid']['nvidia_prolapse_install'], 'vendor-image')
        away = dm.switched(self.write(dict(new['waydroid']), {}), 'venus', None, say=said.append)
        self.assertNotIn('nvidia_prolapse_install', away['waydroid'])
        self.assertTrue(any('dropping nvidia_prolapse_install' in line for line in said))

    def test_identity_rust_driver_native_gl_is_nvrm(self):
        text = '[waydroid]\nnvidia_driver_mode = nvidia-prolapse\nnvidia_native_gl = on\n'
        self.assertEqual(dm.identity(text), ('nvidia-prolapse', str(self.source), 'nvrm'))

    def test_migrating_old_shapes_keeps_identity(self):
        # nvidia_native_root (pre-mode) and nvidia_rust_driver_root (e7d5571) naming the root the link points at
        for waydroid in ({'nvidia_guest_layout': '2', 'nvidia_native_root': self.source},
                         {'nvidia_driver_mode': 'nvidia-prolapse', 'nvidia_rust_driver_root': self.source,
                          'nvidia_native_gl': 'off'}):
            with self.subTest(waydroid=waydroid):
                text = dm.render(self.write(waydroid, {'ro.hardware.egl': 'angle'}))
                self.assertEqual(dm.identity(text), ('nvidia-prolapse', str(self.source), 'angle'))
                migrated = dm.render(dm.switched(dm.read_cfg(self.cfg), 'nvidia-prolapse', None, say=lambda _: None))
                self.assertEqual(dm.identity(migrated), dm.identity(text))   # caches kept

    def test_old_mode_name_switched_with_announcement(self):
        said = []
        out = dm.switched(self.write({'nvidia_driver_mode': 'rust-driver', 'nvidia_native_gl': 'off'}, {}),
                          'nvidia-prolapse', None, say=said.append)
        self.assertEqual(out['waydroid']['nvidia_driver_mode'], 'nvidia-prolapse')
        self.assertTrue(any('rust-driver is the old name of nvidia-prolapse' in s for s in said), said)

    def test_old_mode_name_identity_follows_the_old_link(self):
        old_link = os.path.join(os.path.dirname(self.links['nvidia-prolapse']), 'nvidia-rust-driver')
        os.symlink(str(self.source), old_link)
        with mock.patch.dict(dm.native.RENAMED_MODES, {'rust-driver': ('nvidia-prolapse', old_link)}):
            old = dm.identity('[waydroid]\nnvidia_driver_mode = rust-driver\nnvidia_native_gl = off\n')
        new = dm.identity('[waydroid]\nnvidia_driver_mode = nvidia-prolapse\nnvidia_native_gl = off\n')
        self.assertEqual(old, new)        # same root behind both links: not a driver change, caches kept

    def test_unknown_mode_refused(self):
        with self.assertRaises(RuntimeError):
            dm.switched(self.write({}, {}), 'native', None, say=lambda _: None)

    def test_override_is_mode_independent(self):
        text = dm.override_text()
        self.assertIn('dev/driver-mode verify-active', text)
        self.assertIn('build/waydroid/waydroid.py container start', text)
        self.assertTrue(text.startswith(f'[Unit]\nWants={dm.native.RENDER_DEVICE_UNIT}\n'
                                        f'After={dm.native.RENDER_DEVICE_UNIT}\n'), text)


class VerifyActiveGpuTest(unittest.TestCase):
    """verify-active makes the GPU ready before the generator runs (boot race 2026-10-17)."""

    def run_verify(self, cfg_text):
        order = []
        with tempfile.TemporaryDirectory() as d:
            cfg = Path(d) / 'waydroid.cfg'
            cfg.write_text(cfg_text)

            def check_cfg(path):
                order.append(('check_cfg', path))
                raise RuntimeError('stop after the generator')
            with mock.patch.object(dm, 'CFG', cfg), \
                 mock.patch.object(dm.native, 'ensure_gpu_ready',
                                   lambda native: order.append(('ensure_gpu_ready', native)) or []), \
                 mock.patch.object(dm.native, 'check_cfg', check_cfg), \
                 mock.patch.object(dm.native, 'ROOT_LINKS', {'nvidia-prolapse': d, 'glibc-shim': d}):
                with self.assertRaises(RuntimeError):
                    dm.verify_active()
        return order

    def test_native_mode_ensures_device_files_before_the_generator(self):
        order = self.run_verify('[waydroid]\nnvidia_driver_mode = nvidia-prolapse\nnvidia_native_gl = off\n'
                                'nvidia_prolapse_install = vendor-image\n')
        self.assertEqual([o[0] for o in order], ['ensure_gpu_ready', 'check_cfg'])
        self.assertEqual(order[0][1], True)

    def test_venus_needs_only_the_render_node(self):
        order = self.run_verify('[waydroid]\nnvidia_driver_mode = venus\nnvidia_native_gl = off\n')
        self.assertEqual(order[0], ('ensure_gpu_ready', False))


if __name__ == '__main__':
    unittest.main()
