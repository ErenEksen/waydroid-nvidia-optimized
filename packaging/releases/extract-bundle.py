#!/usr/bin/env python3
"""Extract only the complete flat bundle; reject links, paths and oversized files."""
from pathlib import Path
import sys
import tarfile
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'dev'))
from perf.bundle import ARTIFACTS, load_bundle


def extract(archive, destination):
    destination = Path(destination)
    allowed = set(ARTIFACTS) | {'manifest.json'}
    with tarfile.open(archive, 'r:gz') as tar:
        members = tar.getmembers()
        if len(members) != len(allowed) or {m.name for m in members} != allowed:
            raise ValueError('expected exactly the flat complete-set bundle')
        if any(not m.isfile() or m.size > 64 * 1024 * 1024 for m in members):
            raise ValueError('links, special files and oversized files are forbidden')
        destination.mkdir(mode=0o700, parents=False, exist_ok=False)
        for member in members:
            with tar.extractfile(member) as source:
                (destination / member.name).write_bytes(source.read())
    load_bundle(destination)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: extract-bundle.py ARCHIVE NEW_DIRECTORY')
    extract(sys.argv[1], sys.argv[2])
