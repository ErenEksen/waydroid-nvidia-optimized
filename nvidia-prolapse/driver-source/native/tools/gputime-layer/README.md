# VK_LAYER_PERF_gputime: GPU time per render pass and dispatch, on any driver

A Vulkan layer (Rust, `cdylib`) that times every render pass instance (`vkCmdBeginRenderPass/2`,
`vkCmdBeginRendering`), every dispatch (`vkCmdDispatch/DispatchBase/DispatchIndirect`), every primary command
buffer and every frame of an unmodified application. The same binary runs on NVK and on NVIDIA's driver, so two
runs of a deterministic workload (3DMark Wild Life Extreme) line up pass by pass. `gputime-compare` matches the
passes of two runs and ranks them by the difference.

## What is measured

Inside the frame window only (outside it no command is added):

* A primary command buffer gets a TOP_OF_PIPE timestamp after `vkBeginCommandBuffer` and a BOTTOM_OF_PIPE one
  before `vkEndCommandBuffer` (row kind `cb`).
* A render pass (`rp` = render pass object, `dr` = dynamic rendering) or a dispatch (`cs`, `csi` = indirect) gets
  a BOTTOM_OF_PIPE timestamp before it and after it, outside the render pass instance.
* `GPUTIME_SYNC=barrier` (default) puts a pipeline barrier (src ALL_COMMANDS + MEMORY_WRITE, dst ALL_COMMANDS,
  no destination access) before every BOTTOM_OF_PIPE timestamp. Without it the numbers are not the GPU time of
  the pass: NVK writes the timestamp as a report at the end of the 3D pipeline (`nvk_CmdWriteTimestamp2`,
  src/nouveau/vulkan/nvk_query_pool.c) and waits for idle only for barriers that name a write access
  (`nvk_barrier_flushes_waits`, nvk_cmd_buffer.c). With the barrier every region is "the work between two
  points at which the queue was idle": regions do not overlap and add up, and the cost is one wait for idle per
  region boundary on either driver (frames get slower than without the layer; compare the two drivers in the
  same mode). `GPUTIME_SYNC=none` drops the barriers.
* `frame` row: the union of the frame's `cb` intervals (GPU busy) and their span.
* Queries come from one 65,536-entry timestamp pool per device, in chunks of 128 that are reset on the host
  (`VK_EXT_host_query_reset`, which the layer enables on the application's device) when they come back. A
  query therefore reads as available only after the execution it was handed to wrote it (a
  `vkCmdResetQueryPool` recorded into the next command buffer leaves the old values readable until the GPU
  reaches it). A command buffer submitted again without re-recording waits for its previous execution first
  (`waits` in the frame row).
* The submission itself is never changed.

## Output

`<prefix>-<pid>-d<N>.csv` per VkDevice (an Android app with HWUI/ANGLE has several; the benchmark's device is
the one with thousands of rows), `<prefix>-<pid>-d<N>-pipelines.csv` and `<prefix>-<pid>-d<N>-sync.csv`.

    # device=NVIDIA GeForce RTX 5090 (NVK NV1B2) vendor=0x10de ... timestamp_period=1 frames=200-499 boundary=present sync=barrier
    frame,seq,cb,kind,fmt,extent,pipe,npipe,draws,gpu_ns,start_ns,note
    300,2,1,rp,98+37+37+83+126+122,3840x2160,9801fee1719beb2a,5,537,357504,1792860866425562368,
    300,1,1,cb,,,0,0,537,361088,1792860866425560064,regions=1;skipped=0
    300,,,frame,,,0,0,0,2332608,1792860866425511680,span=2340736;cbs=3;unmeasured_cbs=0;waits=0;skipped=0

* `frame`: present count of that device (or submit count with `GPUTIME_FRAME=submit`).
* `seq`: region order within the frame (submission order, command buffer order, region order); `cb`: command
  buffer index within the frame.
* `fmt`: VkFormat numbers of the attachments, `+`-joined (render pass attachments in order; for dynamic
  rendering the colour attachments, then depth, then stencil if it is another view).
* `extent`: render area `WxH`; dispatch `X*Y*Z` or `ind`.
* `pipe`: FNV-1a 64 over each stage's (stage bit, SPIR-V hash, entry point, specialization map + data); a pass
  folds the pipelines it draws with, in order; `npipe` counts pipeline changes. The pipelines file maps a
  pipeline hash to `stage:spirvhash:entry` per stage (a SPIR-V hash is FNV-1a 64 of the module's bytes).
* `draws`: draw calls inside the pass (draws of executed secondary command buffers included).
* `gpu_ns`, `start_ns`: from the timestamps times `timestampPeriod`.
* `skipped`: regions not measured (pool exhausted, or a suspended/resumed dynamic-rendering instance).

Sync log (`-sync.csv`, rows `ev,frame,rec,ridx,inrp,detail...`): what the application asks the driver to order
and where, so the time between and around the measured regions can be explained per driver. Created objects
(outside any recording): queue families, requested and created queues, memory types, images, memory
allocations, image binds, render pass definitions with their subpass dependencies. Recorded inside the frame
window: every pipeline barrier (one row per memory, buffer or image barrier), event wait and set, render pass
begin and copy/clear/blit/resolve/fill/update command, placed by `rec` (the recording id that the main file's
`cb` row carries as `rec=`) and `ridx` (the number of regions of that recording begun before it); and every
queue submission with its queue family and index and the recording ids of its command buffers. The comment at
the top of src/synclog.rs lists every row's fields.

The files are flushed at every read-back, so an app that is killed without `vkDestroyDevice` still leaves
complete files; the main file ends with `# end of window A-B` once the window is complete.

## Configuration

| glibc environment | Android property | default | meaning |
|---|---|---|---|
| `GPUTIME_OUT` | `debug.gputime.out` | (none: layer is passive) | output prefix |
| `GPUTIME_FRAMES` | `debug.gputime.frames` | `1000-1299` | inclusive frame window |
| `GPUTIME_FRAME` | `debug.gputime.frame` | `present` | `submit` for apps that never present |
| `GPUTIME_SYNC` | `debug.gputime.sync` | `barrier` | `none`: no barrier before timestamps |
| `GPUTIME_DISPATCH` | `debug.gputime.dispatch` | on | `off`: no regions around dispatches, so back-to-back dispatches run as the driver schedules them (their time shows only in the `cb` row) |

## Build (offline; ash 0.38 from the local cargo registry)

    OUT=build/gputime-layer ANDROID_NDK_HOME=<ndk r29> nvidia-prolapse/driver-source/native/tools/gputime-layer/build.sh
    # -> $OUT/host/{libVkLayer_PERF_gputime.so,VkLayer_PERF_gputime.json,gputime-compare}
    #    $OUT/android/libVkLayer_PERF_gputime.so   (x86_64-linux-android, API 33; Android needs no manifest)

## Host

    VK_ADD_LAYER_PATH=$OUT/host VK_INSTANCE_LAYERS=VK_LAYER_PERF_gputime \
    GPUTIME_OUT=/tmp/gt GPUTIME_FRAMES=1000-1299 <vulkan app>
    # NVK through a loader setup that disables implicit layers: also VK_LOADER_LAYERS_ENABLE=VK_LAYER_PERF_gputime

## Android guest (Waydroid; no image rebuild, no ro.debuggable)

Android 13's loader takes debug layers from the app's native library directory when the process is dumpable
(`GraphicsEnv::isDebuggable()` = `prctl(PR_GET_DUMPABLE)`, frameworks/native libs/graphicsenv/GraphicsEnv.cpp).
Apps are dumpable in this guest because the host kernel has `fs.suid_dumpable=2`; on a host with 0,
`ro.debuggable = 1` in waydroid.cfg `[properties]` (then `waydroid upgrade -o`) is needed. The app writes its
own cache directory (`/data/local/tmp` is not writable for app uids).

    L="sudo -n lxc-attach -P /var/lib/waydroid/lxc -n waydroid --clear-env --"
    P=com.futuremark.dmandroid.application
    $L /system/bin/sh -c "PATH=/system/bin dumpsys package $P | grep codePath"   # -> <codePath>
    cat $OUT/android/libVkLayer_PERF_gputime.so | \
        $L /system/bin/sh -c "cat > '<codePath>/lib/x86_64/libVkLayer_PERF_gputime.so'"
    $L /system/bin/sh -c "export PATH=/system/bin
        settings put global enable_gpu_debug_layers 1
        settings put global gpu_debug_app $P
        settings put global gpu_debug_layers VK_LAYER_PERF_gputime
        setprop debug.gputime.out /data/user/0/$P/cache/gt
        setprop debug.gputime.frames 1000-1299
        am force-stop $P"
    # start the app and the benchmark; logcat tag "gputime" names each device's file and says
    # "frame window A-B written" when it is complete:
    $L /system/bin/sh -c "PATH=/system/bin logcat -d -s gputime"
    $L /system/bin/cat /data/user/0/$P/cache/gt-<pid>-d2.csv < /dev/null 2>/dev/null | cat > ours.csv

3DMark (x86_64 2.7.5093, 2560x1412 guest display): `am start -n $P/com.futuremark.flamenco.ui.splash.SplashPageActivity`,
tab "WILD LIFE EXTREME" at `input tap 1568 143`, run at `input tap 1590 535`; the benchmark's device is `d2`
(d0/d1 are HWUI and ANGLE). The same commands work in glibc-shim mode (stock ICD behind the Vulkan HAL).
Undo: `settings delete global enable_gpu_debug_layers` (and `gpu_debug_app`, `gpu_debug_layers`); the library
disappears with the app's next update.

## Compare

    gputime-compare ours.csv stock.csv [--top 40] [--strict] [--csv ranked.csv]

Regions are matched by (kind, fmt, extent, pipeline hash for dispatches, occurrence of that identity within the
frame) over the frames present in both files. A pass's draw count and pipeline sequence are shown (median
draws A/B), not matched: WLE animates by time, so frame N of a faster driver can draw other objects (seen:
537 to 559 draws in the main pass on one run); `--strict` matches them too; per region: median over frames, delta A-B, delta as % of B's median frame busy
time, sorted by delta. It also prints the median frame busy time of both, the median time outside any region
(copies, clears, blits, barriers between passes) and the regions found in only one file (a driver that exposes
other features can make the application pick other formats or pipelines).
