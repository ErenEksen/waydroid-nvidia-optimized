"""Bounded subprocesses and opt-in, read-only host resource capture."""
import json
import os
from pathlib import Path
import subprocess
import threading
import time


def run(argv, timeout=30, check=True, env=None):
    p = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True, timeout=timeout, env=env)
    if check and p.returncode:
        raise RuntimeError(f"{' '.join(argv[:3])}: {p.stderr.strip() or p.stdout.strip()}")
    return p


def guest(command, timeout=30):
    # PIPEs are intentional: lxc-attach can chown its inherited stdio fds.
    return run(["sudo", "-n", "lxc-attach", "-P", "/var/lib/waydroid/lxc", "-n", "waydroid",
                "--clear-env", "-v", "PATH=/system/bin", "--", "/system/bin/sh", "-c", command], timeout).stdout


def clock_compatible():
    try:
        host = Path("/proc/self/timens_offsets").read_text().split()
        android = guest("cat /proc/self/timens_offsets").split()
        return host == android
    except (OSError, RuntimeError):
        return False


class HostCapture:
    def __init__(self, output, unit="wd-venus.service"):
        self.output, self.unit = Path(output), unit
        self.stop_event = threading.Event()
        self.smi = None
        self.thread = None
        self.files = []

    def __enter__(self):
        self.output.mkdir(parents=True, exist_ok=True)
        self.cgroup = run(["systemctl", "--user", "show", self.unit, "-p", "ControlGroup", "--value"]).stdout.strip()
        if not self.cgroup.startswith("/"):
            raise RuntimeError("renderer service has no cgroup")
        out = (self.output / "gpu.csv").open("w")
        err = (self.output / "gpu.err").open("w")
        self.files.extend([out, err])
        try:
            self.smi = subprocess.Popen(["nvidia-smi", "--query-gpu=timestamp,utilization.gpu,utilization.memory,memory.used,power.draw,clocks.sm,temperature.gpu,pstate",
                                         "--format=csv", "-lms", "250"], stdout=out, stderr=err, stdin=subprocess.DEVNULL)
        except OSError as exc:
            err.write(str(exc))
        self.thread = threading.Thread(target=self.sample, daemon=True)
        self.thread.start()
        return self

    def sample(self):
        root = Path("/sys/fs/cgroup") / self.cgroup.lstrip("/")
        with (self.output / "resources.jsonl").open("w") as out:
            while not self.stop_event.is_set():
                row = {"monotonic_ns": time.monotonic_ns(), "processes": []}
                try:
                    for name in ("cpu.stat", "memory.current", "memory.events", "pids.current", "cpu.pressure", "memory.pressure", "io.pressure"):
                        f = root / name
                        if f.exists():
                            row[name] = f.read_text().strip()
                    pids = set()
                    for f in root.rglob("cgroup.procs"):
                        pids.update(f.read_text().split())
                    for pid in sorted(pids):
                        try:
                            proc = Path("/proc") / pid
                            status = proc.joinpath("status").read_text().splitlines()
                            data = {k: v.strip() for k, v in (line.split(":", 1) for line in status if ":" in line)}
                            row["processes"].append({"pid": int(pid), "name": data.get("Name"),
                                "rss": data.get("VmRSS"), "threads": data.get("Threads"),
                                "fds": len(list(proc.joinpath("fd").iterdir()))})
                        except (OSError, ValueError):
                            pass  # short-lived process; never conflate it with a zero sample
                except OSError as exc:
                    row["error"] = str(exc)
                out.write(json.dumps(row) + "\n")
                out.flush()
                self.stop_event.wait(0.25)

    def __exit__(self, *args):
        self.stop_event.set()
        if self.thread:
            self.thread.join(timeout=5)
        if self.smi:
            self.smi.terminate()
            try:
                self.smi.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.smi.kill()
                self.smi.wait()
        for f in self.files:
            f.close()


class DesktopProbe:
    """Separate visible surface; no readiness warm-up of Android."""
    def __init__(self, output):
        self.output = Path(output)

    def __enter__(self):
        exe = Path(__file__).resolve().parents[2]/'.work/present-probe/probe'
        if not exe.is_file():
            raise RuntimeError('build the presentation probe using dev/present-probe --build-only')
        self.out = (self.output/'desktop.csv').open('w')
        self.err = (self.output/'desktop.err').open('w')
        try:
            self.proc = subprocess.Popen([str(exe), '3600'], stdout=self.out, stderr=self.err)
        except BaseException:
            self.out.close()
            self.err.close()
            raise
        return self

    def __exit__(self, *args):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        self.returncode = self.proc.returncode
        self.out.close()
        self.err.close()


def metadata():
    """Controlled-run context; no app data, environment secrets, or cache contents."""
    result = {'kernel': os.uname().release, 'timestamp_ns': time.time_ns()}
    commands = {
        'gpu': ['nvidia-smi', '--query-gpu=name,driver_version', '--format=csv,noheader'],
        'power_profile': ['powerprofilesctl', 'get'],
        'renderer': ['systemctl', '--user', 'show', 'wd-venus.service', '-p', 'ExecStart', '-p', 'Environment'],
    }
    for key, command in commands.items():
        try:
            p = run(command, check=False)
            result[key] = p.stdout.strip() if p.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            result[key] = None
    if not result.get('power_profile'):
        governors = {p.read_text().strip() for p in Path('/sys/devices/system/cpu').glob('cpu*/cpufreq/scaling_governor')}
        result['power_profile'] = 'governors:' + ','.join(sorted(governors)) if governors else None
    return result


def resource_summary(output):
    import csv
    import re
    output = Path(output)
    rows = [json.loads(line) for line in (output/'resources.jsonl').read_text().splitlines() if line.strip()]
    cpu, memory, fds, threads = [], [], [], []
    previous = None
    for row in rows:
        if 'cpu.stat' in row:
            fields = dict(line.split() for line in row['cpu.stat'].splitlines())
            current = (row['monotonic_ns'], int(fields['usage_usec']))
            # A service restart resets its cumulative CPU counter.
            if previous and current[1] >= previous[1] and current[0] > previous[0]:
                cpu.append(100000 * (current[1]-previous[1])/(current[0]-previous[0]))
            previous = current
        if 'memory.current' in row:
            memory.append(int(row['memory.current']))
        if row.get('processes'):
            fds.append(sum(r['fds'] for r in row['processes']))
            threads.append(sum(int(r['threads']) for r in row['processes'] if r.get('threads')))
    gpu = []
    with (output/'gpu.csv').open() as file:
        for row in csv.DictReader(file):
            value = next((v for k,v in row.items() if k and k.strip().startswith('utilization.gpu')), '')
            if re.fullmatch(r'\s*\d+(?:\.\d+)?\s*%?\s*', value):
                gpu.append(float(value.strip().rstrip('%').strip()))
    def describe(values):
        return {'samples': len(values), 'min': min(values) if values else None,
                'max': max(values) if values else None, 'mean': sum(values)/len(values) if values else None,
                'first': values[0] if values else None, 'last': values[-1] if values else None}
    return {'cpu_core_percent': describe(cpu), 'gpu_percent': describe(gpu),
            'memory_bytes': describe(memory), 'fds': describe(fds), 'threads': describe(threads),
            'sampling_errors': sum('error' in r for r in rows),
            'complete': bool(cpu and gpu and memory and fds and threads)}
