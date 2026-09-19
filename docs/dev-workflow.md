# Development workflow

Use a disposable `.work/` directory and keep the downloaded original intact.
`dev/env.sh` defaults to the packaged `/usr/bin/waydroid` and
`waydroid-container.service`. `WAYDROID_SRC` is an optional explicit checkout,
not a prerequisite. There is no required passwordless-sudo configuration.

## Reproducible build inputs

```sh
dev/bootstrap sources           # exact upstream SHAs, local edits never overwritten
dev/bootstrap ndk               # pinned NDK archive + SHA256 verification
dev/bootstrap python            # pinned Python build dependencies
source dev/env.sh
PATH="$WNV/venv/bin:$PATH" build/hwcomposer/provision.sh "$WNV" "$HWC_TREE"
# ROOTFS must contain compatible image link libraries; see building.md.
# A read-only extraction may be supplied instead of starting/mounting Android.
BUILD_PROFILE=release JOBS=12 dev/build all
BUILD_PROFILE=release dev/build mesa-host   # native test ICD, never installed in Android
```

`release` and `diagnostic` select Meson's `release` and `debugoptimized` with
`b_ndebug=if-release`. Use **separate build directories** for profiling if you
need to retain both artifacts. Default jobs are capped at 12, with no changes
to host priority/governors. ANGLE and SurfaceFlinger are not rebuilt by `all`.
The host renderer already used release builds; this is not a standalone FPS fix.

The build virtualenv is selected only by `dev/build` (or by the explicit,
command-local `PATH` in the provisioning example). Sourcing `dev/env.sh` does
not activate it. Runtime commands must use a Python installation that has the
system Waydroid dependencies, including `dbus` and PyGObject; do not activate
`.work/venv` to install or run Waydroid. The installer checks `waydroid --help`
before authentication/service changes and refuses to continue if the CLI fails.
An offline `--check` validates only the bundle, not these runtime dependencies.

`src/` is canonical for the allocator and guest wrapper. Edit Mesa, renderer,
and HWC in their upstream working trees, then regenerate patches:

```sh
dev/sync-patches
python3 tests/check-patches.py
python3 -m unittest discover -s tests/python -v
SYNC_ROUNDS=1000 tests/run-sync-regression.sh
python3 tests/run-fault-regression.py
```

The tests start **private temporary renderer sockets**, never the installed
service. A native Vulkan loader/GPU is needed for integration tests. Fault tests
use a test-only forwarding Vulkan-loader shim; it must never be installed.
A test timeout, absent GPU, or absent measurement is not a pass.

## Candidate installation and rollback

Read `performance-smoothness.md` and capture the installed baseline **first**.
The original `dev/deploy virgl` merely restarted the installed daemon; it now
refuses this misleading operation. For this coupled guest/host change, use a
complete set rather than component-at-a-time replacement:

```sh
dev/make-bundle .work/artifacts/candidate
dev/install-bundle --check .work/artifacts/candidate  # no writes to installed paths
sudo -v                                           # your terminal, never share a password
dev/install-bundle --apply .work/artifacts/candidate
# On demand, use the exact backup path printed by the installer:
dev/install-bundle --rollback /var/lib/waydroid/nv/perf-backups/EXACT_BACKUP_NAME
```

The user-level coordinator stops session → container → renderer. The privileged
helper refuses a running LXC container, wrong ELF ABI, incomplete component set,
checksum mismatch, wrong layout/mount, or symlink destination. It snapshots the
previous set (including the changed cache override) into a root-owned 0700
backup directory, then atomically replaces individual files while everything
is stopped. `waydroid.cfg` and `config_nodes` are copied for context, not changed.
A failed file transaction restores the old files; a failed post-restart health
check triggers restoration of the old complete set. If shutdown/rollback itself
fails, it reports the backup path and leaves recovery to the operator rather
than claiming success. A power loss/SIGKILL can require explicit `--rollback`.

No image, app database, app cache, user content, driver, kernel, sudoers, global
graphics environment, resolution, refresh setting or animation scale is changed.
The package manager still owns the binary paths; a later package upgrade can
replace this local candidate. Root installation has unit-tested file primitives
but must also be validated on the actual running system.

## Measurements and diagnostics

```sh
dev/present-probe --build-only
sudo -v
dev/bench-startup --package com.android.settings --mode app-cold --runs 10 \
  --desktop-output eDP-1 --output .work/reports/before-app-cold
dev/cache-audit --guest > .work/reports/cache-metadata.json
dev/health
dev/status
dev/logs units
```

`dev/measure` is now an alias of `dev/bench-startup` (same required arguments),
not the old hardcoded-coordinate/warm-up benchmark. `dev/gamebench` remains an
**offscreen steady-state GLES workload**, not evidence for cold-start smoothness.
`dev/iter none --output DIR --desktop-output OUTPUT` is an explicit restart/
health/measurement shortcut. It refuses unbacked per-component deployment.

Detailed capture contract, limitations, comparison gates and current test results
are in `performance-smoothness.md` and `performance-validation-2026-09-19.md`.

### Venus submit-pool regression

`python3 tests/test-ring-submit-pool.py` extracts the actual pool and retirement
functions from the patched Mesa tree and tests them with ASan/UBSan/LSan. Run
outside ptrace-restricted sandboxes if LeakSanitizer refuses to operate; do not
silence a failed sanitizer run. `python3 tests/bench-ring-submit-pool.py` compares
actual pre-WIP (`HEAD`) and patched bookkeeping on a mixed zero/one-reference
workload. Its times are not game FPS or physical presentation measurements.
