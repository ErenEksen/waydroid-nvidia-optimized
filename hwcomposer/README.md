# hwcomposer — `hwcomposer.waydroid.so` from pinned source

The guest's composer HAL (`/vendor/lib64/hw/hwcomposer.waydroid.so`, loaded by
`android.hardware.graphics.composer@2.1-service`; the image loads no 32-bit composer) is built
here from Waydroid's hwcomposer at a pinned commit plus the patches in `patches/`, standalone with
the NDK (no AOSP tree). Nothing is binary-patched.

## Pins (`pins.env`)

| Input | Pin |
|---|---|
| hwcomposer | `waydroid/android_hardware_waydroid` `7750307152d2c1bc0f81ca0163c9253a7a82d8fa` (the baseline's HWC_BASE) |
| NDK | r29 (`14206865`), zip sha256 `4abbbcdc…` (what built the prebuilt: `.note.android.ident`) |
| AOSP headers | android-13-era commits of libhidl, libhwbinder, libfmq, fmtlib, frameworks/native, hardware/interfaces, system/tools/hidl, libbase, libhardware, system/core; LineageOS system_logging; waydroid minigbm `16844bab` (same values as the baseline's `packaging/ci/hwc-pins.env`) |
| Static guest libs | libffi 3.4.6, wayland 1.25.0 (+ native wayland-scanner), libxkbcommon 1.7.0, libdrm 2.4.133 headers; tarball sha256s in `pins.env` |
| Protocol XMLs | wayland-protocols 1.49 (regenerates the baseline's committed `gen/*.c|h` byte for byte) |
| Link stubs | the local Waydroid images (`images_path` in waydroid.cfg), else the pinned OTA zips |

## Build

```sh
hwcomposer/provision.sh          # once: NDK, clones, hidl-gen, static libs, protocols, link stubs (~10 min)
hwcomposer/build.sh              # -> build/hwcomposer-work/out/hwcomposer.waydroid.{so,stripped.so}
HWC_PATCHES_UPTO=0001 hwcomposer/build.sh build/hwcomposer-work/out-repro   # the reproduction alone
```

`prime-copy.cpp` (added by 0014) is compiled when the patched tree has it, so `HWC_PATCHES_UPTO`
below 0014 still builds. It loads the guest's `libvulkan.so` with `dlopen` only when the compositor
is on another GPU; the same-GPU path does not touch Vulkan.

`build.sh` applies the patches in a fresh git tree and fails if one does not apply or changes
nothing (a `git apply` in a non-repo directory inside another work tree silently skips every path).
`-ffile-prefix-map` and fixed xkbcommon config roots keep build-host paths out of the binary; with
the baseline's build paths mapped back in, `HWC_PATCHES_UPTO=0001` reproduces the baseline's
prebuilt hwcomposer byte for byte (checked in the development tree).

Glue copied from the baseline's former `build/hwcomposer/` (MIT; this tree replaced it): `hidl-gen/` (CMake for a standalone
hidl-gen), `libcxx_compat.cpp` and `libcxx-override/__config_site` (r29 libc++ headers vs the
image's libc++), `aidl-stubs/` (hand-written minimal declarations of the graphics.common AIDL
types that libui's headers include). `provision.sh`/`build.sh` are adapted from its
`build/hwcomposer/` (removed; this is now the only hwcomposer recipe, for all three driver modes).

## Patches

| # | What | Source |
|---|---|---|
| 0001 | Baseline WIP + issue #5 (decorations, resize as scaled view + display-mode ladder, clipboard, window-lifetime fix) | waydroid-nvidia (2026-07-28) |
| 0002 | `formats_mutex` around formats/modifiers | PR #20 `45d12b1` (The-JimmyDev) |
| 0003 | refuse LINEAR for formats the compositor imports through NVIDIA EGL (per format) | quinovax `f2c2f39` (`compositor_uses_nvidia_egl`), reworked |
| 0004 | keep the wl_buffer cache across geometry changes and hotplug; prune after 8 unused frames; dma-buf inode identity | quinovax `f2c2f39` + `ad04e0a`; identity ours |
| 0005 | no abort() on per-frame / unreachable conditions | quinovax `ad04e0a`, reworked |
| 0006 | lost Wayland connection: named cause, `waydroid.hwc.wayland_lost`, exit 75 | ours (replaces quinovax's frozen HAL) |
| 0007 | ignore SIGPIPE | quinovax `1608854`/`74f7b93` |
| 0008 | cursor buffer importability check | quinovax `ad04e0a` |
| 0009 | never shrink the HIDL thread pool | problem from quinovax `74f7b93`, fix ours |
| 0010 | wl_shm fallback for undisplayable layers | quinovax `f2c2f39` |
| 0011 | acquire-fence timeout 1 s, still presents completely | quinovax `f2c2f39` |
| 0012 | `wp_presentation` timestamps only on CLOCK_MONOTONIC; vsync arithmetic | quinovax `f2c2f39` |
| 0013 | viewport source clamped to the buffer | quinovax `f2c2f39`, rotation fix ours |
| 0014 | compositor on another GPU (hybrid iGPU, baseline #2): dmabuf v4 feedback decides same-GPU direct vs cross-GPU; cross-GPU copies the client target with Vulkan into system-memory LINEAR (pitch 256-aligned) and composes every other layer in SurfaceFlinger; `waydroid.hwc.present_path` | ours; recipe = NVIDIA driver's own cross-GPU present (observed) / egl-wayland2 `force_prime` |
| 0015 | YUV layers (Chrome/ExoPlayer video on a SurfaceControl, NV12 from the Codec2 decoder) are composed by SurfaceFlinger, never attached: hwc1 has no dataspace, and KWin 6.7/NVIDIA advertises NV12+LINEAR yet rejects that import (fatal error 7, exit 75, SF abort, soft reboot). Every dma-buf plane of the gralloc handle is passed and a plane-count mismatch is refused; wl_shm fallback fails loudly for formats without a wl_shm equivalent | ours |
| 0016 | never call `hwc_procs_t::hotplug`: `hwc_set()` sent hotplug(0, 1) after every xdg configure (resize, activation); the 20260927 vendor `libhwc2on1adapter` forwards primary hotplugs, the synchronous callback runs nested on SurfaceFlinger's main thread inside `presentAndGetReleaseFences`, SF recreates the display and dereferences the erased `DisplayData` (SEGV 2026-10-16; upstream waydroid/android_hardware_waydroid#84). `#pragma GCC poison hotplug` makes a new call from those files a build error; live check `dev/hwc-hotplug-check` (KDE Plasma) | ours |
| 0017 | the Android display follows the window (vanilla resize, upstream 40c54a5), safely: a settled window size (`persist.waydroid.resize_settle_ms`, default 250 ms as upstream's lineage-23.2 hwcomposer) becomes the display's only config and the hotplug is sent from hwc's own resize thread (`display-resize.cpp`, the only caller), never from a composer call. Needs a composer HAL that survives a primary reconnect with frames in flight; enabled only when the vendor image declares `ro.vendor.composer.safe_primary_reconnect=true` (our vendor image, with its `hardware/interfaces` patch). Without it nothing changes; the composer log says which path is active | ours |

Each patch header states what, why, source URL and author, licence and deviations. Not taken:
quinovax's multi-window and input hunks (content-frame origins, `prepare_layer_geom` co-clipping,
input clamping, solid parent buffer, subsurface neutralising, fake_touch/mouse_as_touch).

## Licences

android_hardware_waydroid is Apache-2.0; every patch here modifies it and is Apache-2.0
(the baseline's and the quinovax fork's `patches/hwcomposer/` inherit Apache-2.0 per their
LICENSE section 2; PR #20 was submitted to the baseline under the same terms). The build glue
from the baseline is MIT. Statically linked into the binary: wayland (MIT), libffi (MIT),
libxkbcommon (MIT/X11-style).
