"""Host perf capture of a game's GLThread; no sysctl/capability/service changes."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys

LIMITS = ('perf_event_paranoid', 'perf_event_max_sample_rate',
          'perf_cpu_time_max_percent', 'perf_event_mlock_kb')


def package_name(value):
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+', value):
        raise argparse.ArgumentTypeError('invalid Android package name')
    return value


def seconds(value):
    try:
        n = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError('seconds must be an integer')
    if not 1 <= n <= 60:
        raise argparse.ArgumentTypeError('seconds must be between 1 and 60')
    return n


def start_ticks(path):
    # Field 22, after the parenthesized comm (which may contain spaces).
    return int(path.read_text().rsplit(')', 1)[1].split()[19])


def find_target(package, proc=Path('/proc')):
    targets = []
    for path in proc.iterdir():
        if not path.name.isdecimal():
            continue
        try:
            name = (path/'cmdline').read_bytes().split(b'\0', 1)[0].decode().strip()
            if name != package:
                continue
            threads = []
            for task in (path/'task').iterdir():
                if (task/'comm').read_text().strip().startswith('GLThread '):
                    threads.append({'tid': int(task.name), 'start_ticks': start_ticks(task/'stat')})
            if threads:
                targets.append({'pid': int(path.name), 'start_ticks': start_ticks(path/'stat'),
                                'threads': sorted(threads, key=lambda t: t['tid'])})
        except (OSError, UnicodeError, ValueError, IndexError):
            continue  # Exited/inaccessible process; never attach based on a stale PID.
    if len(targets) != 1:
        raise RuntimeError(f'expected one running {package} process with GLThread, found {len(targets)}')
    return targets[0]


def perf_command(perf, args, deadline):
    # The privileged timeout bounds descendants too; an interrupted caller does
    # not leave a profiler running indefinitely. No shell or environment secrets.
    return ['sudo', '-n', '/usr/bin/timeout', '--signal=INT', '--kill-after=5s',
            str(deadline), '/usr/bin/env', 'PERF_CONFIG=/dev/null', 'DEBUGINFOD_URLS=',
            perf, '--no-pager', *args]


def record_args(target, duration):
    tids = ','.join(str(t['tid']) for t in target['threads'])
    return ['record', '--no-buildid-cache', '--namespaces', '-k', 'mono',
            '-F', '99', '-e', 'cpu-clock:u', '-t', tids, '-T', '--sample-cpu',
            '-m', '8', '--max-size', '16M', '-o', '-', '--', '/usr/bin/sleep', str(duration)]


def read_limits():
    return {name: (Path('/proc/sys/kernel')/name).read_text().strip() for name in LIMITS}


def checked(command, timeout=15):
    p = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=timeout)
    if p.returncode:
        raise RuntimeError(p.stderr.decode(errors='replace').strip() or f'command exited {p.returncode}')
    return p.stdout


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', required=True, type=package_name)
    parser.add_argument('--seconds', type=seconds, default=15)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args(argv)
    if os.geteuid() == 0:
        parser.error('run as your desktop user after sudo -v; do not sudo this script')
    created = False
    status = {'package': args.package, 'duration_requested_s': args.seconds,
              'capture_complete': False, 'sample_hz': 99, 'event': 'cpu-clock:u',
              'stack_memory_capture': False, 'performance_improvement_measured': False}
    out = args.output or Path(__file__).resolve().parents[2]/'.work/game-diagnostics'/datetime.now().strftime('%Y%m%d-%H%M%S-perf')
    try:
        perf = '/usr/bin/perf'
        if not os.access(perf, os.X_OK):
            raise RuntimeError('host perf is not installed')
        checked(['sudo', '-n', '/usr/bin/true'])
        target = find_target(args.package)
        status.update(target=target, limits_before=read_limits(), perf_version=checked([perf, 'version']).decode().strip())
        out.mkdir(mode=0o700, parents=True, exist_ok=False)
        created = True
        root = f"/proc/{target['pid']}/root"
        # These are mapping names/addresses, not process memory or app data.
        (out/'maps-before.txt').write_bytes(checked(['sudo', '-n', '/usr/bin/cat', f"/proc/{target['pid']}/maps"]))
        if find_target(args.package) != target:
            raise RuntimeError('game/thread identity changed before capture')
        print(f"Profiling only GLThread(s) {[t['tid'] for t in target['threads']]} for {args.seconds}s. Keep the problem scene active.", flush=True)
        command = perf_command(perf, record_args(target, args.seconds), args.seconds + 20)
        status['record_command'] = command
        # The unprivileged parent opens output. perf's streaming mode avoids
        # root-owned files and does not write a persistent build-id cache.
        with (out/'perf.data').open('wb') as data, (out/'record.stderr').open('wb') as err:
            p = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=data, stderr=err,
                               timeout=args.seconds + 30)
        if p.returncode:
            raise RuntimeError(f"perf record exited {p.returncode}; see {out/'record.stderr'}")
        if find_target(args.package) != target:
            raise RuntimeError('game/thread identity changed during capture; sample set is incomplete')
        reports = [
            ('libraries.txt', ['report', '-i', '-', '--stdio', '--stdio-color', 'never',
                               '--no-children', '-n', '--sort', 'comm,dso', '--percent-limit', '0']),
            ('symbols.txt', ['report', '-i', '-', '--stdio', '--stdio-color', 'never',
                             '--no-children', '-n', '--sort', 'comm,dso,symbol', '--percent-limit', '0.1']),
            ('samples.txt', ['script', '-i', '-', '--ns', '-F', 'comm,pid,tid,time,event,ip,sym,dso,period']),
        ]
        for filename, report in reports:
            with (out/'perf.data').open('rb') as data, (out/filename).open('wb') as dest, (out/(filename+'.stderr')).open('wb') as err:
                p = subprocess.run(perf_command(perf, report + ['--symfs', root], 45),
                                   stdin=data, stdout=dest, stderr=err, timeout=55)
            if p.returncode:
                raise RuntimeError(f'perf {filename} exited {p.returncode}; see saved stderr')
        status['sample_lines'] = sum(bool(line.strip()) and not line.startswith('#')
                                     for line in (out/'samples.txt').read_text().splitlines())
        if not status['sample_lines']:
            raise RuntimeError('no CPU samples; an empty profile is not success')
        status['capture_complete'] = True
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        status['error'] = str(exc)
        print(f'Profile incomplete: {exc}', file=sys.stderr)
        print('Use your own terminal after sudo -v; do not lower perf_event_paranoid or add NOPASSWD.', file=sys.stderr)
    finally:
        if created:
            try:
                status['limits_after'] = read_limits()
                status['limits_unchanged'] = status['limits_after'] == status['limits_before']
                if not status['limits_unchanged']:
                    print('Kernel perf limits changed during capture; inspect metadata. This helper did not write them.', file=sys.stderr)
            except OSError as exc:
                status['limits_error'] = str(exc)
            (out/'metadata.json').write_text(json.dumps(status, indent=2) + '\n')
            print(f'Output: {out.resolve()}')
    if status['capture_complete'] and status.get('limits_unchanged'):
        print('CPU profile captured. No game restart or performance patch applied.')
        return 0
    return 1
