# Installer options, installed files, uninstall

`./install.sh` (which runs `packaging/install.sh` as root) installs the same files as the upstream AUR package plus
this fork's optimized Venus components, on any systemd distro. No other waydroid-nvidia installation is needed.

```sh
git clone https://github.com/ErenEksen/waydroid-nvidia-optimized.git && cd waydroid-nvidia-optimized
./install.sh
sudo waydroid-nvidia-setup
waydroid session start
```

| Option | Effect |
|---|---|
| `--check` | download and verify only (no root needed) |
| `--from DIR` | use the pinned files you already downloaded into `DIR` |
| `--no-deps` | do not install distro packages (the needed tools are still checked) |
| `--uninstall` | restore the official vendor image, then remove every installed file |

Every download is pinned in this tree: `packaging/releases/release.env` names the base release (upstream
waydroid-nvidia `v0.2.0-rc12`: vendor image, native driver bundle, guest and host stack) and the fork's Venus overlay,
`packaging/releases/SHA256SUMS` their hashes. The base files are fetched from this fork's mirror release, else from
the upstream release (identical bytes; a file that does not match its pinned hash is never used). The script also
checks the base assets' build provenance (with `cosign` 2.4.3 or newer, or a logged-in `gh`; without either it says
provenance was not checked) and that the native bundle was built from the pinned commit. It refuses to overwrite
files owned by a distro package. If `python3-gbinder` is missing, add the Waydroid repository first:
`curl -fsSL https://repo.waydro.id | sudo bash`.

Dependency lists for apt, dnf and pacman are in `packaging/install.sh`. On other distros install the equivalents
and pass `--no-deps`: lxc, python3 with gi/dbus/gbinder, nftables, dnsmasq, binutils, e2fsprogs, util-linux, zstd,
curl, git, make, patch, and for Venus mode libepoxy, libdrm, libgbm, libX11, expat and the Vulkan loader.

## What gets installed

| Path | What |
|---|---|
| `/usr/lib/waydroid`, `/usr/bin/waydroid` | Waydroid with our patches |
| `/usr/lib/waydroid-nvidia/vendor/` | our Android vendor image and its update channel |
| `/usr/lib/waydroid-nvidia/native/` | the driver bundle, its test programs and the setup tools |
| `/usr/lib/waydroid-nvidia/guest/`, `virgl_*` | ANGLE, hwcomposer and the Venus mode files |
| `/usr/bin/waydroid-nvidia-setup` | the setup command |

`waydroid-nvidia-setup` keeps your official vendor image in `/var/lib/waydroid-nvidia/vendor-image/official/`,
writes test logs to `/var/lib/waydroid-nvidia/gate-<time>/`, and backs up the previous `waydroid.cfg` to
`/var/lib/waydroid/before-vendor-image-<time>/`. Every container start checks that the running vendor image is the
one setup tested, on the loaded kernel module.

Android system updates (`waydroid upgrade`) stay official. Our vendor image changes only with this package.

## Uninstall

```sh
sudo /usr/lib/waydroid-nvidia/install.sh --uninstall      # Arch package: sudo pacman -R waydroid-nvidia-bin
sudo rm -rf /var/lib/waydroid-nvidia
```

Both put the official vendor image and a stock `waydroid.cfg` back first. If the saved official image was
deleted, that step stops and tells you to run `sudo waydroid init -f`. Installs of earlier release candidates may
also have left `/var/lib/waydroid/nvidia-source-native-*` and `/var/lib/waydroid/nvidia-prolapse`; you can delete
them.

## Verify a download

The installer does this for you. To check a release file by hand (releases from v0.2.0-rc2 on), use
`gh attestation verify <asset> --repo Shiro836/waydroid-nvidia`, or without a GitHub login:

```sh
COSIGN_MAX_ATTACHMENT_SIZE=300000000 cosign verify-blob-attestation \
  --bundle <asset>.sigstore.json --type slsaprovenance1 \
  --certificate-identity https://github.com/Shiro836/waydroid-nvidia/.github/workflows/build.yml@refs/tags/<tag> \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com <asset>
```
