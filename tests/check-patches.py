#!/usr/bin/env python3
"""Apply every patch from pinned pristine sources and compare all touched files."""
import io
import re
from pathlib import Path
import subprocess
import tarfile
import tempfile

repo=Path(__file__).resolve().parent.parent
for component, tree in [('mesa','mesa'), ('virglrenderer','virglrenderer'), ('hwcomposer','hwcomposer-src')]:
    source=repo/'.work'/tree
    pin=re.search(r'^base-commit: ([0-9a-f]{40})', (repo/'patches'/component/'BASE').read_text(), re.M).group(1)
    with tempfile.TemporaryDirectory(prefix='nvwd-patch-check-') as directory:
        temp=Path(directory)
        archive=subprocess.check_output(['git','-C',str(source),'archive',pin])
        with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
            # Android's .clang-format points outside its repository; it is not a build input here.
            tar.extractall(temp, members=[m for m in tar.getmembers() if m.isfile() or m.isdir()], filter='data')
        touched=set()
        for patch in sorted((repo/'patches'/component).glob('*.patch')):
            subprocess.run(['git','apply','--check',str(patch)],cwd=temp,check=True)
            subprocess.run(['git','apply',str(patch)],cwd=temp,check=True)
            for line in patch.read_text().splitlines():
                if line.startswith('+++ b/'):
                    touched.add(line[6:])
        for name in touched:
            if (temp/name).read_bytes()!=(source/name).read_bytes():
                raise SystemExit(f'patch/build source mismatch: {component}/{name}')
        print(f'PASS: {component}: full series applies at {pin}, {len(touched)} touched files match build sources',flush=True)
