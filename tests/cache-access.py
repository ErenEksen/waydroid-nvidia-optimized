#!/usr/bin/env python3
"""Watch cache metadata events during an isolated command; never read its contents."""
import ctypes
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time

root = Path(sys.argv[1]).resolve()
command = sys.argv[2:]
if not root.is_dir() or not command:
    raise SystemExit('usage: cache-access.py DIRECTORY COMMAND [ARGS...]')
libc = ctypes.CDLL(None, use_errno=True)
fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
if fd < 0:
    raise OSError(ctypes.get_errno(), 'inotify_init1')
watches = {}
for file in root.rglob('*'):
    if file.is_file() and not file.is_symlink():
        wd = libc.inotify_add_watch(fd, os.fsencode(file), 0x1 | 0x20)  # ACCESS | OPEN
        if wd < 0:
            raise OSError(ctypes.get_errno(), 'inotify_add_watch')
        watches[wd] = str(file.relative_to(root))
start = time.monotonic_ns()
result = subprocess.run(command, timeout=30, capture_output=True, text=True)
elapsed = time.monotonic_ns()-start
events = []
while True:
    try:
        data = os.read(fd, 65536)
    except BlockingIOError:
        break
    at = 0
    while at < len(data):
        wd, mask, cookie, length = struct.unpack_from('iIII', data, at)
        events.append({'path': watches.get(wd), 'opened': bool(mask & 0x20), 'accessed': bool(mask & 1), 'mask': mask})
        at += 16+length
os.close(fd)
print(json.dumps({'command_stdout': result.stdout, 'command_stderr': result.stderr,
                  'returncode': result.returncode, 'elapsed_ns': elapsed, 'events': events}, indent=2))
raise SystemExit(0 if result.returncode == 0 and any(e['accessed'] for e in events) else 1)
