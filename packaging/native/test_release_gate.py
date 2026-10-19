#!/usr/bin/env python3
"""Unit tests of the release bundle check (no GPU, no root): python3 -m pytest -q packaging/native/test_release_gate.py"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_gate as gate  # noqa: E402


def bundle(tmp_path, files=None):
    files = files or {'payload/vendor_extra/lib64/libx.so': b'x', 'gate/bin/android/vk-compute': b'y'}
    for rel, data in files.items():
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_bytes(data)
    record = {'format': 2, 'commit': '0' * 40,
              'files': {rel: hashlib.sha256(data).hexdigest() for rel, data in files.items()}}
    (tmp_path / gate.RECORD).write_text(json.dumps(record))
    return tmp_path


def test_intact_bundle_passes(tmp_path):
    assert gate.verify_bundle(bundle(tmp_path))['commit'] == '0' * 40


def test_modified_file_refused(tmp_path):
    b = bundle(tmp_path)
    (b / 'gate/bin/android/vk-compute').write_bytes(b'z')
    with pytest.raises(RuntimeError, match='modified: gate/bin/android/vk-compute'):
        gate.verify_bundle(b)


def test_extra_and_missing_files_refused(tmp_path):
    b = bundle(tmp_path)
    (b / 'payload/extra.so').write_bytes(b'e')
    with pytest.raises(RuntimeError, match='not in the build record: payload/extra.so'):
        gate.verify_bundle(b)
    (b / 'payload/extra.so').unlink()
    (b / 'payload/vendor_extra/lib64/libx.so').unlink()
    with pytest.raises(RuntimeError, match='missing payload/vendor_extra/lib64/libx.so'):
        gate.verify_bundle(b)


def test_wrong_format_refused(tmp_path):
    b = bundle(tmp_path)
    record = json.loads((b / gate.RECORD).read_text())
    record['format'] = 1
    (b / gate.RECORD).write_text(json.dumps(record))
    with pytest.raises(RuntimeError, match='format 1'):
        gate.verify_bundle(b)


def test_elf_bits(tmp_path):
    (tmp_path / 'a').write_bytes(b'\x7fELF\x02rest')
    (tmp_path / 'b').write_bytes(b'\x7fELF\x01rest')
    (tmp_path / 'c').write_bytes(b'#!/bin/sh')
    assert [gate.elf_bits(tmp_path / n) for n in 'abc'] == [64, 32, None]


# ---------------------------------------------------------------- DT_NEEDED from the ELF file

def make_elf(bits=64, endian='<', needed=('libc.so', 'libdl.so'), runpath=None, dynamic=True, strtab_mapped=True,
             dt_null=True):
    """A minimal ELF object: header, PT_LOAD over the whole file, PT_DYNAMIC, .dynstr. Virtual addresses are
    file offsets + 0x1000, so DT_STRTAB must be translated through PT_LOAD."""
    import struct
    is64 = bits == 64
    ehsize, phentsize = (64, 56) if is64 else (52, 32)
    phnum = 2 if dynamic else 1
    strtab = b'\0'
    offsets = []
    for name in list(needed) + ([runpath] if runpath else []):
        offsets.append(len(strtab))
        strtab += name.encode() + b'\0'
    str_off = ehsize + phnum * phentsize
    dyn_off = str_off + len(strtab)
    dyn_off += -dyn_off % 8
    vbase = 0x1000
    tags = [(1, o) for o in offsets[:len(needed)]]
    if runpath:
        tags.append((29, offsets[-1]))
    tags += [(5, (str_off + vbase) if strtab_mapped else 0x900000), (10, len(strtab))]
    if dt_null:
        tags.append((0, 0))
    dfmt = end = endian
    dfmt += 'qQ' if is64 else 'iI'
    dyn = b''.join(struct.pack(dfmt, t, v) for t, v in tags)
    total = dyn_off + len(dyn)
    if is64:
        head = struct.pack(end + '4sBBBBB7sHHIQQQIHHHHHH', b'\x7fELF', 2, 1 if end == '<' else 2, 1, 0, 0, b'',
                           3, 62, 1, 0, ehsize, 0, 0, ehsize, phentsize, phnum, 64, 0, 0)
        load = struct.pack(end + 'IIQQQQQQ', 1, 5, 0, vbase, vbase, total, total, 0x1000)
        dynp = struct.pack(end + 'IIQQQQQQ', 2, 6, dyn_off, dyn_off + vbase, dyn_off + vbase, len(dyn), len(dyn), 8)
    else:
        head = struct.pack(end + '4sBBBBB7sHHIIIIIHHHHHH', b'\x7fELF', 1, 1 if end == '<' else 2, 1, 0, 0, b'',
                           3, 3, 1, 0, ehsize, 0, 0, ehsize, phentsize, phnum, 40, 0, 0)
        load = struct.pack(end + 'IIIIIIII', 1, 0, vbase, vbase, total, total, 5, 0x1000)
        dynp = struct.pack(end + 'IIIIIIII', 2, dyn_off, dyn_off + vbase, dyn_off + vbase, len(dyn), len(dyn), 6, 4)
    out = head + load + (dynp if dynamic else b'') + strtab
    return out + b'\0' * (dyn_off - len(out)) + dyn


@pytest.mark.parametrize('bits', [32, 64])
@pytest.mark.parametrize('endian', ['<', '>'])
def test_needed_from_the_file(tmp_path, bits, endian):
    f = tmp_path / 'lib.so'
    f.write_bytes(make_elf(bits, endian, runpath='/a:/b'))
    assert gate.needed(f) == ['libc.so', 'libdl.so']
    assert gate.elf_dynamic(f)['runpath'] == ['/a', '/b']


def test_needed_does_not_read_tool_output(tmp_path, monkeypatch):
    """Issue #28: binutils localises readelf ("Biblioteca compartida: [libdl.so]"); the closure came back empty and
    the gate died with 'library "libdl.so" not found'. A readelf on PATH that prints Spanish changes nothing now."""
    f = tmp_path / 'vk-compute'
    f.write_bytes(make_elf())
    fake = tmp_path / 'bin'
    fake.mkdir()
    (fake / 'readelf').write_text('#!/bin/sh\necho " 0x1 (NEEDED) Biblioteca compartida: [libdl.so]"\n')
    (fake / 'readelf').chmod(0o755)
    monkeypatch.setenv('PATH', f'{fake}:{os.environ["PATH"]}')
    monkeypatch.setenv('LC_ALL', 'es_ES.UTF-8')
    assert gate.needed(f) == ['libc.so', 'libdl.so']


def foreign_locales():
    try:
        out = subprocess.run(['locale', '-a'], capture_output=True, text=True).stdout.split()
    except OSError:
        return []
    return [l for l in out if l.split('.')[0] not in ('C', 'POSIX') and not l.startswith('en_')]


@pytest.mark.skipif(not shutil.which('readelf'), reason='binutils not installed')
@pytest.mark.parametrize('locale', foreign_locales()[:1] or [None])
def test_needed_matches_readelf_on_system_objects(locale):
    """Same names as `LC_ALL=C readelf -dW` for real objects; also under an installed non-English locale."""
    objs = [p for p in (shutil.which('ls'), shutil.which('python3'), '/usr/lib/libc.so.6', '/usr/lib32/libc.so.6',
                        '/lib/x86_64-linux-gnu/libc.so.6') if p and Path(p).is_file()]
    assert objs
    code = ('import json, sys; sys.path.insert(0, sys.argv[1]); import release_gate as g; '
            'print(json.dumps([g.elf_dynamic(p)["needed"] for p in sys.argv[2:]]))')
    env = dict(os.environ, LC_ALL=locale) if locale else dict(os.environ)
    got = json.loads(subprocess.check_output([sys.executable, '-c', code, str(Path(gate.__file__).parent), *objs],
                                             env=env, text=True))
    for obj, names in zip(objs, got):
        text = subprocess.check_output(['readelf', '-dW', obj], text=True, env=dict(os.environ, LC_ALL='C'))
        assert names == re.findall(r'\(NEEDED\)\s+Shared library: \[(.+?)\]', text), obj


@pytest.mark.parametrize('kwargs,message', [
    ({'dynamic': False}, 'no PT_DYNAMIC'),
    ({'strtab_mapped': False}, 'outside the file'),
    ({'dt_null': False}, 'without DT_NULL'),
    ({'needed': ()}, 'no DT_NEEDED entry'),
])
def test_damaged_or_empty_dynamic_section_is_an_error(tmp_path, kwargs, message):
    f = tmp_path / 'x.so'
    f.write_bytes(make_elf(**kwargs))
    with pytest.raises(gate.ElfError, match=message):
        gate.needed(f)


def test_truncated_and_non_elf_files(tmp_path):
    (tmp_path / 'cut').write_bytes(make_elf()[:70])
    (tmp_path / 'sh').write_bytes(b'#!/bin/sh\n' + b'\0' * 60)
    with pytest.raises(gate.ElfError, match='truncated'):
        gate.needed(tmp_path / 'cut')
    with pytest.raises(gate.ElfError, match='no ELF header'):
        gate.needed(tmp_path / 'sh')


def test_image_leaf_library_may_need_nothing(tmp_path):
    """ld-android.so of the user's image has no DT_NEEDED: extract_runtime accepts it (leaf_ok)."""
    f = tmp_path / 'ld-android.so'
    f.write_bytes(make_elf(needed=()))
    assert gate.needed(f, leaf_ok=True) == []
