# waydroid/: driver modes inside Waydroid

- `nvidia_native.py`: the mode rules, the LXC mount generator and the mode's Android properties. Installed into
  Waydroid as `tools/helpers/nvidia_native.py`.
- `0001-lxc-nvidia-native-mounts.patch`: hooks it into Waydroid (`lxc.py`, `images.py`, `session_manager.py`).
  Applies after `patches/waydroid/0001-nvidia-integration.patch` at Waydroid `a33a5c0`.
- `test_nvidia_native.py`: unit tests, no root. Run `dev/build-waydroid`, then
  `python3 -m pytest -q waydroid/test_nvidia_native.py`.

Check a config without changing anything:

```sh
./nvidia_native.py --cfg /var/lib/waydroid/waydroid.cfg --describe
```

## Configuration

```ini
[waydroid]
nvidia_driver_mode = nvidia-prolapse      # or glibc-shim, or venus
nvidia_native_gl = off                    # on: nvrm (nvidia-prolapse) or NVIDIA's GLES (glibc-shim)
nvidia_prolapse_install = vendor-image    # nvidia-prolapse only: vendor-image (users) or dev-root (developers)
```

`waydroid init` writes these defaults into a config that has no NVIDIA key yet. A config with no NVIDIA key at
container start means stock Waydroid. `waydroid init` installs the official vendor image, so in `vendor-image` it
leaves out the NVIDIA mounts and logs that `sudo waydroid-nvidia-setup` comes next; until the setup installed our
image, the container start refuses. Every key is explicit: nothing is guessed, and every refusal names the key
and the command that fixes it. Old names (`rust-driver`, the `nvidia_*_root` path keys) are refused with the new
name.

Don't put the mode's properties (`ro.hardware.vulkan`, `ro.hardware.egl`, `ro.zygote.disable_gl_preload`, the
Venus socket properties) in `[properties]`; the generator writes them and refuses overrides.

| Mode | What the generator mounts | `ro.hardware.vulkan` / `egl` |
|---|---|---|
| `nvidia-prolapse`, `vendor-image` | only the NVIDIA device nodes and the GPU's codec list. It first checks that `vendor.img` is the image setup tested, on the loaded kernel module | `nvidia` / `angle` or `nvrm` |
| `nvidia-prolapse`, `dev-root` | the staged root behind `/var/lib/waydroid/nvidia-prolapse` over the official vendor image | `nvidia` / `angle` or `nvrm` |
| `glibc-shim` | the root behind `/var/lib/waydroid/nvidia-glibc-shim` (shims, HAL, gralloc, libdrm) plus the host's NVIDIA libraries, 64- and 32-bit | `nvidia` / `angle` or `nvnative` |
| `venus` | the Venus socket and guest libraries from `/var/lib/waydroid/nv/guest`; `drm_device` must be the NVIDIA render node | `virtio` / `angle` |

The native modes set `ro.zygote.disable_gl_preload=true`; otherwise the zygote keeps `/dev/nvidia*` open and
system_server aborts at fork. `glibc-shim`, `venus` and `dev-root` need the official vendor image:
`sudo waydroid-nvidia-setup --restore-official-vendor`.

A missing piece fails `waydroid upgrade -o` before anything is written, naming the file and the remedy. The
container start logs `NVIDIA driver mode: ...`.

## Switching

```sh
dev/driver-mode status
dev/driver-mode set glibc-shim --native-gl on   # check, stop, apply, start
dev/driver-mode set nvidia-prolapse
```

`set` checks the new config first, then stops Waydroid, backs up `waydroid.cfg` to
`/var/lib/waydroid/before-source-native-<time>`, moves the shader caches aside when the driver changes, writes the
config, runs `waydroid upgrade -o` and starts again. Any failure restores the backup. Without the root helper,
run `sudo python3 dev/driver-mode apply MODE` with Waydroid stopped. It never stages a driver: use
`dev/source-native stage/activate` (nvidia-prolapse) or `dev/native-deploy` (glibc-shim, also after every host
driver update) first.

Native GL in `dev-root` needs an nvrm build in the staged root (`dev/source-native stage` adds it when
`build/nvrm-android*` exists and passes its smoke test); otherwise `--native-gl on` is refused.

## Hardware video decode

The default mode ships a Codec2 NVDEC decoder. Turn it off or on at runtime, no restart needed:

```sh
waydroid prop set persist.waydroid.nvidia_video_decode off   # software decoders
waydroid prop set persist.waydroid.nvidia_video_decode on    # NVDEC (default)
```

Use `waydroid prop set`, not `[properties]` (that entry is refused). Running apps pick up the change on their
next decoder; `logcat -s nvc2` shows the service's log.
