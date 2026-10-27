# Waydroid NVIDIA Optimized

A self-contained, performance-focused fork of [Shiro836/waydroid-nvidia](https://github.com/Shiro836/waydroid-nvidia)
(GPU-accelerated [Waydroid](https://waydro.id) on NVIDIA cards). It aims for smoother frames and lower CPU overhead in
the **Venus** path, without lowering resolution, refresh rate or graphics quality, and installs on its own: no
upstream package is needed.

## What this fork does

**Venus synchronization and submit path** (guest Mesa, host virglrenderer, gralloc):

- Fences are exported once and shared through a DRM timeline syncobj, so a frame needs no socket round trip for
  them; waits on BufferQueue fences are forwarded to the host driver instead of being CPU-waited before submit.
- Ring submissions wait for consumption once per submit, and submit structures are reused instead of reallocated.
- Format/modifier enumeration is cached; per-buffer and per-frame logs are off unless asked for.
- Safer failure handling: bounded socket I/O (30 s), no endless EOF loops, no double-closed fds, no silent
  "already signalled" fallback.

**Renderer shader cache:** `wd-venus.service` keeps a private, persistent NVIDIA shader cache under
`~/.cache/waydroid-nvidia`, so shaders compiled once are reused across sessions.

**hwcomposer** (`hwcomposer/patches/0018`): each Wayland surface's opaque region is sent only when it changes, and
per-frame logs are opt-in (`debug.waydroid.hwc_buffers`, `debug.waydroid.hwc_frames`).

**Standalone installer:** `./install.sh` installs the whole stack from this checkout. Every binary it downloads is
pinned by SHA-256 in [`packaging/releases/`](packaging/releases/), and checksums are never taken from the download.

Details: [scope and acceptance criteria](docs/performance-smoothness.md) ·
[measurements](docs/performance-epic7-2026-09-19.md) · [fence design](docs/fence-timeline-design.md).

## Status

- Synchronization and fault regression tests pass, and both guest ABIs (x86, x86_64) build.
- Full gameplay acceptance is **still pending**: some games (Epic Seven) still stutter, and results vary by game and
  system.
- The optimized Venus binaries (`perf-v1`) are locally built and checksummed, **not CI-attested**.
- The hwcomposer change is in source only; the installed hwcomposer is still upstream's until a new overlay ships.

## Install

You need an NVIDIA GPU with `nvidia-drm.modeset=1`, a Wayland desktop on that GPU and Waydroid kernel support
(binder) on a systemd distribution. Venus mode needs the **open** NVIDIA kernel module and driver 580 or newer.
Remove an older `waydroid` or `waydroid-nvidia-bin` package first; the installer will not overwrite distro-owned
files. Android apps and data in `/var/lib/waydroid` are kept.

```sh
git clone https://github.com/ErenEksen/waydroid-nvidia-optimized.git && cd waydroid-nvidia-optimized
./install.sh                                # run as your desktop user; it asks for sudo
systemctl --user enable --now wd-venus.service
sudo waydroid-nvidia-setup --mode venus     # add -s GAPPS for Google apps on a fresh install
waydroid session start
```

Log out and back in once afterwards so the `/dev/udmabuf` permission rule applies. `./install.sh --check` only
downloads and verifies. To update, `git pull` and run `./install.sh` again. Run `sudo waydroid-nvidia-setup`
again after every NVIDIA driver update.

**Uninstall:** `sudo /usr/lib/waydroid-nvidia/install.sh --uninstall`.
`sudo waydroid-nvidia-setup --restore-official-vendor` returns to stock Waydroid. See
[installer options](docs/install-manual.md).

## Other driver modes

The install also includes upstream's other modes, unchanged: `nvidia-prolapse` (upstream's default, open-source
NVK-based driver with video decode) and `glibc-shim` (the host's NVIDIA driver inside Android). Switch with
`sudo waydroid-nvidia-setup --mode <mode>`. GPU/driver compatibility for them is documented upstream; this fork's
optimizations do not apply to them.

## Development

[Dev workflow](docs/dev-workflow.md) · [building](docs/building.md) · [architecture](docs/architecture.md) ·
[troubleshooting](docs/troubleshooting.md) · [third-party sources](THIRD_PARTY.md)

## Credits / license

Based on **Shiro836/waydroid-nvidia**, Mesa, virglrenderer and Waydroid. The prebuilt base (vendor image, native
driver bundle, ANGLE, SurfaceFlinger) is upstream's v0.2.0-rc12 build. Our code is [MIT](LICENSE); patches keep their
upstream licenses. Upstream's benchmark claims are not maintained or guaranteed by this fork.
