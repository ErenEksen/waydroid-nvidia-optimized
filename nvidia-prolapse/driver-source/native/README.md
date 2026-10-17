# Native Rust RM driver and source Vulkan

The running Waydroid path now uses full source-built **Mesa NVK + NAK** over
this Rust RM backend, with our Android HAL and gralloc. Both x86_64 and i686
run against bionic. ANGLE supplies GLES. There is no proprietary NVIDIA
userspace or glibc shim inside the guest; the host kernel module/firmware and
host compositor remain the existing NVIDIA stack.

Start with [mesa/README.md](mesa/README.md) for builds and
[mesa/RM-ADAPTER.md](mesa/RM-ADAPTER.md) for the measured transport contract.
The older small compute frontend below remains useful for isolated experiments;
Waydroid builds disable it with `--no-default-features`.

## New compatibility source (not deployed)

The compatibility work described in [COMPATIBILITY.md](COMPATIBILITY.md) changes
the source and local build artifacts only. It adds exact release ABI adapters,
explicit class/ISA selection, old/new submission and semaphore modes, typed-kind
aliases, lazy CPU mapping/BAR budgets and CPU portability. Hardware/integration
and vast.ai testing were explicitly deferred. The measured deployment described
below remains historical evidence for its exact GB202/open615.71.09 tuple.

## Read the running path

| Question | Source |
|---|---|
| Which component owns each operation? | `mesa/nv_rm.h`: narrow C/Rust ABI; `mesa/nvkmd_rm.c`: NVK adapter |
| Which thread owns kernel objects? | `src/mesa.rs`: caller threads under one mutex (no worker), opaque IDs, ordered lifetime and error propagation; see `mesa/SYNCHRONIZATION.md` |
| How does memory reach CPU/GPU? | `src/rm.rs`: allocation/cache attributes/CPU mappings; `src/gpu.rs`: VA bindings |
| How does a frame become submitted work? | Mesa `nvk_queue.c` → adapter push arrays → `src/gpu.rs::Queue::execute` |
| What prevents stale work and early completion? | GPU-uncached SYS push/tail memory, USERD posted-write flush, unique completion sequence, RM error notifier in `src/gpu.rs` |
| How do Android buffers cross processes? | `src/external.rs`: RM object sharing → NVKMS GEM → PRIME dma-buf; no raw-RM dma-buf shortcut |
| Where are Vulkan, shader compiler and image layouts? | `build/mesa-rm-source/src/nouveau/{vulkan,nak,nil}` at the pinned source revision |
| Where is Android-specific behavior? | Repository `nvidia-prolapse/{hal,gralloc}`; Waydroid staging and cache migration: `dev/source-native` |

Measured 2026-10-12: full gralloc400-cycle stress passes, both bionicABIs pass
compute/graphics/external-memory/sync and actual Android window/AHB pixels;
Launcher, Settings and Minecraft render through NVK.
This is a working development deployment, not a Vulkan conformance claim.

## Earlier compute-only frontend (separate build)


The separate prototype implements a **real Vulkan compute/buffer path**, built and
verified against Android bionic. Ordinary SPIR-V supplied to
`vkCreateShaderModule` is compiled at pipeline creation by source-built Mesa
NIR/NAK, then executed through our Rust RM/Blackwell backend. No proprietary
NVIDIA userspace library, glibc shim, CUDA, NVRTC or PTX assembler is required
by the current build or runtime.

This compute-only frontend is **not the Waydroid Vulkan driver**. Images,
graphics and presentation belong to the full NVK integration above. The prototype uses the
host's NVIDIA kernel module and firmware; it does not replace those.

Public probe code and GLSL sources live under repository `tools/`.

## Reading routes

| Question | Start here |
|---|---|
| How does a Vulkan call become GPU work? | [src/vulkan/README.md](src/vulkan/README.md), then `commands.rs` |
| How are handles, layouts and descriptors kept alive? | `src/vulkan/mod.rs`, `pipeline.rs`, `memory.rs` |
| How can another thread use the driver without sharing unsafe Rust objects? | `src/backend.rs`: messages to a worker that owns all RM/GPU objects |
| What crosses the kernel boundary? | `src/rm.rs` and named wire layouts in `src/abi.rs` |
| How are memory, channels and submissions connected? | `src/gpu.rs`: VA → TSG/context-share → channel → GPFIFO → doorbell → completion |
| How does SPIR-V turn into instructions? | [compiler/README.md](compiler/README.md), `compiler/nv_nak.c`, `src/compiler.rs` |
| Where are workgroup dimensions, constant buffers and releases encoded? | `src/compute.rs`, public QMD v5 bit ranges |
| How do I reproduce the evidence? | `tools/verify-vulkan.py` |

This native backend is a new
implementation using public RM contracts and open-source compiler code; it is
not claimed to recreate all closed NVIDIA internals.

## Build and verify

From the repository root:

```sh
rustup target add x86_64-linux-android
bash nvidia-prolapse/driver-source/native/build-android.sh
python3 nvidia-prolapse/driver-source/native/tools/verify-vulkan.py
```

Prerequisites and the pinned compiler bootstrap are in `compiler/README.md`.
This checkout already has them. On a fresh environment, the compiler build's
explicit `--download-deps` option installs its pinned local tools and Mesa wrap
dependencies. Cargo's single dependency is ash `0.38.0+1.3.281`, with Vulkan
loader features disabled; `Cargo.lock` pins it. Builds otherwise run offline.
The Android target is x86_64 API33, NDK r29, Rust1.95.0. GPU tests require access
to the enumerated NVIDIA device; no container start or deployment occurs.

Artifacts in `target/x86_64-linux-android/release/`:

- `libnv_bionic_native.so`: Rust Vulkan interface and RM/GPU backend.
- `libnv_native_nak.so`: source-built SPIR-V/NIR/NAK compiler for bionic.
- `nv-native-check`: direct backend diagnostics.
- `native-backend-probe`: independent C diagnostic-ABI loader.

The public Vulkan probe is built to `build/native-vulkan-probe/probe-android`.
Its two GLSL shaders are compiled with glslang to ordinary SPIR-V. An example
standalone bionic run after verification has generated the probe/shaders:

```sh
repo_dir="$PWD"
native_dir="$repo_dir/nvidia-prolapse/driver-source/native"
artifact_dir="$native_dir/target/x86_64-linux-android/release"
NV_NATIVE_COMPILER_PATH="$artifact_dir/libnv_native_nak.so" \
LD_LIBRARY_PATH="$native_dir/runtime/lib64" "$native_dir/runtime/linker64" \
  "$repo_dir/build/native-vulkan-probe/probe-android" \
  --driver "$artifact_dir/libnv_bionic_native.so" \
  --shader "$repo_dir/build/native-vulkan-probe/generate.spv" \
  --transform "$repo_dir/build/native-vulkan-probe/transform.spv" --negative
```

`runtime/` contains unmodified guest linker64/libc/libdl/libm extracted read-only
from `system.img`'s com.android.runtime APEX. Use absolute executable paths.
The warning about missing `/linkerconfig/ld.config.txt` is preserved: this runs
real bionic with its default linker configuration on the host, not in Android
app/sphal namespaces or under app seccomp. Those gates are covered separately by the full NVK integration above.
The compiler path is explicit; its default SONAME is `libnv_native_nak.so`.
There is no fallback to the installed proprietary driver or software rendering.

## What passed (2026-10-12)

`tools/verify-vulkan.py` writes the full commands, output, source/artifact hashes and Xid check
to `evidence/vulkan-source-verification.json` (12 commands, 43 hashed files). Its backend
regression includes `native-bionic-verification.json` (19 commands).

- The same public Vulkan probe passed direct host GIPA, the standard Khronos
  host loader, and direct bionic GIPA. Stock NVIDIA passed as a positive control.
- Two distinct runtime-compiled shaders, six invocation counts (1,63,64,65,257,
  1024), 4,422 generated/transformed/copied words per run, full allocation guards.
- Push constants change between dispatches. Descriptors rotate buffers between
  rounds. Buffer memory is bound at offset4096 and descriptors at offset64.
- Hardware shader writes → shader reads → GPU copy → CPU readback all agree.
  Fence unsignalled/zero-timeout/submitted/reset paths pass. Unknown device
  extension and an advertised-unsupported core feature are rejected correctly.
- File descriptors remain 4→4 after complete Vulkan teardown. No cleanup errors
  and no Xid during final verification. No NVIDIA userspace/shim mappings.
- 30 independent public-C/Rust RM ABI checks agree under host and bionic.
  Compiler result ABI is independently checked in C and Rust (48 bytes).
- 9 focused Rust tests, strict Clippy and formatting pass; public probes compile
  warning-clean for host and Android. Both public shader modules pass spirv-val.
- Android driver DT_NEEDED: libc.so/libdl.so. Compiler additionally needs libm.so;
  neither has GLIBC symbol requirements or proprietary userspace dependencies.

This establishes this compute subset, not Vulkan conformance. The driver offers
one synchronous compute/transfer queue, a bounded512MiB heap, set0 storage-buffer
bindings0..7 and128-byte push constants. Runtime checks SM120, shader resources
and all accepted buffer ranges. No optional Vulkan core features/extensions are
advertised. Specialization, descriptor arrays, UBOs, images, NumWorkgroups and
shader local stacks are unsupported and fail explicitly. Robust buffer access
is not implemented; valid shader accesses must remain in bounds.

The global frontend mutex and synchronous GPU completion simplify ownership and
ordering at a performance cost. Long-running stress and device-loss recovery
are not verified. Destructors attempt channel teardown before memory release and
log failures; teardown-failure fault injection is untested. The compiler DSO is
intentionally pinned for process lifetime, including after the driver is dlclosed.

## Provenance and previous dead ends

- Public NVIDIA RM source/headers: `build/ogkm-615.71.09`, commit
  `61dcc93722ecb418bb5f2e00923f05b4b8051dd1`. Driver version is read from `/proc`
  and checked strictly with RM; there is no hardcoded runtime driver version.
- tinygrad's NV backend, commit`37132d7553a831fdaf900e2334d80380702c5564`, provided
  public direct-RM/GPFIFO prior art. QMD v5 definitions carry NVIDIA's MIT notice.
- Mesa compiler pinned at`ddaf85e353442d9d7f192fdcf1823e652e3f2ee3` (mesa-26.2.5), in isolated
  `build/mesa-nak-source`; canonical patch/build adapter are under `compiler/`.
- ash supplies Khronos ABI bindings only. Attribution is in `licenses/` and
  `compiler/MESA-MIT.txt`; original upstream notices remain in source trees.
- TSG engine0 is invalid (RM0x1f); engine1 is accepted. TSG channels must use
  context-share rather than explicit channel hVASpace (kernel_channel.c436).
  RM-owned nonfaulting VA works here without UVM-managed page tables.
- Historical `pattern.ptx`, generated code/metadata and `tools/build-shader.py`
  remain local research artifacts; the current build does not use them.
  The diagnostic now also compiles source GLSL/SPIR-V through NAK.

The former next gates (graphics, sharing and Android presentation) now pass
through the full NVK integration. Do not deploy this separate compute prototype
as the system Vulkan HAL.
