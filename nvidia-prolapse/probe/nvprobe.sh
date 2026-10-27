#!/system/bin/sh
# nvprobe.sh — run the compute probe against NVIDIA's ICD inside the guest (P1 spike).
# Layout under $NVP (default /data/local/tmp/nvp): bin/nvprobe, lib/<the five host blobs>,
# shim/<libnvglibc.so + six glibc-named fronts>, host-glibc-version (written by dev/deploy-guest).
# Fails loudly on every missing piece; never falls back.
NVP=${NVP:-/data/local/tmp/nvp}
die() { echo "nvprobe.sh: $*" >&2; exit 90; }
[ -x "$NVP/bin/nvprobe" ] || die "missing $NVP/bin/nvprobe (run dev/deploy-guest on the host)"
[ -e "$NVP/lib/libEGL_nvidia.so.0" ] || die "missing $NVP/lib/libEGL_nvidia.so.0 (host blobs not staged)"
for f in libnvglibc.so libc.so.6 libm.so.6 libpthread.so.0 librt.so.1 libdl.so.2 ld-linux-x86-64.so.2; do
    [ -e "$NVP/shim/$f" ] || die "missing shim $NVP/shim/$f (build shim/ and deploy)"
done
[ -s "$NVP/host-glibc-version" ] || die "missing $NVP/host-glibc-version (dev/deploy-guest writes it from the host's getconf GNU_LIBC_VERSION)"
for d in /dev/nvidiactl /dev/nvidia0 /dev/nvidia-modeset; do [ -c "$d" ] || die "missing device node $d (mknod it in the guest's /dev, or bind-mount it)"; done
export NVGLIBC_GLIBC_VERSION=$(cat "$NVP/host-glibc-version")
export LD_LIBRARY_PATH="$NVP/shim:$NVP/lib"
echo "nvprobe.sh: pid $$, glibc-compat says glibc $NVGLIBC_GLIBC_VERSION, LD_LIBRARY_PATH=$LD_LIBRARY_PATH" >&2
exec "$NVP/bin/nvprobe" --icd "$NVP/lib/libEGL_nvidia.so.0" "$@"
