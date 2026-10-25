# waydroid-nvidia

GPU-accelerated [Waydroid](https://waydro.id) on NVIDIA cards. Android apps and games render on your NVIDIA GPU
inside the normal Waydroid container, with no VM. The default mode uses only the NVIDIA kernel module from your
system; the rest of the driver is built from source.

This `public-test` branch is the next version and replaces `main` once testers confirm it works.
Please report problems as issues titled `[public-test] ...` ([what to include](docs/troubleshooting.md#reporting-a-problem)).

## Why switch from Venus

- **Speed:** in 3DMark Wild Life Extreme on an RTX 5090, the default mode runs at 475.4 FPS and NVIDIA's own driver
  (`glibc-shim`) at 507.7 FPS. Venus scored 59.8k against our 72.5k on the same card in an earlier build, because
  its socket can't feed a GPU that fast. On an RTX 5070 Ti, a user measured Venus ahead of us
- **No shader stutter:** Minecraft's first item switch ~0.2 s (Venus: 1.8 s)
- **Hardware video decode:** H.264, HEVC, VP9, AV1 (RTX 20 and newer)
- **Closed kernel module works too.** GTX 10 works as well; GTX 900 and Titan V are in testing

## Does my setup work?

✅ works in Waydroid · 🧪 driver tests pass on a rented machine, Waydroid not tried · ⚠️ known bug · ⚪ untested · ❌ no

| | default (`nvidia-prolapse`) | `glibc-shim` | `venus` |
|---|---|---|---|
| RTX 50 (Blackwell) | ✅ | ✅ | ✅ |
| RTX 40 (Ada) | 🧪 | ✅ | ✅ |
| RTX 30 (Ampere) | ✅ | ✅ | ✅ |
| RTX 20 / GTX 16 (Turing) | 🧪 | ⚪ | ✅ |
| Titan V (Volta) | 🧪 | ⚪ | ❌ |
| GTX 10 (Pascal) | ✅ (⚠️ with `--native-gl on`, some texture formats crash the GPU) | ⚪ | ❌ |
| GTX 900 (Maxwell 2) | ⚠️ some GL tests fail | ⚪ | ❌ |
| GTX 750 / 750 Ti, GTX 950M / 960M (Maxwell 1) | ⚪ | ⚪ | ❌ |
| Older GPUs (Kepler and before: their last driver is 470) | ❌ | ❌ | ❌ |
| Driver 615, open kernel module | ✅ | ✅ | ✅ |
| Driver 580–610, open module | 🧪 | ⚪ | ✅ |
| Driver 570, open module | 🧪 | ✅ | ⚪ |
| Driver 580, closed module | ✅ | ✅ | ❌ |
| Driver 550–610, other closed releases | 🧪 (550.107 is refused) | ✅ on 550, 570 and 595, ⚪ on the rest | ❌ |
| Driver 535 | ⚪ | ❌ (no sync_file fences) | ⚪ |
| Driver 470 and older | ❌ | ❌ | ❌ |

Apart from RTX 50 and driver 615, the `glibc-shim` results come from rented machines with no screen attached: the
setup passed, Android booted with its compositor on the GPU, and our in-Android tests passed. Nobody has used apps
there yet. Data-centre GPUs (A100, H100, P100, V100, B200) are out of scope.

| Distribution | Status |
|---|---|
| Arch Linux, CachyOS | ✅ |
| Ubuntu 22.04 | ✅ in `glibc-shim` on rented machines, default mode ⚪ |
| Fedora 44 | installer tested in a VM without an NVIDIA GPU, ⚪ with one |
| Debian and others | ⚪ |

Only the official Android 13 image (LineageOS 20) is supported. Hardware video decode (H.264, HEVC, VP9, AV1)
works in the default mode on RTX 20 and newer. On older GPUs the setup says so and Android's software decoders
play video instead.

You also need `nvidia-drm.modeset=1`, a Wayland desktop running on the NVIDIA GPU (tested on KDE Plasma 6), and
the usual Waydroid kernel support (binder). The default mode needs an entry for your exact driver release in its
version table, which ships with each of our releases; if yours has none yet, the setup says so.

## Install

Current release: [v0.2.0-rc12](https://github.com/Shiro836/waydroid-nvidia/releases/tag/v0.2.0-rc12) (prerelease).
The installer is for any systemd distribution. On Arch use it too: the AUR package still has the previous release.

```sh
curl -fsSL https://raw.githubusercontent.com/Shiro836/waydroid-nvidia/public-test/packaging/install.sh | sudo bash
sudo waydroid-nvidia-setup
waydroid session start
```

The installer verifies every download and will not overwrite files owned by a distro package, so remove an older
`waydroid` or `waydroid-nvidia-bin` package first. On SELinux hosts (Fedora) it also builds and loads Waydroid's
SELinux policy module, the one Fedora's own `waydroid-selinux` package ships. `waydroid-nvidia-setup` checks your
system, tests the driver on your GPU, and only then installs our Android vendor image. Your official vendor image is
kept as a backup. If a check fails, it says why and leaves everything as it was.

On a fresh install the setup runs `waydroid init` for you. For Google apps, use `sudo waydroid-nvidia-setup -s GAPPS`
instead (the default is `VANILLA`, without them). On an existing install `-s` only checks the system type; to switch,
run `sudo waydroid init -f -s GAPPS`, then `sudo waydroid-nvidia-setup` again (apps and data are kept).

Run `sudo waydroid-nvidia-setup` again after every NVIDIA driver or package update; Waydroid refuses to start until
you do. `sudo waydroid-nvidia-setup --restore-official-vendor` takes you back to stock Waydroid. Uninstalling is
described in [docs/install-manual.md](docs/install-manual.md#uninstall).

## Driver modes

| Mode | How it works | Pick it when |
|---|---|---|
| `nvidia-prolapse` (default) | Our open-source driver (Mesa NVK plus our own NVIDIA backend) runs inside Android and talks to the kernel module directly | almost always: it has video decode and the widest driver support |
| `venus` | Android's Vulkan calls go over a socket to the NVIDIA driver on the host | the default mode fails on your machine; slowest, needs the open kernel module |
| `glibc-shim` | Your host's own NVIDIA driver (550 or newer) runs inside Android through our compatibility libraries | you want NVIDIA's own driver and shader compiler; the setup checks your driver first and needs its 32-bit libraries too |

Switch with `sudo waydroid-nvidia-setup --mode venus` (or `--mode glibc-shim`, `--mode nvidia-prolapse`). Apps get
OpenGL ES through ANGLE by default. `--native-gl on` uses the mode's own GL driver instead: our `nvrm` in the
default mode (a bit faster but newer, so compare with `--native-gl off` before reporting a GL bug), NVIDIA's GLES in
`glibc-shim` (driver 570 or newer). In `glibc-shim`, run the setup again after every driver update, as in the
default mode.

## Known issues

- With `--native-gl on`, GTA San Andreas sometimes shows black frames.
- In `glibc-shim` on drivers 550–560 and 580, two bugs in NVIDIA's driver are worked around by waiting on the CPU
  until the compositor releases a buffer before an app gets it back. That costs some frame latency; how much is not
  measured yet.
- Above about 333 Hz the display can stall. Cap the rate with `sudo waydroid-nvidia-setup --refresh <hz>`.
- Android 14 and 15 have no official Waydroid images and are untested.

## More

[Troubleshooting](docs/troubleshooting.md) · [installer options and uninstall](docs/install-manual.md) ·
[how it works](docs/architecture.md) · [building](docs/building.md) · [developing](docs/dev-setup.md) ·
[third-party sources](THIRD_PARTY.md)

Our code is MIT licensed. Patches to other projects keep their upstream licenses; see [LICENSE](LICENSE).
