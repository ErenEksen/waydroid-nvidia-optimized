# Cold-start and desktop smoothness: capture and acceptance contract

## Changes and boundaries

1. **Command ordering, not GPU idle.** The semaphore import socket cannot pass
   earlier ring-side semaphore creation or waits. `vn_ring_wait_all` waits for
   ring consumption once per submission. `vn_ring_roundtrip` only queued a wait
   in the opposite direction and did not provide this guarantee.
2. **Fence payload ownership.** Internal SYNC_FD export is performed once, before
   a retirement thread can start waiting. Its retained fd is polled for retirement
   and duplicated for exports; the reset VkFence is not subsequently waited on.
   Export/import failure is sticky, not an already-signaled fence. A successful
   `-1` export bypasses `poll`, which would otherwise ignore it and wait forever.
3. **Safe fallback and lifetimes.** Device/semaphore ownership and destruction
   are serialized against import; transferred fds are not closed again when
   sending a reply fails. EOF no longer becomes an endless zero-byte read loop;
   vtest/gralloc socket I/O has a 30 s bound. Existing vtest transport failure
   handling aborts the affected client explicitly; it is not transparent recovery.
4. **One ordered queue per shared timeline.** The first external-fence queue uses
   the context timeline. Other queues use the socket path rather than allowing
   independent queues to advance a cumulative timeline past each other's work.
5. **Low-risk hot paths.** Cache immutable format/modifier enumeration, not live
   images. Reuse unchanged HWC opaque-region state and keep its ownership paired
   with the Wayland surface. Per-buffer/frame logs are opt-in; direct DMA-BUF
   composition and the client-composition fallback remain available.
6. **Cache scope.** `50-renderer-cache.conf` gives only the renderer service a
   user-owned persistent cache below `%C/waydroid-nvidia`. The isolated Vulkan
   compute test verifies writes and subsequent **IN_ACCESS** events on existing
   driver cache files. It does not prove reuse inside Android HWUI/Skia/ANGLE.
   Existing caches are not cleared or silently copied; the first use of the new
   directory can be colder. Cache population and shader compilation must be
   reported separately from any synchronization improvement.

Not implemented as an unmeasured “optimization”: a guest scratch-syncobj pool,
image-memory pooling, global scheduling changes, busy-wait extensions, shader
pre-warming, disabling animations, lowering resolution/refresh or forcing
high/real-time priority. No new cache layer was placed above Skia/ANGLE.

## Build and component controls

See `dev-workflow.md`. The source pins and NDK SHA are in `packaging/ci/*.env`;
release and diagnostic build profiles are explicit. Native `mesa-host` is only
for unprivileged regression tests; the Android x86/x86_64 outputs use the NDK.
`tests/check-patches.py` starts from pristine pinned upstream files, reapplies the
entire series and verifies every touched source file against the build tree.

Diagnostic controls (off in normal use):

- Guest `VTEST_PERF_STATS=1`: ring wait nanoseconds, successful semaphore imports,
  real CPU waits per submission. Mesa tracing also marks the ring-order wait.
- Host `VTEST_PERF_STATS=1`: imports, import errors/time, timeline high-water mark
  and queue-full waits. Opt-in summaries are emitted periodically/on high-water
  changes as well as at context destruction; normal operation does not time imports.
- Host `VTEST_STATS=1`: existing per-command counts/time, including GPU allocation.
- HWC `debug.waydroid.hwc_buffers` and `debug.waydroid.hwc_frames`: opt-in logs.
- `VTEST_NO_TIMELINE_FENCE=1` / `VTEST_NO_SEMAPHORE_IMPORT=1` on the **guest
  process** select socket/CPU fallbacks for isolated A/B tests.

Do not set these globally. Detailed logging itself changes frame pacing.

## Startup capture

`dev/bench-startup` requires package/mode/repetition/output parameters and the
actual output name hosting **both visible windows**. It never deletes app data or
shader caches. Resolution comes from `wm size`; gestures come from the focused
Android window bounds, not fixed screen coordinates. Refresh comes from SF's
reported period (165 Hz is approximately 6.06 ms, not a hardcoded 250 Hz).

- `session-cold`: controlled complete restart, without `show-full-ui` pre-warm;
  resource/probe capture begins before restart. This is session-cold, not a
  machine boot or an empty kernel page/shader cache.
- `app-cold`: force-stop the application process before each launch; disk caches
  and app data remain intact.
- `warm`: one explicitly reported initial launch, then home/reopen and animation
  repeats without force-stop. The main PID set is checked against the initial
  launch; a changed or unidentifiable process makes the run incomplete.

Gfxinfo is drained **while** `am start -W` waits and throughout gestures. First-
draw flagged rows, long stalls and later presentation updates are retained.
Rolling snapshots are deduplicated by window and vsync. Separate windows are
never concatenated into a fake presentation stream. Idle gaps between deliberate
inputs are excluded only when they cross recorded active ranges; long stalls
inside an active range are not clipped. Gaps long enough to risk a 120-frame
rolling-buffer overflow make the capture incomplete.

Three distinct times are not interchangeable:

- `am start -W`: activity-manager launch wait, **not display presentation**.
- HWUI `FrameCompleted - IntendedVsync`: rendering work duration.
- Gfxinfo `DisplayPresentTime`: **Android-reported presentation**, not proof of
  the corresponding pixel being visible on the physical host display. The
  first-present-after-request field uses this source only and is null if absent.

A separate 160×120 Wayland client logs `wp_presentation` timestamps, discarded
frames, refresh intervals, output identity and presentation-clock identity.
Frame callbacks only pace drawing, not measure it. KWin can provide sequence
zero: missed refreshes are then explicitly a **timestamp estimate**, not an
invented hardware counter. The JSON also includes cgroup CPU/memory/pressure,
process RSS/fd/thread samples, GPU utilization/power/clocks and raw logs.

**Limitations:** the host probe is not the Android surface and cannot attribute
its first visible pixel to a particular activity. Android 13 builds may not
supply valid `DisplayPresentTime`; missing/partial data remains incomplete,
rather than being replaced with render completion or average FPS. Perfetto/HWC
surface-specific presentation correlation is follow-up work if this image lacks
those timestamps. These tools have not yet been exercised against this live
Android instance because authenticated root access was unavailable.

## Controlled before/after matrix

First build the probe and validate that it is visible on the requested display.
Use the same output, resolution, power profile, application content and controlled
background load. Keep the probe visible but do not let it cover/intercept the
Android gesture target. Save workload conditions alongside the raw reports.

```sh
mkdir -p .work/reports
dev/present-probe --build-only
sudo -v
# Run this entire block BEFORE installation, then repeat with phase=after.
phase=before
for spec in session-cold:5 app-cold:10 warm:10; do
  mode=${spec%:*}; runs=${spec#*:}
  dev/bench-startup --package com.android.settings --mode "$mode" --runs "$runs" \
    --duration 10 --label "$phase" --desktop-output eDP-1 \
    --output ".work/reports/$phase-$mode" || break
done
# Only after all captures, not after a failed/missing run:
dev/compare-startup .work/reports/before-app-cold/report.json .work/reports/after-app-cold/report.json
```

The comparator rejects mismatched/unknown controlled metadata, resolution,
refresh, scenario, output, run count or incomplete measurement. Cold gates are
p99 ≤70% of baseline and >50 ms interval rate ≤50% of baseline; warm p99 must be
≤2 periods. Desktop missed-refresh ratio must not worsen by more than 5%.
A zero-stall baseline requires zero new stalls. No result currently demonstrates
these improvements. The comparator is **not full project acceptance**.

Remaining full acceptance gates:

- Zero new normal-path object-ID errors, synchronization errors or crashes.
- Launcher as well as Settings animation and app/process/session lifecycle tests.
- Actual x86 and x86_64 Android apps; alpha, resize, DMA-BUF and composition fallback.
- Paired GLES/Vulkan synthetic throughput (no reproducible >5% regression).
- ≥30 minutes of open/close use, renderer restarts, then stable CPU/RSS/fd/syncobj/
  thread levels, with no early buffer reuse or black frames.
- If first physical Android-surface visibility is needed, capture correlated HWC/
  Perfetto presentation rather than relabeling the independent desktop probe.

Install only using the complete-set backup/health/rollback workflow, and preserve
failed/incomplete captures. Baseline data is not manufactured by repeated opening
or clearing caches. Historical 250 Hz claims elsewhere in the repo are not this
machine's before/after evidence.
