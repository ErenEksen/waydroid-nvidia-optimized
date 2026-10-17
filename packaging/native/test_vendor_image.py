#!/usr/bin/env python3
"""Unit tests of the vendor image packaging (no GPU, no root): python3 -m pytest -q packaging/native/test_vendor_image.py"""
import hashlib
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

import pytest

REPO = Path(__file__).resolve().parents[2]


def load(path, name):
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


vi = load(REPO / 'packaging/native/vendor_image.py', 'vendor_image')


def image(tmp_path, props):
    src = tmp_path / 'src'
    src.mkdir()
    (src / 'build.prop').write_text(''.join(f'{k}={v}\n' for k, v in props.items()))
    img = tmp_path / 'vendor.img'
    subprocess.run(['mke2fs', '-q', '-F', '-t', 'ext4', '-d', str(src), str(img), '2M'], check=True, capture_output=True)
    return img


def test_prebuilt_table_matches_the_product_makefile():
    """Every bundle binary Android.mk installs into the image is checked by `install`, and nothing else."""
    mk = (REPO / 'vendor-image/product/vendor/nvidia-prolapse/Android.mk').read_text()
    want = {}
    for module, stem, rel, src in re.findall(r'nvp-prebuilt-lib,([^,]+),([^,]+),([^,]*),\$\(NVP_NATIVE\)/([^,)]+)', mk):
        if '$(l' in stem:
            continue                                  # the $(foreach ...) block, below
        for sub in ('lib64', 'lib'):
            want['payload/' + src.replace('%LIB%', sub)] = f'/{sub}/' + (f'{rel}/' if rel else '') + stem
    for l in ('EGL', 'GLESv1_CM', 'GLESv2'):        # the $(foreach ...) block
        for sub in ('lib64', 'lib'):
            want[f'payload/vendor/{sub}/egl/lib{l}_nvrm.so'] = f'/{sub}/egl/lib{l}_nvrm.so'
    service = re.search(r'LOCAL_SRC_FILES := \$\(NVP_NATIVE\)/(odm/bin/hw/\S+)', mk).group(1)
    want['payload/' + service] = '/' + service
    assert vi.IMAGE_PREBUILTS == want


def test_ota_zip_and_channel(tmp_path):
    record = tmp_path / 'BUILD-RECORD.json'
    record.write_text('{"format": 2}')
    sha = hashlib.sha256(record.read_bytes()).hexdigest()
    img = image(tmp_path, {'ro.vendor.build.date.utc': '1792170882', 'ro.vendor.nvidia_prolapse.version': 'v1.0.0',
                           'ro.vendor.nvidia_prolapse.build_record': sha})
    out = tmp_path / 'out'
    subprocess.run([sys.executable, str(REPO / 'packaging/vendor-ota.py'), str(img), str(record), str(out),
                    '--tag', 'v1.0.0', '--repo', 'o/r'], check=True, capture_output=True)
    zpath, channel = out / 'waydroid-nvidia-vendor-x86_64-v1.0.0.zip', out / 'waydroid-nvidia-vendor-ota-v1.0.0.json'
    with zipfile.ZipFile(zpath) as z:
        assert z.namelist() == ['vendor.img'] and z.read('vendor.img') == img.read_bytes()
    entry, url = vi.channel_entry(channel)
    assert entry['id'] == hashlib.sha256(zpath.read_bytes()).hexdigest() and entry['datetime'] == 1792170882
    assert entry['url'] == 'https://github.com/o/r/releases/download/v1.0.0/' + zpath.name
    assert url == 'https://github.com/o/r/releases/download/v1.0.0/' + channel.name
    # waydroid's own updater logic (tools/helpers/images.py get): only a NEWER datetime is downloaded
    assert not entry['datetime'] > int(str(entry['datetime']))


def test_ota_refuses_an_image_of_another_bundle(tmp_path):
    record = tmp_path / 'BUILD-RECORD.json'
    record.write_text('{}')
    img = image(tmp_path, {'ro.vendor.build.date.utc': '1', 'ro.vendor.nvidia_prolapse.build_record': '0' * 64})
    proc = subprocess.run([sys.executable, str(REPO / 'packaging/vendor-ota.py'), str(img), str(record),
                           str(tmp_path / 'out'), '--tag', 'v1'], capture_output=True, text=True)
    assert proc.returncode != 0 and 'the release bundle is' in proc.stderr


def test_channel_entry_rejects_other_shapes(tmp_path):
    bad = tmp_path / 'c.json'
    bad.write_text(json.dumps({'response': []}))
    with pytest.raises(RuntimeError, match='one-image'):
        vi.channel_entry(bad)
    bad.write_text(json.dumps({'response': [{'datetime': 1, 'filename': 'a.zip', 'id': 'x', 'size': 1,
                                             'url': 'https://h/other.zip'}]}))
    with pytest.raises(RuntimeError, match='does not name a.zip'):
        vi.channel_entry(bad)
