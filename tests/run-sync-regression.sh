#!/usr/bin/env bash
# Isolated GPU/transport test. Never touches the installed renderer/container.
set -euo pipefail
ulimit -c 0
REPO="$(cd "$(dirname "$0")/.." && pwd)"
source "$REPO/dev/env.sh"
OUT="${SYNC_TEST_OUT:-$WNV/sync-regression-$(date +%Y%m%d-%H%M%S)}"
OUT="$(realpath -m "$OUT")"
mkdir -p "$OUT"
ICD="$WNV/mesa-host/src/virtio/vulkan/virtio_devenv_icd.x86_64.json"
[[ -r "$ICD" ]] || die 'build the host Venus ICD first'
cc -O2 -Wall -Wextra -Werror "$REPO/tests/sync-import.c" -lvulkan -o "$OUT/sync-import"
SOCK_DIR="$(mktemp -d /tmp/nvwd-sync.XXXXXX)"
SOCK="$SOCK_DIR/venus.sock"
SRV=
EXTRA_LIBS=
if [[ -n "${NVWD_FAULT:-}" ]]; then
    mkdir -p "$OUT/fault-loader"
    cc -O2 -shared -fPIC -Wall -Wextra -Werror "$REPO/tests/fault-vulkan.c" -ldl -pthread -o "$OUT/fault-loader/libvulkan.so.1"
    EXTRA_LIBS="$OUT/fault-loader:"
    : "${NVWD_REAL_VULKAN:=$(ldconfig -p | awk '/libvulkan.so.1 .*x86-64/{print $NF; exit}')}"
    export NVWD_REAL_VULKAN
fi
cleanup() {
    if [[ -n "$SRV" ]]; then kill -- "-$SRV" 2>/dev/null || true; wait "$SRV" 2>/dev/null || true; fi
    rm -f "$SOCK"; rmdir "$SOCK_DIR" 2>/dev/null || true
}
trap cleanup EXIT
mkdir -p "$OUT/cache/nvidia"
XDG_CACHE_HOME="$OUT/cache" __GL_SHADER_DISK_CACHE_PATH="$OUT/cache/nvidia" \
VTEST_STATS="${VTEST_STATS:-0}" RENDER_SERVER_EXEC_PATH="${SYNC_RENDER_BIN:-$VIRGL_BUILD/server/virgl_render_server}" \
LD_LIBRARY_PATH="$EXTRA_LIBS${SYNC_LIB_DIR:-$VIRGL_BUILD/src}" \
setsid "${SYNC_SERVER_BIN:-$VIRGL_BUILD/vtest/virgl_test_server}" --venus --multi-clients --socket-path "$SOCK" > "$OUT/server.log" 2>&1 &
SRV=$!
for ((i=0;i<100;i++)); do [[ -S "$SOCK" ]] && break; kill -0 "$SRV"; sleep .1; done
[[ -S "$SOCK" ]] || die 'test server not ready'
run_case() {
    local name="$1";shift
    local runner=(timeout 90)
    if [[ "${SYNC_GDB:-0}" == 1 ]]; then
        runner=(timeout --preserve-status -s INT -k 10 20 gdb -q -batch
                -ex 'set pagination off' -ex run -ex 'thread apply all bt 20' --args)
    fi
    env VK_ICD_FILENAMES="$ICD" VN_DEBUG=vtest VTEST_SOCKET_NAME="$SOCK" "$@" "${runner[@]}" \
        "$OUT/sync-import" "${SYNC_ROUNDS:-1000}" > "$OUT/$name.log" 2>&1
    grep -q "^PASS: ${SYNC_ROUNDS:-1000} create/import/submit/export cycles" "$OUT/$name.log"
    tail -2 "$OUT/$name.log"
}
for case_name in ${SYNC_CASES:-timeline socket cpu-fallback concurrent}; do
    case "$case_name" in
        timeline) run_case timeline ;;
        socket) run_case socket VTEST_NO_TIMELINE_FENCE=1 ;;
        cpu-fallback) run_case cpu-fallback VTEST_NO_SEMAPHORE_IMPORT=1 ;;
        multiqueue) run_case multiqueue NVWD_SYNC_QUEUES=2 ;;
        compute)
            cc -O2 "$REPO/tests/vnprobe.c" -lvulkan -o "$OUT/vnprobe"
            for attempt in 1 2 3; do
                env VK_ICD_FILENAMES="$ICD" VN_DEBUG=vtest VTEST_SOCKET_NAME="$SOCK" \
                    timeout 30 "$OUT/vnprobe" "$REPO/tests/vnprobe.spv" > "$OUT/compute-$attempt.log" 2>&1
                grep '^PASS:' "$OUT/compute-$attempt.log"
                find "$OUT/cache" -type f -printf '%P %s %T@\n' > "$OUT/cache-$attempt.txt"
            done
            env VK_ICD_FILENAMES="$ICD" VN_DEBUG=vtest VTEST_SOCKET_NAME="$SOCK" \
                python3 "$REPO/tests/cache-access.py" "$OUT/cache" "$OUT/vnprobe" "$REPO/tests/vnprobe.spv" > "$OUT/cache-read.json"
            echo 'PASS: existing renderer cache files accessed on a new Vulkan client' ;;
        allocator)
            cc -O2 "$REPO/tests/nvalloc.c" -lEGL -lGLESv2 -o "$OUT/nvalloc"
            timeout 30 "$OUT/nvalloc" "$SOCK" > "$OUT/allocator.log" 2>&1
            cat "$OUT/allocator.log" ;;
        concurrent)
            run_case concurrent-a & a=$!
            run_case concurrent-b & b=$!
            wait "$a"; wait "$b" ;;
        *) die "unknown test case $case_name" ;;
    esac
done
if [[ -z "${NVWD_FAULT:-}" ]] && grep -Ei 'bad object ids|failed|fatal' "$OUT/server.log"; then
    die "renderer errors: $OUT/server.log"
fi
printf 'PASS: isolated cases: %s\n' "${SYNC_CASES:-timeline socket cpu-fallback concurrent}" | tee "$OUT/summary.txt"
