# ANGLE (guest GLES) — pinned source + our patches

The guest's GLES driver is ANGLE on Vulkan (`/vendor/lib{,64}/egl/lib{EGL,GLESv2,GLESv1_CM}_angle.so`),
built from upstream at a pinned commit plus the patches in `patches/`. Nothing is binary-patched.

- Upstream: `https://chromium.googlesource.com/angle/angle`
- Pin: `c1a25085dd9e4a8cd6c72be278c0b4bdf6ce2824` (same as `packaging/ci/pins.env`
  `ANGLE_SHA`; version string "ANGLE 2.1.28324 git hash: c1a25085dd9e" — the revision number is
  `git rev-list --count HEAD`, so fetch full history, not `--depth 1`)
- Unpatched, this recipe reproduces the baseline's attested v0.1.2 libraries byte for byte
  (x86_64 libGLESv2 `36218d44…`, x86 `395f7aef…`).

## Build

```sh
W=build/angle-work                  # gitignored work area (build/angle holds the tracked gn args)
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git $W/depot_tools
export PATH=$PWD/$W/depot_tools:$PATH DEPOT_TOOLS_UPDATE=0
git init $W/angle && cd $W/angle
git fetch https://chromium.googlesource.com/angle/angle c1a25085dd9e4a8cd6c72be278c0b4bdf6ce2824
git checkout FETCH_HEAD
python3 scripts/bootstrap.py && echo "target_os = ['android']" >> .gclient
gclient sync --no-history -j16
git apply ../../../angle/patches/*.patch
mkdir -p out/AndroidX64 out/AndroidX86      # args below
buildtools/linux64/gn gen out/AndroidX64 && buildtools/linux64/gn gen out/AndroidX86
third_party/ninja/ninja -C out/AndroidX64 libEGL libGLESv2 libGLESv1_CM
third_party/ninja/ninja -C out/AndroidX86 libEGL libGLESv2 libGLESv1_CM
```

`out/AndroidX64/args.gn` (x86 identical except `target_cpu = "x86"`, `android32_ndk_api_level = 30`):

```
target_os = "android"
target_cpu = "x64"
is_debug = false
is_component_build = false
angle_libs_suffix = "_angle"
android64_ndk_api_level = 30
angle_enable_swiftshader = false
```

Check: x86_64 outputs are ELF64 / "Advanced Micro Devices X86-64", x86 outputs ELF32 / "Intel 80386".
The patches change only `libGLESv2_angle.so`; libEGL and libGLESv1_CM stay byte-identical.

## Patch 0001: ReBAR memory for dynamic buffers on discrete GPUs

**Problem.** ANGLE gives `GL_DYNAMIC_DRAW`/`GL_STREAM_DRAW` buffers host-visible *system* memory, and
enables `preferDeviceLocalMemoryHostVisible` (the "DEVICE_LOCAL|HOST_VISIBLE heap covers ≥ 80 % of
VRAM" check) only for virtual GPUs, never discrete ones. Minecraft Bedrock's Vibrant Visuals
luminance histogram does global `atomicAdd`s into such a buffer, so on a discrete GPU every atomic
crosses PCIe. Unchanged on upstream main as of 34ffa963 (2026-10-09).

**Fix.**
1. `Renderer`: the existing ≥ 80 % heap check (`hasLargeHostVisibleDeviceLocalHeap()`) now also
   enables `preferDeviceLocalMemoryHostVisible` on discrete GPUs, so only with resizable BAR
   (a GPU with a 256 MiB BAR keeps today's behaviour). `mutableMipmapTextureUpload`, which shares the
   old helper and is off on discrete GPUs for VRAM-OOM reasons, is untouched.
2. `BufferVk`: with that feature on, DynamicDraw/StreamDraw get DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT
   (static usages already did); `preferHostCachedForNonStaticBufferUsage` keeps precedence;
   Read/Copy usages stay host-cached.
3. `BufferVk`, discrete GPUs only: CPU *reads* of BAR memory are uncached PCIe round trips (RTX 5090,
   host microbench: 0.084 GB/s memcpy vs 36–43 GB/s host-cached and 0.79 GB/s uncached sysmem; writes
   19 vs 22–27 GB/s). So non-persistent read maps of BAR buffers use the existing GPU-copy staging path
   (as device-local buffers did before), buffer-to-buffer copies from them run on the GPU, ghosting
   copies around the mapped range on the GPU, and `glBufferStorage(GL_MAP_READ_BIT)` buffers stay
   host-cached. Integrated/virtual GPUs keep their read paths.

**Measured** (RTX 5090, driver 615.71.09, Minecraft 1.26.60.24 VV main menu, 2560x1412, HAL timing off):

| | HUD fps | render / waitForPreviousFrame | GPU |
|---|---|---|---|
| pinned ANGLE | 59.4 | 16.8 / 16.3 ms | 113 W, SM 100 %, mem 2 % |
| + patch 0001 | 500.0 (vsync cap) | 1.9 / 1.5 ms | 184 W, SM 61 %, mem 8 % |

HAL `debug.nvhal.gputime=1`: the atomic histogram dispatch went from 13.2 ms/frame with its storage
buffer in memory type 2 (system RAM) to 0.031 ms/frame, with no storage buffer left outside device-local
memory; GPU busy per frame 17.1 → 0.86 ms. Regression: dEQP-GLES3.functional.buffer.* 695/695,
dEQP-GLES31.functional.ssbo.* 2061/2061, compute.* 195/195, atomic_counter.* 298/298 pass; Fancy 500 fps;
gpuload 1440p 7392 (was 7265).

Upstream candidate (no existing upstream bug or CL found; related history: 3b38b37907 added the
discrete exclusion, 5ddf631cc1 the heap-size check for virtual GPUs).

## Patch 0002: tell Mesa NVK apart from the NVIDIA proprietary driver

**Problem.** NVK reports NVIDIA's vendor ID, so `Renderer::initFeatures()` treated it as the proprietary
driver and parsed its Mesa version (26.2.5) as NVIDIA driver "26.x": every workaround for NVIDIA drivers
older than 421/441/470/525/531 applied. The costly one is `externallySynchronizePipelineCacheAccess`
(NVIDIA < 441): ANGLE's link-time pipeline warm-up ran one `vkCreateGraphicsPipelines` at a time under a
renderer mutex, and the first draw waited for it. Unchanged on upstream main 7865acacb.

**Fix.** As the PanVK split (ba5db43c1): NVK is identified by `VK_DRIVER_ID_MESA_NVK`;
`isNvidiaProprietary = isNvidia && !isMesaNVK` gates the driver-bug workarounds and NVIDIA version
checks, NVK's version is parsed as Mesa. Hardware-related conditions keep `isNvidia`.

| feature | NVK before | NVK after | reason (vk_renderer.cpp lines at the pin + 0001) |
|---|---|---|---|
| driverVersion parse | NVIDIA layout (26.0.x) | Mesa (26.2.5) | :5533 |
| externallySynchronizePipelineCacheAccess | on | off | NVIDIA < 441 (:6575-6578); NVK's VkPipelineCache is internally synchronized |
| supportsGraphicsPipelineLibrary | off | on | NVIDIA < 531 (:6489-6492); NVK exposes graphicsPipelineLibrary |
| forceWaitForSubmissionToCompleteForQueryResult | on | off | NVIDIA < 470 query bug (:6706-6710) |
| supportsSPIRV14 | off | on | NVIDIA < 525 (:6270-6275) |
| clampPointSize | on | off | NVIDIA < 421 (:5638-5643) |
| supportsDynamicRenderingLocalRead | off | on | NVIDIA driver bug with attachment location gaps (:6771-6780) |
| disableDepthStencilResolveThroughAttachment | on | off | NVIDIA driver bug anglebug.com/42267095 (:5818-5824) |
| avoidOpSelectWithMismatchingRelaxedPrecision | off | off | NVIDIA 535-551 (:5645-5648) |
| avoidInvisibleWindowSwapchainRecreate | off | off | NVIDIA on X11 only (:5892-5896) |
| swapchainDeferredMemoryAllocation | unchanged | unchanged | NVIDIA on Wayland only (:6153-6156) |
| isImmediateModeRenderer | IMR | IMR | GPU architecture (:5568-5569), kept |
| clampFragDepth | off | off | kept; only without depthClampZeroOne, which NVK has (:5657-5658) |
| allowGenerateMipmapWithCompute | on | on | kept (:6007-6010) |
| doubleDepthBiasConstantFactor | on | on | kept (:6718-6719); the list holds other vendors' Mesa drivers too |
| preferGlobalPipelineCache | on | on | kept (:6552-6553) |
| skipPipelineCacheSerialization | on | on | kept (:6529); NVK keeps its own disk cache, a blob-cache copy would duplicate it |

GL renderer string: the driver version now reads `NVK-26.2.5`.

**Measured** (RTX 5090, NVK 26.2.5 with the per-application disk cache, Minecraft 1.26.60.24, first hotbar
switch, longest present-to-present interval of the app's layer; cold = empty NVK and EGL blob caches):

| | cold | warm relaunch |
|---|---|---|
| pinned ANGLE + 0001, NVK without disk cache | 1542 ms | 1504 ms |
| + 0002, NVK with disk cache | 206 / 228 ms | 134 / 134 ms |
