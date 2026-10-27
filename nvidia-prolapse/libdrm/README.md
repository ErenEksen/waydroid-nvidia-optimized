# libdrm/ — libdrm.so.2 for bionic, from pinned upstream

NVIDIA's Vulkan ICD (`libEGL_nvidia.so.0` and the libraries it loads) needs libdrm at runtime for
one feature this project cannot do without: **`VK_EXT_external_memory_dma_buf`**, which
`VK_ANDROID_native_buffer` (and with it `VK_KHR_swapchain` for every app) is built on.

## Why: the measured gating rule

The ICD advertises `VK_EXT_external_memory_dma_buf` only when **both**

1. `dlopen("libdrm.so.2")` succeeds in the ICD's linker namespace, and
2. the NVIDIA DRM render node is present.

Measured on the host (driver 615.71.09, RTX 5090) with `probe/vkcaps --icd libEGL_nvidia.so.0 --all-exts`:
with `libdrm.so.2` denied through the LD_AUDIT deny-list (`AUDIT_DENY=libdrm.so.2
LD_AUDIT=probe/libldaudit.so`) the device lists 290 extensions and dma_buf is missing; with it loadable,
291, dma_buf included. The one other difference in the report: without libdrm,
`VK_EXT_physical_device_drm` says `hasPrimary=0 hasRender=0` (with it: primary 226:2, render 226:129),
so the driver also uses libdrm to find its own DRM nodes. None of the driver's libraries has libdrm in
`DT_NEEDED`; the driver looks it up by its desktop SONAME at runtime and resolves the `drm*` entry
points itself.

Android has no file of that name: its own libdrm (upstream `Android.bp`, module `libdrm`) is built
unversioned, as `libdrm.so`. So inside the guest the ICD silently drops dma_buf, and the Vulkan HAL
(`hal/`) must refuse `vkCreateDevice` with `VK_ANDROID_native_buffer`.

## What

The upstream core `libdrm` (no vendor helper libraries, no tests, no docs), cross-built with NDK r30
for Android API 33 x86_64 against bionic, installed as the real file `libdrm.so.2` (`DT_SONAME
libdrm.so.2`, `DT_NEEDED` bionic `libc.so` only, linked `-z defs`). Waydroid integration copies it to
`/vendor_extra/lib64/libdrm.so.2`, next to the ICD and the glibc-compat shims, so the ICD's
`dlopen("libdrm.so.2")` finds it in the same sphal search path.

libdrm itself is a plain bionic library: its `drm*` entry points take ints, pointers and libdrm's own
structs, so it needs no shim between it and the glibc-linked driver.

## Pin

`https://gitlab.freedesktop.org/mesa/drm.git` tag `libdrm-2.4.134` = commit
`e984d448b8b17aab853369e6c203e53719f46de1`, the version of the host's own libdrm (Arch `libdrm
2.4.134-1`). Unmodified: `make` refuses a clone at another commit or with local changes.

Upstream's `meson.build` links an *unversioned* `libdrm.so` when it detects Android; the cross file
(`x86_64-android33.ini`) sets the SONAME to `libdrm.so.2` with a linker flag. No source change.

## Build and check

    make            # clone into ../../build/libdrm/drm if needed, meson + ninja, out/libdrm.so.2, check
    make check      # artifact check only
    make clean

`make check` (`check-libdrm.py`) reads the artifact with `llvm-readelf` and fails on any of:
- not ELF64 x86-64 `ET_DYN`, `DT_SONAME` other than `libdrm.so.2`, a `DT_NEEDED` outside
  `libc.so`/`libm.so`/`libdl.so` (a glibc SONAME is named as such), `TEXTREL`;
- any `drm*` name the driver's libraries reference that is not exported as `FUNC GLOBAL DEFAULT`: the names of
  every supported driver branch (`imports/<version>-<arch>.names`, cut from each branch's libraries), plus a census
  taken at check time from the host driver's libraries at the running kernel module's version (found through the
  loader cache by `../shim/tools/find-driver.py`), the way `strings -a | grep -E '^drm[A-Z][A-Za-z0-9_]+$'` sees
  them; 39 names for 535 and 550 (glsi 39, eglcore 5), 46 from 570 on (glsi 39, eglcore 12, EGL_nvidia/gpucomp/
  glvkspirv 0). `NO_BLOB_CENSUS=<reason>` skips only the host census (a builder without the driver);
- the exported `drm*` set differing from the host's `/usr/lib/libdrm.so.2` when the host has the
  pinned version (a different host version prints the difference instead).

The real proof is in the guest: the ICD lists `VK_EXT_external_memory_dma_buf` and the HAL's
`vkCreateDevice` with `VK_ANDROID_native_buffer` succeeds.
