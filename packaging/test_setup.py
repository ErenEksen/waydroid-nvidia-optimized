#!/usr/bin/env python3
"""Unit tests of waydroid-nvidia-setup's `waydroid init` step and -s/--system-type (no root, no Waydroid):
python3 -m pytest -q packaging/test_setup.py

The setup runs from a copy whose two path constants (waydroid.cfg, the package's lib dir) point into a temporary
directory, with stubs for `id` (root), `systemctl` and `waydroid` (records its arguments, then fails, so the setup
stops right after its init step) and no `readelf` (the setup's next check stops it when init is not needed)."""
from pathlib import Path
import os
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[1]
SETUP = REPO / 'packaging/aur/waydroid-nvidia-bin/waydroid-nvidia-setup'
CONSTANTS = {'CFG=/var/lib/waydroid/waydroid.cfg\n': 'CFG={cfg}\n', 'LIB=/usr/lib/waydroid-nvidia\n': 'LIB={lib}\n'}
OTA = 'https://ota.waydro.id/system/lineage/waydroid_x86_64/{}.json'


@pytest.fixture
def env(tmp_path):
    text = SETUP.read_text()
    cfg, lib = tmp_path / 'waydroid.cfg', tmp_path / 'lib'
    for old, new in CONSTANTS.items():
        assert text.count(old) == 1, old
        text = text.replace(old, new.format(cfg=cfg, lib=lib))
    script = tmp_path / 'setup'
    script.write_text(text)
    (lib / 'guest').mkdir(parents=True)
    bin_dir = tmp_path / 'bin'
    bin_dir.mkdir()
    calls = tmp_path / 'waydroid-calls'
    stubs = {'id': 'echo 0', 'systemctl': 'exit 3',
             'waydroid': f'printf "%s\\n" "$*" >> {calls}; exit 1'}
    for name, body in stubs.items():
        (bin_dir / name).write_text(f'#!/bin/sh\n{body}\n')
        (bin_dir / name).chmod(0o755)
    for tool in ('python3', 'basename', 'cat', 'awk'):
        os.symlink(shutil.which(tool), bin_dir / tool)

    def run(*argv, system_ota=None):
        if system_ota is not None:
            cfg.write_text(f'[waydroid]\nsystem_ota = {system_ota}\nimages_path = /var/lib/waydroid/images\n')
        out = subprocess.run([shutil.which('bash'), str(script), *argv], capture_output=True, text=True,
                             env={'PATH': str(bin_dir), 'LC_ALL': 'C'})
        made = calls.read_text().splitlines() if calls.exists() else []
        return out.returncode, out.stdout + out.stderr, made
    return run


def test_fresh_install_passes_the_system_type_to_init(env):
    code, out, calls = env('-s', 'GAPPS')
    assert calls == ['init -s GAPPS']
    assert "running 'waydroid init -s GAPPS'" in out
    assert 'waydroid init -s GAPPS failed' in out and code != 0


@pytest.mark.parametrize('flag', ['-s', '--system-type', '--system_type'])
def test_spellings_and_case(env, flag):
    _, _, calls = env(flag, 'gapps')
    assert calls == ['init -s GAPPS']


def test_fresh_install_without_s_runs_plain_init(env):
    _, out, calls = env()
    assert calls == ['init']
    assert 'sudo waydroid-nvidia-setup -s GAPPS' in out          # how to get Google apps instead


def test_unknown_system_type_is_refused(env):
    code, out, calls = env('-s', 'MICROG')
    assert code != 0 and calls == []
    assert 'takes VANILLA, GAPPS (with Google apps) or FOSS' in out


def test_initialized_with_another_type_says_how_to_switch(env):
    code, out, calls = env('-s', 'GAPPS', system_ota=OTA.format('VANILLA'))
    assert code != 0 and calls == []                             # never re-initializes on its own
    assert 'already initialized with the VANILLA system image' in out
    assert 'sudo waydroid init -f -s GAPPS' in out and 'apps and data are kept' in out
    assert 'run this setup without -s' in out


def test_initialized_with_the_same_type_goes_on(env):
    code, out, calls = env('-s', 'GAPPS', system_ota=OTA.format('GAPPS'))
    assert calls == []
    assert 'Waydroid is initialized with the GAPPS system image' in out
    assert 'readelf is required' in out                          # the next check after the init step


def test_preinstalled_images_have_no_system_type_to_pick(env):
    code, out, calls = env('-s', 'GAPPS', system_ota='None')
    assert code != 0 and calls == []
    assert 'images it does not download' in out


def test_initialized_without_s_does_not_check_the_type(env):
    _, out, calls = env(system_ota=OTA.format('VANILLA'))
    assert calls == [] and 'readelf is required' in out


def test_restore_takes_no_system_type(env):
    code, out, calls = env('--restore-official-vendor', '-s', 'GAPPS')
    assert code != 0 and calls == [] and 'takes no other option' in out


def test_help_names_the_option(env):
    code, out, _ = env('--help')
    assert code == 0 and '-s, --system-type VANILLA|GAPPS|FOSS' in out
