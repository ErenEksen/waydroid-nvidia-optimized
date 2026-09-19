import argparse
import contextlib
import io
import json
from types import SimpleNamespace
from unittest.mock import patch
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'dev'))
from perf import game_profile
from perf.game_profile import find_target, package_name, perf_command, record_args, seconds


class GameProfileTests(unittest.TestCase):
    def test_input_validation(self):
        self.assertEqual(package_name('com.stove.epic7.google'), 'com.stove.epic7.google')
        for value in ('com.a;id', 'com.a$(id)', 'com.a\n', '-p', ''):
            with self.assertRaises(argparse.ArgumentTypeError):
                package_name(value)
        self.assertEqual(seconds('15'), 15)
        for value in ('0', '-1', '61', '1.5', 'NaN'):
            with self.assertRaises(argparse.ArgumentTypeError):
                seconds(value)

    def make_process(self, root, pid, package, tid=None, ticks=42):
        path = root/str(pid)
        path.mkdir()
        (path/'cmdline').write_bytes(package.encode()+b'   \0')
        fields = ['0']*50
        fields[19] = str(ticks)
        (path/'stat').write_text(f'{pid} (game with spaces) '+' '.join(fields))
        (path/'task').mkdir()
        if tid:
            task = path/'task'/str(tid)
            task.mkdir()
            (task/'comm').write_text('GLThread 93\n')
            (task/'stat').write_text(f'{tid} (GLThread 93) '+' '.join(fields))

    def test_only_main_game_render_thread_is_selected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_process(root, 10, 'com.example.game', 11)
            self.make_process(root, 20, 'com.example.game')  # Translation helper, no GLThread.
            self.make_process(root, 30, 'com.unrelated.game', 31)
            self.assertEqual(find_target('com.example.game', root),
                             {'pid': 10, 'start_ticks': 42, 'threads': [{'tid': 11, 'start_ticks': 42}]})

    def test_missing_or_ambiguous_targets_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaises(RuntimeError):
                find_target('com.example.game', root)
            self.make_process(root, 10, 'com.example.game', 11)
            self.make_process(root, 20, 'com.example.game', 21)
            with self.assertRaises(RuntimeError):
                find_target('com.example.game', root)

    def test_record_is_bounded_userspace_only_and_does_not_dump_stacks(self):
        args = record_args({'threads': [{'tid': 11}]}, 15)
        self.assertEqual(args[args.index('-t')+1], '11')
        self.assertEqual(args[args.index('-e')+1], 'cpu-clock:u')
        self.assertEqual(args[args.index('-F')+1], '99')
        self.assertEqual(args[args.index('-o')+1], '-')
        self.assertIn('--no-buildid-cache', args)
        self.assertEqual(args[-3:], ['--', '/usr/bin/sleep', '15'])
        for forbidden in ('-a', '-g', '--call-graph'):
            self.assertNotIn(forbidden, args)

    def test_root_command_has_timeout_no_shell_and_no_external_symbols(self):
        args = perf_command('/usr/bin/perf', ['record', '-o', '-'], 35)
        self.assertEqual(args[:3], ['sudo', '-n', '/usr/bin/timeout'])
        self.assertIn('DEBUGINFOD_URLS=', args)
        self.assertIn('PERF_CONFIG=/dev/null', args)
        self.assertNotIn('sh', args)
        self.assertNotIn('sysctl', args)

    def invoke(self, output, runner, checked_error=None):
        target = {'pid': 10, 'start_ticks': 42, 'threads': [{'tid': 11, 'start_ticks': 42}]}
        with patch.object(game_profile.os, 'geteuid', return_value=1000), \
             patch.object(game_profile.os, 'access', return_value=True), \
             patch.object(game_profile, 'checked', side_effect=checked_error, return_value=b'test'), \
             patch.object(game_profile, 'find_target', return_value=target), \
             patch.object(game_profile, 'read_limits', return_value={'perf_event_paranoid': '2'}), \
             patch.object(game_profile.subprocess, 'run', side_effect=runner), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return game_profile.main(['--package', 'com.example.game', '--output', str(output)])

    def test_no_authenticated_root_means_no_capture_or_output(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)/'capture'
            def forbidden(*args, **kwargs):
                self.fail('must not start capture without authentication')
            self.assertEqual(self.invoke(output, forbidden, RuntimeError('password required')), 1)
            self.assertFalse(output.exists())

    def test_existing_output_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)/'existing'
            output.mkdir()
            (output/'metadata.json').write_text('preserve')
            self.assertEqual(self.invoke(output, None), 1)
            self.assertEqual((output/'metadata.json').read_text(), 'preserve')

    def test_capture_pipeline_preserves_raw_data_and_does_not_accept_empty_samples(self):
        for samples in (b'GLThread 10 11 1.0 cpu-clock:u: sample\n', b''):
            with self.subTest(samples=bool(samples)), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp)/'capture'
                def runner(command, **kwargs):
                    data = b'raw-perf-stream' if 'record' in command else (samples if 'script' in command else b'report')
                    kwargs['stdout'].write(data)
                    return SimpleNamespace(returncode=0)
                self.assertEqual(self.invoke(output, runner), 0 if samples else 1)
                status = json.loads((output/'metadata.json').read_text())
                self.assertEqual(status['capture_complete'], bool(samples))
                self.assertEqual((output/'perf.data').read_bytes(), b'raw-perf-stream')
                self.assertTrue(status['limits_unchanged'])
