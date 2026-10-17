# probe/ — measurement tools (P0 host, P1 guest)

- `vkcompute.c`: headless Vulkan compute dispatch with a verified result. Selects the GPU
  by vendor ID, never by node path. Three bootstraps:
  - default: through `libvulkan.so.1` (ICD manifest → `libGLX_nvidia.so.0`)
  - `--icd PATH`: dlopen PATH and use the Khronos loader↔ICD interface directly.
    `./vkcompute --icd libEGL_nvidia.so.0` is the configuration the P1 spike used.
  - `--sphal NAME` (Android only): `android_load_sphal_library(NAME)` from libvndksupport, i.e.
    load by soname from the sphal linker namespace, exactly as AOSP's libvulkan loads
    `vulkan.<hw>.so`. The P2 gate: no LD_LIBRARY_PATH, no absolute path.
  `make` builds the host binary (`vkcompute`); `make nvprobe` builds the same source with the
  NDK (API 33, x86_64) — that is the in-guest P1 binary.
- `ldaudit.c`: `LD_AUDIT` helper that logs every library search and denies the ones matching
  `$AUDIT_DENY` (extended regex). Used to prove which dependencies are optional:

      LD_AUDIT=$PWD/libldaudit.so AUDIT_DENY='libGLX_nvidia|libvulkan|libX11' ./vkcompute --icd libEGL_nvidia.so.0

- `dev/deploy-guest` (repo root, Waydroid-side; host, root): stages the installed driver's five blobs, `shim/out/*`,
  `nvprobe` and `nvprobe.sh` into the running guest's `/data/local/tmp/nvp`, plus the host
  glibc version string the shim must report. Spike-only; the shipping path bind-mounts.
- `nvprobe.sh` (guest): checks every piece is present, sets `LD_LIBRARY_PATH` to the shims
  and blobs and `NVGLIBC_GLIBC_VERSION`, runs `nvprobe --icd`. Pass `VKCOMPUTE_HOLD=1` in
  the environment to keep the process alive for `g1-check.sh`.
- `g1-check.sh <guest-pid>` (host, root): maps the guest PID to the host PID through
  `NSpid` and checks that `nvidia-smi` lists it. The gate G1/G2 host half.
- `appsandbox.cpp` (NDK): `appsandbox [--uid N] [--gid N] [--no-seccomp] -- CMD…` drops to an app
  uid, sets no-new-privs, installs **bionic's own app seccomp filter** and execs. The filter is
  generated at build time by bionic's `libc/tools/genseccomp.py` from `SYSCALLS.TXT` and the
  `SECCOMP_*LIST` files at a pinned android13-release commit (`make appsandbox` clones it into
  `gen/bionic`), with syscall numbers from the NDK's `asm/unistd` headers, and assembled the way
  `libseccomp_policy` does (arch check, x86_64 policy, x86 policy, `SECCOMP_RET_TRAP`). A blocked
  syscall is a SIGSYS, never a silent EPERM.
- `nvprobe-sphal.sh` (guest, root): the P2 runner. Must live in a `dir.system` linker path
  (`/data/local/tests/system`); runs `appsandbox -- nvprobe --sphal libEGL_nvidia.so.0` with
  `LD_LIBRARY_PATH` and `NVGLIBC_GLIBC_VERSION` unset. `NVP_LIBDIR`, `NVP_UID`,
  `NVP_SANDBOX_FLAGS`, `NVP_SKIP_NODE_CHECK` tune it. `dev/deploy-guest` stages all three.
  `NVP_ABI=32` runs `nvprobe32` (the i686 build) against `/vendor_extra/lib`; appsandbox stays 64-bit,
  because bionic's app filter is dual-arch (x86_64 + x86) in both zygotes and the kernel applies its x86
  half to the 32-bit program it execs.
- `eglgles.c` (P7 G7a): NVIDIA's own EGL + GLES **without libglvnd**. The probe is its own minimal glvnd vendor
  host: it calls `libEGL_nvidia`'s `__egl_Main` with its own `__EGLapiExports` (glvnd semantics, every callback
  counted, `fetchDispatchEntry` aborts), resolves every EGL/GL function through the vendor's `getProcAddress`, gets
  a display from `EGL_PLATFORM_DEVICE_EXT` (`--platform device`, default) or `EGL_PLATFORM_SURFACELESS_MESA`, and
  verifies exact pixels (pbuffer and surfaceless FBO), a GLES 3.1 compute SSBO, `EGL_KHR_fence_sync` and
  `EGL_ANDROID_native_fence_sync`; `--out FILE` writes the GL/EGL strings. Headers: pinned libglvnd 1.7.0 in
  `build/libglvnd`. `make eglgles` (host), `make eglgles-android eglgles-android32` (NDK; staged as `eglgles`,
  `eglgles32`); in the guest `NVP_PROBE=eglgles nvprobe-sphal.sh` (or `NVP_ABI=32 NVP_PROBE=eglgles32`).
- `nvprobe32`, `nvexts32` (NDK i686): the same probes as 32-bit processes (`make nvprobe32 nvexts32`).
- `glesapp/` (NDK, `make glesapp-<abi>.apk` for `x86`, `x86_64`, `armeabi-v7a`): a NativeActivity GLES2 app
  without Java code, a rotating triangle drawn with EGL on the activity window; logs (tag `glesapp`)
  `GL_RENDERER`/`GL_VERSION` and the frame rate. The x86 build is the 32-bit GLES process for P6 (ANGLE x86
  → 32-bit HAL → i386 ICD); the armeabi-v7a build runs through the ARM translator. Signed with a throwaway
  key made at build time; a test app, never shipped.
