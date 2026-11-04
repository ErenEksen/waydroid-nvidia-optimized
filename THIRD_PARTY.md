# Third-party sources

Everything below is fetched at a pinned revision by the named recipe; nothing third-party is vendored except
where the "Used in" column says so. The recipe file is authoritative for the exact version. Licences: [LICENSE](LICENSE).

| Source | Pinned version | Used in |
|---|---|---|
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) — NVK, NAK, nil, gallium | mesa-26.2.5 `ddaf85e3` | `nvidia-prolapse/driver-source/native/mesa/` (NVK on the Rust RM, `nvrm`), `native/compiler/`, `nvidia-prolapse/gralloc` (`MESA_REV`) |
| Mesa (Venus) | `a8ce4d82` | Venus guest driver, `patches/mesa/`, `build/mesa/` (`packaging/ci/pins.env`) |
| Mesa (ASTC CPU decoder, test reference only) | mesa-25.2.0 `0b97dcdd` | `nvidia-prolapse/hal` tests and LUT generation |
| Mesa CI libelf recipe | libelf-0.8.13 (md5 from Mesa's `.gitlab-ci/container/debian/android_build.sh`) | `native/mesa/android_libelf.py` (bionic libelf for NVK) |
| [virglrenderer](https://gitlab.freedesktop.org/virgl/virglrenderer) | `dc35e4db` | Venus host renderer, `patches/virglrenderer/` |
| [ANGLE](https://chromium.googlesource.com/angle/angle) | `c1a25085` | guest GLES for all modes, `build/angle/`, `angle/patches/` |
| [Waydroid](https://github.com/waydroid/waydroid) | `a33a5c0b` | `patches/waydroid/`, `waydroid/0001-lxc-nvidia-native-mounts.patch`, `dev/build-waydroid` |
| [android_hardware_waydroid](https://github.com/waydroid/android_hardware_waydroid) | `a3e42399` | `hwcomposer/` (+ `hwcomposer/patches/`) |
| [android_external_minigbm](https://github.com/waydroid/android_external_minigbm) (Waydroid fork) | `16844bab` | `nvidia-prolapse/gralloc` (+ `gralloc/minigbm/` patches), Venus gralloc, hwcomposer headers |
| [android_vendor_waydroid](https://github.com/waydroid/android_vendor_waydroid) | `dd9c6361` (lineage-20, 2026-04-02) | `nvidia-prolapse/media/vndk/build.sh` |
| [LineageOS](https://github.com/LineageOS/android) 20 + Waydroid device/vendor trees | manifest pinned to 2026-09-27 (`vendor-image/manifests/`) | `vendor-image/` (vendor image from source), `build/lineage-20/` (SurfaceFlinger) |
| Waydroid images (LineageOS 20, x86_64) | system GAPPS + vendor MAINLINE 20260927 (sha256 in `hwcomposer/pins.env`) | link stubs for hwcomposer/media/NVK, `vendor-image/` reference comparison |
| [WayDroid-ATV](https://github.com/WayDroid-ATV) (`waydroid-builds`, `android_vendor_waydroid`) | not used in builds | reference for vendor-module packaging and A14+ images (`vendor-image/notes.md`) |
| AOSP (`android.googlesource.com/platform/...`) | android-13 era commits: libhidl, libhwbinder, libfmq, fmtlib, frameworks/native, hardware/interfaces, system/tools/hidl, libbase, libhardware, system/core; frameworks/av `95be9bad` (tm-qpr3); libion, libdmabufheap (android-13.0.0_r79); LineageOS system_logging `373d54e7` | hwcomposer (`hwcomposer/pins.env`), Codec2 component (`nvidia-prolapse/media/vndk/build.sh`), HAL/EGL interface headers |
| [libdrm](https://gitlab.freedesktop.org/mesa/drm) | 2.4.134 (`nvidia-prolapse/libdrm`), 2.4.133 (hwcomposer headers) | bionic libdrm for the guest, hwcomposer |
| wayland 1.25.0, wayland-protocols 1.49, libffi 3.4.6, libxkbcommon 1.7.0 | sha256 in `hwcomposer/pins.env` | hwcomposer (static, bionic) |
| Android NDK | r29 (`packaging/ci/pins.env`, `hwcomposer/pins.env`); r30 for `nvidia-prolapse` shim/HAL defaults | all guest builds |
| [NVIDIA open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) headers | 615.71.09 `61dcc937` | RM ABI of `native/` (struct layouts via `tools/generate-compat.py`), `native/vendor/nvidia/` (MIT) |
| NVIDIA [egl-x11](https://github.com/NVIDIA/egl-x11), [eglexternalplatform](https://github.com/NVIDIA/eglexternalplatform), [libglvnd](https://gitlab.freedesktop.org/glvnd/libglvnd) | egl-x11 `328a5e1b` (v1.0.6-5, its `src/base` compiled in, Apache-2.0), eglexternalplatform `cf9f1058`, libglvnd `faa23f21`, frameworks/native `2fe7eddf` | `nvidia-prolapse/egl` (glibc-shim mode's NVIDIA GLES front and platform) |
| [ash](https://github.com/ash-rs/ash) | 0.38.0 | `native/` Vulkan ABI bindings (`native/licenses/`) |
| [tinygrad](https://github.com/tinygrad/tinygrad) NV backend | `37132d75` | cross-check of public direct-RM setup only (`native/src/rm.rs` comment, `native/licenses/`) |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) | `63bcd588` | VP9 default probability tables (`native/src/vp9_tables.rs`) |
| Mesa `ac_vcn_av1_default.h` (AMD, MIT) | mesa-25.2.0 `0b97dcd` | AV1 default CDFs (`native/src/video/av1/cdf_defaults.rs`) |
| Test media: Big Buck Bunny (© Blender Foundation, CC-BY 3.0), FFmpeg `testsrc2` | encoded with libaom, SVT-AV1, libx264, libx265, libvpx | `native/src/video/**/testdata/` (decoder tests) |
| [Magisk](https://github.com/topjohnwu/Magisk) (module format) | not shipped | `mcpe-vv-module/` is a Magisk module the user installs |
| [waydroid_script](https://github.com/casualsnek/waydroid_script) | not shipped | used by testers to install libhoudini (ARM translation); not called by this repo |
| [quinovax/waydroid-nvidia](https://github.com/quinovax/waydroid-nvidia) `packaging/install-from-release.sh` (fork) | main, read 2026-10-16 | ideas reused in `packaging/install.sh`: distro-family detection, per-family dependency lists, one SHA256SUMS line required per asset, absolute python interpreter for `waydroid.py` |
| [ErenEksen/waydroid-nvidia-optimized](https://github.com/ErenEksen/waydroid-nvidia-optimized) `install.sh` (fork) | main, read 2026-10-16 | idea reused in `packaging/install.sh`: verify-only `--check` before installing |
