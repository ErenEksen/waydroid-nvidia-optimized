# Waydroid NVIDIA Optimized

**Compared with [Shiro836/waydroid-nvidia](https://github.com/Shiro836/waydroid-nvidia):**
Performance optimizations for smoother desktop animations and lower CPU usage,
without reducing graphics quality or refresh rate. Includes easy installation
and rollback. Some games may still stutter; improvements vary by game and system.

## Install / update (existing NVIDIA installation)

Run as your **normal desktop user**, not root:

```sh
git clone https://github.com/ErenEksen/waydroid-nvidia-optimized.git && cd waydroid-nvidia-optimized && ./install.sh
```

Already cloned? `git pull --ff-only && ./install.sh`.
The installer downloads the pinned `perf-v1` bundle, verifies SHA-256 and every
component, asks for sudo, backs up the old set, and restarts Waydroid. **Save your
game first.** It is a persistent update: launch Waydroid normally afterward.
Apps, saves and Android images are not erased. `./install.sh --check` only downloads
and validates; it does not install or restart anything.

**Prerequisite:** an initialized upstream **waydroid-nvidia** installation using
`wd-venus.service` and `waydroid-container.service`. Stock Waydroid alone is not
sufficient. On Arch/CachyOS, first follow the upstream installation:

```sh
yay -S waydroid-nvidia-bin
# Only for a fresh installation; do not reinitialize existing Android data:
waydroid init
sudo waydroid-nvidia-setup
sudo systemctl enable --now waydroid-container.service
systemctl --user enable --now wd-venus.service
```

The AUR package above is the **upstream base**, not this fork's optimized release.
Then run this fork's installer. It preserves the existing display configuration.
The prebuilt host binaries require glibc **2.38+** and the upstream renderer
runtime libraries (initial validation: Arch/CachyOS). First-time upstream setup,
NVIDIA open kernel modules, a compatible NVIDIA driver,
Wayland, binder and working DMA-BUF sharing are still required. Hybrid/iGPU-driven
compositors retain upstream limitations. See [manual installation](docs/install-manual.md)
and [troubleshooting](docs/troubleshooting.md). Other installation layouts need manual
review; the updater refuses custom renderer paths.

### Rollback

The installer prints the exact backup directory. From this checkout, use it as follows:

```sh
sudo -v && ./dev/install-bundle --rollback /var/lib/waydroid/nv/perf-backups/EXACT-BACKUP
```

A failed post-install health check attempts to restore the previous complete set.
Package upgrades or upstream setup can overwrite these optimizations; reapply afterward.
No global governor, real-time priority, sysctl or passwordless-sudo rules are installed.

## What's shipped / validation

The bundle updates both x86 and x86_64 guest Mesa drivers, the host renderer,
gralloc backend, HWC and a user-service cache override. It **reuses** the upstream
Waydroid Python integration, ANGLE and SurfaceFlinger; it is not a complete Android image.
`perf-v1` contains locally built binaries with checksums, **not CI/SLSA-attested binaries**.
Source patches and pinned build recipes are in this repository; binaries live in Releases,
not Git. Native synchronization/fault tests and both guest builds passed; full Android
gameplay/desktop acceptance is still pending.

- [Measured results and remaining Epic Seven stutter](docs/performance-epic7-2026-09-19.md)
- [Performance scope and acceptance criteria](docs/performance-smoothness.md)
- [Build and development workflow](docs/dev-workflow.md)
- [Architecture](docs/architecture.md) · [Building](docs/building.md)

## Credits / license

Based on **Shiro836/waydroid-nvidia**, Mesa Venus, virglrenderer and Waydroid.
Original project code is [MIT](LICENSE); patches retain their respective upstream
licenses. The upstream AUR/Nix packages and upstream benchmark claims are not
maintained or independently guaranteed by this fork.
