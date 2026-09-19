#!/usr/bin/env python3
"""Isolated injected failure matrix. Requires no root and never changes installed services."""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import threading
import time

repo = Path(__file__).resolve().parent.parent
output = repo/'.work'/('fault-regression-'+time.strftime('%Y%m%d-%H%M%S'))
output.mkdir()
results = []
for mode in ('delay', 'import', 'export', 'device-lost'):
    out = output/mode
    env = {**os.environ, 'NVWD_FAULT': mode, 'SYNC_CASES': 'socket', 'SYNC_ROUNDS': '40', 'SYNC_TEST_OUT': str(out)}
    start = time.monotonic()
    p = subprocess.run([str(repo/'tests/run-sync-regression.sh')], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=110)
    (output/(mode+'.log')).write_text(p.stdout)
    log = (out/'server.log').read_text() if (out/'server.log').exists() else ''
    injected = 'FAULT SHIM enabled' in log
    expected_success = mode in ('delay', 'import')
    passed = injected and ((p.returncode == 0) if expected_success else (p.returncode not in (0, 124, 137)))
    results.append({'case': mode, 'returncode': p.returncode, 'elapsed_s': time.monotonic()-start, 'injection_seen': injected, 'passed': passed})
    print(results[-1], flush=True)

# Test EOF and stalled peer without involving a GPU. Existing vtest errors abort
# the affected client; the regression requirement here is bounded, explicit exit.
client = output/'delay/sync-import'
for mode in ('socket-eof', 'socket-timeout'):
    with tempfile.TemporaryDirectory(prefix='nvwd-fault-') as directory:
        path = directory+'/venus.sock'
        server = socket.socket(socket.AF_UNIX)
        server.bind(path)
        server.listen(1)
        stop = threading.Event()
        def peer():
            conn, _ = server.accept()
            if mode == 'socket-timeout':
                stop.wait(40)
            conn.close()
        thread = threading.Thread(target=peer, daemon=True)
        thread.start()
        env = {**os.environ, 'VK_ICD_FILENAMES': str(repo/'.work/mesa-host/src/virtio/vulkan/virtio_devenv_icd.x86_64.json'),
               'VN_DEBUG': 'vtest', 'VTEST_SOCKET_NAME': path}
        start = time.monotonic()
        # Disable core dumps in the child only.
        import resource
        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        try:
            p = subprocess.run([str(client), '1'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=36, preexec_fn=no_core)
            passed = p.returncode != 0 and 'lost connection' in p.stdout
            (output/(mode+'.log')).write_text(p.stdout)
        except subprocess.TimeoutExpired:
            passed = False
        finally:
            stop.set()
            thread.join(timeout=2)
            server.close()
        results.append({'case': mode, 'elapsed_s': time.monotonic()-start, 'passed': passed})
        print(results[-1], flush=True)
(output/'report.json').write_text(json.dumps(results, indent=2)+'\n')
print(output)
raise SystemExit(0 if all(r['passed'] for r in results) else 1)
