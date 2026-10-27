# Foreign-fence latency probe

`native-vulkan-fence-latency.c` measures a real foreign Linux `sync_file` release
to completion of a dependent Vulkan buffer fill. It uses public Vulkan, DRM/vgem
and dma-buf interfaces only. It neither loads kernel modules nor changes drivers.

Prerequisites: Linux with vgem already loaded and accessible, Vulkan development
headers, libdrm headers, a C compiler, pthreads, and Linux UAPI `vgem_drm.h`.
Many libdrm packages omit the latter; the kernel headers package supplies it.
The Vulkan device must support coherent host-visible memory and binary SYNC_FD
imports. The default selects NVIDIA vendor `0x10de` with Mesa NVK driver ID;
`--allow-stock-driver` also permits the NVIDIA proprietary driver ID.
`--device-index` selects among matching Vulkan devices, never a DRM node number.
The producer card is separately discovered by its DRM driver name `vgem`.

Build from this repository root (the libdrm include directory must precede the
kernel directory, so exported userspace `drm.h` is used):

```sh
mkdir -p build/fence-tracing
cc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libdrm vulkan) \
  -I"/usr/lib/modules/$(uname -r)/build/include/uapi/drm" \
  tools/native-vulkan-fence-latency.c -ldl -pthread \
  -o build/fence-tracing/native-vulkan-fence-latency
```

Run after separately preparing vgem/access permissions:

```sh
timeout --signal=TERM --kill-after=5s 120s \
  build/fence-tracing/native-vulkan-fence-latency \
  --driver "$PWD/build/nvk-host/src/nouveau/vulkan/libvulkan_nouveau.so" \
  --iterations 1000 --idle-ms 20 --measure-idle-ms 1000
```

Use the same executable, arguments, selected device and environment for old/new
backend comparisons. Preserve the output and hashes of the executable, ICD and
its backend dependencies. Extend the outer timeout when increasing iteration or
idle counts. This example takes at least 41 seconds at the default settings.

For an Android64 comparison, build the same source with NDK r29:

```sh
build/hwcomposer-work/ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang \
  -std=c11 -Wall -Wextra -Werror -I/usr/include/libdrm \
  -I"/usr/lib/modules/$(uname -r)/build/include/uapi/drm" \
  tools/native-vulkan-fence-latency.c -ldl -pthread \
  -o build/fence-tracing/native-vulkan-fence-latency-android64
```

Invoke it using the extracted bionic linker and runtime, with the selected
backend directory **first** in the library path. Otherwise the ICD's build-time
RUNPATH may load a different Rust backend and invalidate the comparison:

```sh
latency_backend=/var/lib/waydroid/nvidia-source-native-async/vendor_extra/lib64
latency_runtime="$PWD/build/vast-bundle/stage/lib/android/runtime"
timeout --signal=TERM --kill-after=5s 120s env \
  LD_LIBRARY_PATH="$latency_backend:$latency_runtime:$PWD/build/hwcomposer-work/deps/image-rootfs/system/lib64" \
  "$latency_runtime/linker64" "$PWD/build/fence-tracing/native-vulkan-fence-latency-android64" \
  --driver "$latency_backend/libvulkan_nouveau.so" --iterations 1000 --idle-ms 20 --measure-idle-ms 1000
```

Select the other backend by changing `latency_backend`; verify the printed
`mapped_driver` paths before interpreting results. The shared runtime helper
initializes Android tracing before baseline FD accounting and closes the Android
logger before final accounting. No Android GPU execution is implied by a build.

For each dependent iteration, the probe attaches a vgem write fence, exports it
through `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`, verifies it is pending, imports it
temporarily into a binary Vulkan semaphore, and submits a four-byte marker fill.
After the idle interval it checks the Vulkan fence is still pending, signals the
producer, waits at most two seconds, then verifies the marker. Successful imports
transfer FD ownership to Vulkan. A TRANSFER-to-HOST barrier and coherent memory
make the post-completion read valid; it never races a CPU marker read against a
GPU write. The control performs the same fill and completion wait without the
foreign dependency, using the same idle interval.

The reported metric is **completion-observed marker latency**: signal-ioctl
entry (or control `vkQueueSubmit` entry) to `vkWaitForFences` return, with marker
verification immediately afterwards. It includes signal/submit, scheduling and
host-wait overhead; it is not a raw GPU store timestamp. Do not compare its
numbers directly to the earlier scratch probe's unsynchronized marker polling.
All iterations, including the first, contribute to mean/min/p50/p90/p95/p99/max.
Whole-phase wall time and process CPU time are also printed, along with an
initial idle sample. Process CPU includes this probe, watchdog and in-process
driver threads; it excludes other processes and kernel workers.

An independent monotonic-condition watchdog releases any attached producer after
750 ms, including if a driver blocks submission; a watchdog release invalidates
the measurement. Failure cleanup releases the producer before a bounded drain.
There is no GPU busy loop or infinite `DeviceWaitIdle`. If released work still
cannot drain, the probe reports failure and exits without destroying resources
the GPU may still access. The outer process timeout remains necessary for a
broken driver call or kernel teardown: no userspace tool can guarantee a bound
on those operations. Normal teardown destroys Vulkan objects, closes producer
resources, and requires the FD count to return to baseline. A driver/runtime FD
leak is reported as failure, including for stock controls.

Public contracts: Linux `include/uapi/drm/vgem_drm.h`,
`include/uapi/linux/dma-buf.h`, and
[temporary SYNC_FD import](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportSemaphoreFdInfoKHR.html),
[bounded Vulkan fence waits](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html).
Compile/CLI checks do not establish GPU correctness or latency; hardware runs
are separate gates.
