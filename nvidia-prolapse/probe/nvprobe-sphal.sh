#!/system/bin/sh
# nvprobe-sphal.sh — P2 gate runner (inside the guest, as root).
#
# Runs nvprobe the way an app would reach the driver:
#   - nvprobe loads the ICD by *soname* through android_load_sphal_library() (the sphal linker
#     namespace; its search paths must hold the ICD, the blobs and the glibc-named shims);
#     NVP_BOOTSTRAP selects the probe's bootstrap (default "--sphal libEGL_nvidia.so.0").
#     With NVP_BOOTSTRAP="--hal vulkan.nvidia.so" (G3b) nvprobe instead loads the Vulkan HAL
#     the same way libvulkan does (sphal namespace, dlsym HMI, open "vk0") and runs the compute
#     through the HAL's GetInstanceProcAddr; $LIBDIR/hw/vulkan.nvidia.so must then exist;
#   - as an app uid (default 10200) with bionic's app seccomp filter installed (appsandbox);
#   - with no LD_LIBRARY_PATH and no NVGLIBC_GLIBC_VERSION, so the shims must find their
#     host-glibc-version sidecar themselves.
# This script must live in a directory the linker treats as a *system* binary location
# (e.g. /data/local/tests/system), so nvprobe runs under the [system] section like app_process.
#
# NVP_ABI=32 runs the 32-bit (i686) build nvprobe32 against /vendor_extra/lib instead (P6); appsandbox
# stays the 64-bit one: bionic's app filter is dual-arch and its x86 half applies to the 32-bit program.
# Env: NVP_ABI (64 default, or 32), NVP_PROBE (default nvprobe/nvprobe32), NVP_LIBDIR (default /vendor_extra/lib64, or /vendor_extra/lib for
#      NVP_ABI=32), NVP_UID, NVP_SANDBOX_FLAGS (e.g. --no-seccomp),
#      NVP_BOOTSTRAP (default "--sphal libEGL_nvidia.so.0"; e.g. "--hal vulkan.nvidia.so"),
#      NVGLIBC_TRACE=1, VKCOMPUTE_HOLD=1 (pass through to the probe).
D=$(cd "$(dirname "$0")" && pwd)
case "${NVP_ABI:-64}" in
    64) PROBE=nvprobe; LIBDIR=${NVP_LIBDIR:-/vendor_extra/lib64} ;;
    32) PROBE=nvprobe32; LIBDIR=${NVP_LIBDIR:-/vendor_extra/lib} ;;
    *) echo "nvprobe-sphal.sh: NVP_ABI must be 64 or 32" >&2; exit 90 ;;
esac
# NVP_PROBE: another probe in this directory with the same bootstrap flags, e.g. eglgles / eglgles32 (P7 G7a:
# NVIDIA's EGL + GLES without libglvnd); must match NVP_ABI
PROBE=${NVP_PROBE:-$PROBE}
die() { echo "nvprobe-sphal.sh: $*" >&2; exit 90; }
[ -x "$D/$PROBE" ] || die "missing $D/$PROBE"
[ -x "$D/appsandbox" ] || die "missing $D/appsandbox"
case "$D" in /data/local/tests/system*|/system/bin*) ;; *) die "$D is not a [system]-section location (use /data/local/tests/system)";; esac
[ -e "$LIBDIR/libEGL_nvidia.so.0" ] || die "missing $LIBDIR/libEGL_nvidia.so.0 (is /vendor_extra mounted?)"
[ -f "$LIBDIR/libnvglibc.so" ] || die "missing $LIBDIR/libnvglibc.so (shims not deployed)"
[ -s "$LIBDIR/host-glibc-version" ] || die "missing $LIBDIR/host-glibc-version"
case "${NVP_BOOTSTRAP:-}" in
    --hal*) [ -f "$LIBDIR/hw/vulkan.nvidia.so" ] || die "missing $LIBDIR/hw/vulkan.nvidia.so (HAL not deployed; run dev/native-deploy)";;
esac
for b in "$LIBDIR"/libnvidia-*.so.[0-9]*; do [ -s "$b" ] || die "$b is empty: the host blob is not bind-mounted over the placeholder"; done
if [ -z "$NVP_SKIP_NODE_CHECK" ]; then
    for n in /dev/nvidiactl /dev/nvidia0 /dev/nvidia-modeset; do [ -c "$n" ] || die "missing device node $n"; done
fi
unset LD_LIBRARY_PATH NVGLIBC_GLIBC_VERSION
echo "nvprobe-sphal.sh: $PROBE, libdir $LIBDIR, glibc sidecar $(cat "$LIBDIR/host-glibc-version"), uid ${NVP_UID:-10200}, flags '${NVP_SANDBOX_FLAGS:-}'" >&2
[ -n "${NVP_BOOTSTRAP:-}" ] && echo "nvprobe-sphal.sh: bootstrap '$NVP_BOOTSTRAP'" >&2
exec "$D/appsandbox" ${NVP_UID:+--uid "$NVP_UID" --gid "$NVP_UID"} ${NVP_SANDBOX_FLAGS:-} -- "$D/$PROBE" ${NVP_BOOTSTRAP:---sphal libEGL_nvidia.so.0} "$@"
