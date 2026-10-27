#!/usr/bin/env python3
"""Package the nvidia-prolapse vendor image as a Waydroid OTA zip plus its one-image OTA channel JSON.

  packaging/vendor-ota.py VENDOR_IMG BUILD_RECORD OUT_DIR --tag TAG [--repo OWNER/NAME]

One recipe for the CI release job (.github/workflows/build.yml) and local release builds. Writes
  OUT_DIR/waydroid-nvidia-vendor-x86_64-TAG.zip        vendor.img, the only member (what `waydroid upgrade` extracts)
  OUT_DIR/waydroid-nvidia-vendor-ota-TAG.json          {"response": [{datetime, filename, id, romtype, size, url,
                                                       version}]}, the format of Waydroid's own channels
                                                       (ota.waydro.id/vendor/waydroid_x86_64/MAINLINE.json)
The JSON is served from the same GitHub release; waydroid-nvidia-setup points waydroid.cfg vendor_ota at it, so
Waydroid's updater (tools/helpers/images.py get(): newer datetime -> download, sha256 must equal id) only ever
sees the image of the installed release. datetime is the image's ro.vendor.build.date.utc.

Refuses an image whose ro.vendor.nvidia_prolapse.build_record is not the sha256 of BUILD_RECORD (the native
bundle the release ships), so the image and the package always come from one build.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile


def build_props(image):
    out = subprocess.run(['debugfs', '-R', 'cat /build.prop', str(image)], capture_output=True, check=True).stdout
    return dict(l.split('=', 1) for l in out.decode(errors='replace').splitlines() if '=' in l and not l.startswith('#'))


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('vendor_img', type=Path)
    parser.add_argument('build_record', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--repo', default='Shiro836/waydroid-nvidia')
    args = parser.parse_args()
    if not re.fullmatch(r'v[0-9A-Za-z.+-]+', args.tag):
        raise SystemExit(f'vendor-ota: invalid tag {args.tag!r}')
    props = build_props(args.vendor_img)
    record_sha = sha256(args.build_record)
    if props.get('ro.vendor.nvidia_prolapse.build_record') != record_sha:
        raise SystemExit(f'vendor-ota: {args.vendor_img} carries ro.vendor.nvidia_prolapse.build_record='
                         f'{props.get("ro.vendor.nvidia_prolapse.build_record")}, the release bundle is {record_sha}')
    stamp = int(props['ro.vendor.build.date.utc'])
    args.out.mkdir(parents=True, exist_ok=True)
    name = f'waydroid-nvidia-vendor-x86_64-{args.tag}.zip'
    zpath = args.out / name
    info = zipfile.ZipInfo('vendor.img', date_time=(1980, 1, 1, 0, 0, 0))   # fixed: the zip depends on the image only
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    with zipfile.ZipFile(zpath, 'w', allowZip64=True) as z, open(args.vendor_img, 'rb') as src, \
            z.open(info, 'w', force_zip64=True) as dst:
        while chunk := src.read(16 << 20):
            dst.write(chunk)
    entry = {'datetime': stamp, 'filename': name, 'id': sha256(zpath), 'romtype': 'MAINLINE',
             'size': zpath.stat().st_size,
             'url': f'https://github.com/{args.repo}/releases/download/{args.tag}/{name}', 'version': '20.0'}
    channel = args.out / f'waydroid-nvidia-vendor-ota-{args.tag}.json'
    channel.write_text(json.dumps({'response': [entry]}, indent=2) + '\n')
    print(f'{zpath} ({entry["size"]} bytes, sha256 {entry["id"]})\n{channel} (datetime {stamp}, '
          f'{props.get("ro.vendor.nvidia_prolapse.version")})')


if __name__ == '__main__':
    main()
