`perf-v2`: this fork brought up to upstream waydroid-nvidia **v0.2.0**, three more Venus optimizations, and an Arch
Linux package built from source.

## From upstream (v0.2.0-rc12 → v0.2.0)

- **hwcomposer:** no more display freezes when opening or switching apps (#38: `setFocusedTask` no longer called
  under `windowsMutex`; window creation never waits for the Wayland thread); app windows resize Android without
  multi-window mode (#35); a failed EGL read-back is reported instead of showing an empty buffer (#36).
- **Venus mode:** real biplanar NV12 buffers for video and camera (#16); a per-app limit on how fast GPU buffers can
  be allocated (512 MiB/s, `VTEST_GPU_ALLOC_MB_PER_SEC`); a semaphore import waits briefly for an object the ring has
  not created yet instead of failing.
- **Default mode (nvidia-prolapse):** GPU crash (Xid 32) on RTX 20–50 and Titan V from the async compute queue
  fixed (indirect copies run as compute kernels).
- Not taken: the `rust-rewrite` branch (v0.3.0-dev1). Upstream marks it as a development build that has not reached
  its `main`; it moves the driver to a separate repository and changes only the default mode.

How the merge kept this fork's behaviour: a failed timeline import still marks the context failed instead of
reporting unfinished GPU work as done; the import retry keeps the fork's check that the semaphore belongs to the
device, fails at once on a wrong object, and does not hold the import lock while it waits. The fork's hwcomposer
patch is now `0021`, after upstream's `0018`–`0020`.

## New in this fork

- **⑧ Fence export in 2 ioctls** (kernel 6.16 or newer): the guest exports the timeline point directly instead of
  creating, filling, exporting and destroying a syncobj for every presented frame (5 ioctls). Older kernels fall
  back automatically.
- **⑨ One write per vtest command** for semaphore imports and timeline submits: one syscall and one renderer wakeup
  less each, same bytes on the wire.
- **⑩ 4 GiB private shader cache** for the Venus renderer.

## Arch Linux package

`packaging/arch/waydroid-nvidia-optimized/PKGBUILD` builds the Venus components from source with this tree's patches
and takes the vendor image, native bundle, ANGLE, hwcomposer and SurfaceFlinger from upstream's v0.2.0 release,
pinned by SHA-256. CI builds it in a clean Arch container, checks it, installs it with its dependencies and removes
it again.

```sh
sudo pacman -U waydroid-nvidia-optimized-0.2.0.perf2-1-x86_64.pkg.tar.zst
systemctl --user enable --now wd-venus.service
sudo waydroid-nvidia-setup --mode venus
```

## Testing

`tests/host` (new): the host allocator, the gralloc backend, and the guest/host code the Mesa and virglrenderer
patches add, built from this tree and run against a fake kernel, a mock Vulkan driver and a scripted vtest server
under ASan/UBSan. Each test was mutation-checked (deliberately broken variants fail). Live gameplay on NVIDIA
hardware was not part of this update's testing.
