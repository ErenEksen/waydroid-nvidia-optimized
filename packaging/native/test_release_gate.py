#!/usr/bin/env python3
"""Unit tests of the release bundle check (no GPU, no root): python3 -m pytest -q packaging/native/test_release_gate.py"""
import hashlib
import json
from pathlib import Path
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
