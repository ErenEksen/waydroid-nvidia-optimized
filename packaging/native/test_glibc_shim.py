#!/usr/bin/env python3
"""Unit tests of packaging/native/glibc_shim.py (no root, no GPU): python3 -m pytest -q packaging/native/test_glibc_shim.py

A fake release bundle (glibc-shim/ tree + BUILD-RECORD.json) and a fake host driver (waydroid/test_nvidia_native.py's
fake libraries and loader cache) in a temporary directory."""
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
from unittest import mock

import pytest

REPO = Path(__file__).resolve().parents[2]


def load(path, name):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


gs = load(REPO / 'packaging/native/glibc_shim.py', 'glibc_shim_under_test')
tn = load(REPO / 'waydroid/test_nvidia_native.py', 'test_nvidia_native_helpers')
nn = gs.native
V = tn.V
ELF = {64: tn.fake_elf(64, []), 32: tn.fake_elf(32, [])}
DRM = ['drmIoctl', 'drmPrimeFDToHandle', 'drmGetCap']


def make_bundle(top):
    """A release bundle whose glibc-shim tree has every file collect.py puts there (fake contents)."""
    bundle = top / 'release'
    files = {}
    for sub, shims in (('lib64', nn.SHIMS), ('lib', nn.SHIMS32)):
        bits = 64 if sub == 'lib64' else 32
        lib = f'root/vendor_extra/{sub}'
        for name in list(shims) + ['hw/vulkan.nvidia.so', nn.GBM_WRAPPER, nn.MINIGBM_CORE, nn.LIBDRM, nn.EGL_PLATFORM]:
            files[f'{lib}/{name}'] = ELF[bits]
        files[f'{lib}/{nn.EGL_PLATFORM_JSON}'] = tn.JSON
        files[f'{lib}/{nn.DEFAULT_ENV}'] = tn.ENVLINE
        for front in nn.EGL_FRONT:
            files[f'root/vendor/{sub}/egl/{front}'] = ELF[bits]
    for arch in ('x86_64', 'x86'):
        files[f'check/census-{arch}.tsv'] = b'front\tname\tversions\tblobs\n'
        files[f'check/libdrm-{arch}.names'] = ''.join(n + '\n' for n in DRM).encode()
    record = {}
    for rel, data in files.items():
        p = bundle / 'glibc-shim' / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        record['glibc-shim/' + rel] = hashlib.sha256(data).hexdigest()
    (bundle / 'BUILD-RECORD.json').write_text(json.dumps({'format': 2, 'files': {}, 'glibc_shim_files': record}))
    return bundle


@pytest.fixture
def host(tmp_path):
    tn.make_fake_driver(str(tmp_path))
    tn.touch(str(tmp_path / 'os-release'), b'ID=arch\n')
    for node in ('nvidiactl', 'nvidia-modeset', 'nvidia0'):
        tn.touch(str(tmp_path / 'dev' / node))
    for sub, bits in (('lib64', 64), ('lib', 32)):
        for name in nn.EGL_ANGLE:
            tn.touch(str(tmp_path / 'nv-guest/vendor' / sub / 'egl' / name), tn.ELF[bits])
    patches = [mock.patch.object(nn, 'LD_SO_CACHE', str(tmp_path / 'ld.so.cache')),
               mock.patch.object(nn, 'OS_RELEASE', str(tmp_path / 'os-release')),
               mock.patch.object(nn, 'driver_version', lambda *a, **k: V),
               mock.patch.object(nn, 'nvidia_render_node', lambda *a, **k: '/dev/dri/renderD129'),
               mock.patch.object(nn, 'ANGLE_GUEST_ROOT', str(tmp_path / 'nv-guest')),
               mock.patch.object(nn, 'DEV_DIR', str(tmp_path / 'dev')),
               mock.patch.object(nn, 'ROOT_LINKS', {'glibc-shim': str(tmp_path / 'nvidia-glibc-shim'),
                                                    'nvidia-prolapse': str(tmp_path / 'nvidia-prolapse')}),
               mock.patch.object(gs, 'imports_not_covered', lambda arch, census, paths: ''),
               mock.patch.object(gs, 'drm_names', lambda paths: {n: ['libnvidia-glsi.so.' + V] for n in DRM}),
               mock.patch.object(gs, 'gate', lambda *a, **k: [])]
    for p in patches:
        p.start()
    yield tmp_path
    for p in patches:
        p.stop()


def refused(fn, *needles):
    with pytest.raises((gs.Refused, OSError)) as e:
        fn()
    for n in needles:
        assert n in str(e.value), str(e.value)
    return str(e.value)


def test_check_passes_and_names_the_closure(host, capsys):
    v, closures, files = gs.check(make_bundle(host), native_gl=False)
    assert v == V and [n for n, _ in closures[64]] == [c.format(v=V) for c in tn.CLOSURE]
    assert all(p.startswith(str(host / 'usr-lib32')) for _, p in closures[32])
    assert 'every import covered' in capsys.readouterr().out


def test_release_without_glibc_shim(host):
    bundle = make_bundle(host)
    (bundle / 'BUILD-RECORD.json').write_text(json.dumps({'format': 2, 'files': {}}))
    refused(lambda: gs.check(bundle, False), 'does not ship glibc-shim')


def test_modified_or_extra_file(host):
    bundle = make_bundle(host)
    (bundle / 'glibc-shim/root/vendor_extra/lib64/libc.so.6').write_bytes(ELF[64] + b'x')
    refused(lambda: gs.check(bundle, False), 'modified: glibc-shim/root/vendor_extra/lib64/libc.so.6')
    bundle = make_bundle(host / 'b')
    (bundle / 'glibc-shim/root/extra').write_bytes(b'')
    refused(lambda: gs.check(bundle, False), 'not in the build record')


def test_import_not_covered(host):
    with mock.patch.object(gs, 'imports_not_covered', lambda arch, c, p: 'import-census: newname (libc.so.6) ...'):
        refused(lambda: gs.check(make_bundle(host), False), 'newname', 'Update waydroid-nvidia', '64-bit')


def test_drm_name_not_exported(host):
    with mock.patch.object(gs, 'drm_names', lambda paths: {'drmNewThing': ['libnvidia-glsi.so.' + V]}):
        refused(lambda: gs.check(make_bundle(host), False), 'drmNewThing')


def test_32bit_driver_not_installed(host):
    for d in ('usr-lib', 'usr-lib32'):
        os.unlink(host / d / 'libEGL_nvidia.so.0')
    tn.make_fake_driver(str(host), bits_present=(64,))
    refused(lambda: gs.check(make_bundle(host), False), '32-bit', 'lib32-nvidia-utils')


def test_old_branch_refused(host):
    with mock.patch.object(nn, 'driver_version', lambda *a, **k: '470.256.02'):
        refused(lambda: gs.check(make_bundle(host), False), '470.256.02', 'VK_EXT_external_memory_dma_buf')


def test_native_gl_needs_nvx(host):
    for d in ('usr-lib', 'usr-lib32'):
        os.unlink(host / d / 'libEGL_nvidia.so.0')
    tn.make_fake_driver(str(host), icd_entries=('eglDupNativeFenceFDANDROID',))       # 550: no NVX
    bundle = make_bundle(host)
    gs.check(bundle, native_gl=False)
    refused(lambda: gs.check(bundle, native_gl=True), 'eglPlatformGetVersionNVX', 'nvidia_native_gl = off')


def test_stage_writes_the_tree_and_points_the_link(host):
    bundle, root = make_bundle(host), host / 'staging'
    stale = root / 'vendor_extra/lib64/libnvidia-glsi.so.610.57.04'
    stale.parent.mkdir(parents=True)
    stale.touch()
    with mock.patch.object(os, 'geteuid', lambda: 0), mock.patch.object(gs, 'glibc_version', lambda: '2.42'):
        gs.stage(bundle, root, native_gl=False, tag='v0.2.0-rc12')
    for sub in ('lib64', 'lib'):
        lib = root / 'vendor_extra' / sub
        assert os.readlink(lib / 'libEGL_nvidia.so.0') == 'libEGL_nvidia.so.' + V
        assert (lib / 'host-glibc-version').read_text() == '2.42\n'
        for c in tn.CLOSURE:
            p = lib / c.format(v=V)
            assert p.is_file() and p.stat().st_size == 0
        assert (lib / 'hw/vulkan.nvidia.so').read_bytes() == ELF[64 if sub == 'lib64' else 32]
    assert not stale.exists()
    assert os.readlink(host / 'nvidia-glibc-shim') == str(root)
    assert 'package v0.2.0-rc12 glibc-shim driver ' + V in (root / 'staged-build-info').read_text()
    # what the container binds: the host's own files over the placeholders
    entries = nn.lxc_nodes(str(root))
    assert 'lxc.mount.entry = {}/usr-lib/libnvidia-gpucomp.so.{v} vendor_extra/lib64/libnvidia-gpucomp.so.{v} none ' \
           'rbind,create=file 0 0'.format(host, v=V) in entries


def test_stage_refuses_a_nonempty_stale_file(host):
    bundle, root = make_bundle(host), host / 'staging'
    stale = root / 'vendor_extra/lib/libnvidia-eglcore.so.610.57.04'
    stale.parent.mkdir(parents=True)
    stale.write_bytes(b'data')
    with mock.patch.object(os, 'geteuid', lambda: 0), mock.patch.object(gs, 'glibc_version', lambda: '2.42'):
        refused(lambda: gs.stage(bundle, root, False, None), 'not an empty placeholder')


def test_stage_needs_root(host):
    with mock.patch.object(os, 'geteuid', lambda: 1000):
        refused(lambda: gs.stage(make_bundle(host), host / 'staging', False, None), 'needs root')


def test_stage_refuses_when_the_gate_fails(host):
    def failing(*a, **k):
        raise RuntimeError('gate case android/compute FAILED (exit 1); log /x')
    bundle, root = make_bundle(host), host / 'staging'
    with mock.patch.object(os, 'geteuid', lambda: 0), mock.patch.object(gs, 'glibc_version', lambda: '2.42'), \
            mock.patch.object(gs, 'gate', failing):
        refused(lambda: gs.stage(bundle, root, False, None), 'gate failed', 'android/compute', 'not switched')
    assert not os.path.lexists(host / 'nvidia-glibc-shim')        # the root link was not pointed at it
