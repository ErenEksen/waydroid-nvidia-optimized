# Ordinary completion and exported fence timeout

This standalone probe uses public Vulkan and Linux sync-file interfaces. It
selects an NVIDIA device by vendor ID, enables the timeline-semaphore feature,
and submits a four-byte fill behind timeline value 1. A joined host thread
signals that value after a configurable delay. There is no shader, GPU spin,
host VkEvent gate, private RM interface, or deployment.

```sh
mkdir -p build/fence-design
cc -std=c11 -Wall -Wextra -Werror -O2 tools/native-vulkan-fence-design.c \
  -ldl -pthread -o build/fence-design/fence-probe
timeout --signal=TERM 20s build/fence-design/fence-probe \
  --driver /usr/lib/libEGL_nvidia.so.0 --allow-stock-driver --delay-ms 6250
timeout --signal=TERM 20s build/fence-design/fence-probe \
  --driver /usr/lib/libEGL_nvidia.so.0 --allow-stock-driver \
  --delay-ms 6250 --sync-fd --expect-export-timeout
```

Use the absolute source-built ICD path without `--allow-stock-driver` for the
native comparison. Proprietary NVIDIA/shim mappings are then rejected. The
probe loads `vk_icdGetInstanceProcAddr`, falling back to `vkGetInstanceProcAddr`
only if the former symbol is absent; it does not link a Vulkan loader. Android
runtime accounting follows `native-vulkan-runtime.h`.

`--delay-ms` defaults to 100 and accepts 10–60000. `--wait-ms` independently
sets the finite completion/cleanup wait **after host release**, defaults to
2000, and accepts 1–60000. The initial blocked fence wait is always 1 ms.
`--single-wait` also uses this budget for each ordinary witness wait while the
host gate is held, instead of the default 10 ms caller slices. For syscall
tracing, choose a budget longer than the host delay so the driver's internal
wait strategy is visible within one API call. This option does not alter the
opaque test or the probe's explicit sync-file status checks.
Choose an outer process timeout greater than the delay plus completion and
cleanup budgets. `--sync-fd` returns 77 if exportable sync-file fences are
unsupported. `--expect-export-timeout` additionally requires observing a
negative timeout status before host release; it requires `--sync-fd` and a
delay of at least 5500 ms. Ordinary mode does not require external-fence support.

The ordinary fence must initially return `VK_TIMEOUT` from the finite wait
and `VK_NOT_READY` from status. A separate ordinary completion witness remains
pending throughout the held interval. After host signaling, the witness must
complete and the coherent marker must match. A second submission verifies the
device still works; untouched buffer guards must remain intact. Host reads of
markers occur only after the Vulkan completion wait, with a recorded transfer
write → host read memory barrier.

In sync-file mode the first fence is exportable. Its export must return a real
pending descriptor before host signal; exporting resets the source fence.
The probe reads `SYNC_IOC_FILE_INFO` and prints readiness/error separately from
the ordinary completion witness. A timed-out export must not complete or lose
that witness. Descriptor status is not used as authorization to read/reuse the
GPU buffer. Export error alone does not make the default observational mode
fail; the optional timeout expectation checks the specific timeout outcome.

Failure paths release the host gate immediately and join the signal thread.
They attempt only a finite completion wait, never `vkDeviceWaitIdle`. If GPU
quiescence remains unproven, the probe reports that fact and exits without
calling Vulkan object destructors or `dlclose`; it does not detach cleanup or
claim successful destruction. A blocked driver call or kernel process cleanup
still requires the outer process supervisor. The gate is a real, finite host
operation; this fixture intentionally does not create an unreleasable GPU hang.

## Trace a blocking wait

Use short host-gated work to distinguish an event wait from a periodic sleep:

```sh
timeout --signal=TERM 20s strace -f -ttt -T -yy \
  -e trace=poll,ppoll,ioctl,openat,close,fcntl,nanosleep,clock_nanosleep \
  -o build/fence-design/stock-wait.strace \
  build/fence-design/fence-probe --driver /usr/lib/libEGL_nvidia.so.0 \
  --allow-stock-driver --delay-ms 100 --wait-ms 2000 --single-wait
```

An event can end `poll()` before its timeout. A timeout can also request a
predicate recheck without implying device loss. Attribute syscalls to their
actual callers: this probe itself queries/polls exported sync-files, and other
driver threads can have unrelated timers. Add strace `-k` for call stacks when
available. Tracing perturbs timings; use it to identify mechanisms, not measure
production latency. Do not inject a never-signalling producer to test cleanup.

`tools/vulkan-entry-probe.c` requests Vulkan 1.2 and includes timeline wait,
signal and counter entrypoints. Its reported addresses belong to the exact
installed image; record that image's hash and build ID before using them.

## Measured stock control

On the local RTX 5090, stock 615.71.09, short ordinary and sync-file cases pass.
With a 6250 ms host gate, ordinary completion passes at 6250.207 ms. With a
pending sync-file export, that descriptor reports `-110` at 5426.108 ms while
the gate is still closed and the ordinary witness remains `VK_NOT_READY`.
The host signals at 6250.095 ms; the ordinary witness completes at 6250.422 ms,
the marker and second submission pass, and descriptors return 4 → 4.

Logs are under `build/fence-design/stock-{ordinary,syncfd}-{100,6250}*.log`.
These numbers are observations, not hardcoded expected timing windows or
coverage claims for other drivers. Initial sandbox runs could not resolve
stock `vkCreateInstance`; successful controls ran with approved local access
outside that sandbox. No backend implementation was changed by this probe.

## Opaque semaphore sharing

`--opaque` selects a separate regression using two independent `VkDevice`
objects on the selected NVIDIA physical device. It cannot be combined with
`--sync-fd`. The producer exports an opaque timeline semaphore and an opaque
binary semaphore; the consumer permanently imports both descriptors. Successful
imports consume the descriptors. The devices have separate coherent marker
buffers, command pools and ordinary completion fences.

```sh
timeout --signal=TERM 20s build/fence-design/fence-probe \
  --driver /usr/lib/libEGL_nvidia.so.0 --allow-stock-driver --opaque
timeout --signal=TERM 35s build/fence-design/fence-probe \
  --driver /absolute/native/ICD.so --opaque --delay-ms 6250
```

Two cycles each hold producer GPU work behind the host timeline gate for
`--delay-ms`. The producer fills a marker and signals both shared semaphores.
The consumer first waits on both imported semaphores before filling its own
marker. On the second cycle it waits only on the reused binary semaphore, so a
correct timeline cannot hide a stale binary completion. The imported timeline
is independently checked by finite CPU waits and counter queries in both
cycles. Both devices' fences must stay pending before release; afterward their
timeline values and buffer contents must match. Ordinary fences are explicitly
reset between cycles; binary payload reset is the implicit reset caused by the
consumer's GPU wait, followed by producer resignal.

The second cycle is a second submission on each device. Cleanup releases and
joins the current gate, then uses only finite fence waits. It skips all Vulkan
destruction if either device's quiescence remains unproven. No Vulkan state is
forked, and this same-process test does not establish cross-process transport.
Unsupported opaque binary/timeline import or export returns 77.

The stock RTX 5090/615.71.09 short control passes: both imported CPU waits and
consumer fence waits initially return `VK_TIMEOUT`, cycles complete at
100.353/100.419 ms with timeline values 1/2, markers pass and descriptors return
4 → 4. See `build/fence-design/stock-opaque-100.log`. The 6250 ms opaque command
above is supported by the fixture but was not run as part of this short stock
control; native results require a fresh backend build.

## Native local validation

The redesigned source backend passed on RTX 5090/open615.71.09 (GNU x86_64).
With a 6250 ms host delay, the exported sync-file reported `-ETIMEDOUT` at
5113 ms while the ordinary witness remained `VK_NOT_READY`. Ordinary completion
arrived at 6250 ms; the second submission, marker and guards passed, FDs 4 → 4.
Two-device opaque cycles completed at 6255/6254 ms with timeline values 1/2,
binary consume/resignal and stable FDs. Logs: `build/fence-design/final-{syncfd,opaque}.log`.
These are bounded host-gated workloads, not long non-preemptible GPU kernels.
No Android runtime, remote-driver coverage or deployment is implied.

## Public contracts

- [Host timeline signal](https://docs.vulkan.org/refpages/latest/refpages/source/vkSignalSemaphore.html)
  defines the host signal operation; no host-set event is used.
- [Finite fence waits](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html)
  distinguish timeout from completion and device loss.
- [Fence FD export validity](https://docs.vulkan.org/refpages/latest/refpages/source/VkFenceGetFdInfoKHR.html)
  permits copy export of a fence whose signal operation is pending execution.
  This is a fence export, not a binary-semaphore export with different valid-use
  constraints.
- The inspected NVIDIA public kernel source at commit
  `61dcc93722ecb418bb5f2e00923f05b4b8051dd1`,
  `kernel-open/nvidia-drm/nvidia-drm-fence.c:43,1399–1407,1447–1449`, caps
  semaphore-surface fence instances at 5000 ms. The probe itself neither uses
  that ioctl nor assumes this cap applies to ordinary Vulkan completion.
