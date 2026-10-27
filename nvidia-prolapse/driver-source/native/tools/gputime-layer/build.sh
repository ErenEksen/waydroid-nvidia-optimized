#!/usr/bin/env bash
# Builds the layer for the host (x86_64-unknown-linux-gnu) and the guest (x86_64-linux-android, NDK clang as
# linker), offline. Output: $OUT/host/{libVkLayer_PERF_gputime.so,VkLayer_PERF_gputime.json,gputime-compare}
# and $OUT/android/libVkLayer_PERF_gputime.so (Android's loader finds layers by the libVkLayer_*.so file name
# in the app's layer search path; it reads no manifest).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
OUT=${OUT:?set OUT to an output directory}
export CARGO_TARGET_DIR=${CARGO_TARGET_DIR:-$OUT/target}
ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME (NDK r29 or newer)}
clang=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang
[[ -x $clang ]] || { echo "missing $clang" >&2; exit 1; }
cargo build --offline --release --manifest-path "$here/Cargo.toml"
CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER=$clang \
    cargo build --offline --release --manifest-path "$here/Cargo.toml" --target x86_64-linux-android --lib
mkdir -p "$OUT/host" "$OUT/android"
cp "$CARGO_TARGET_DIR/release/libVkLayer_PERF_gputime.so" "$OUT/host/"
cp "$here/VkLayer_PERF_gputime.json" "$OUT/host/"
cp "$CARGO_TARGET_DIR/release/gputime-compare" "$OUT/host/"
cp "$CARGO_TARGET_DIR/x86_64-linux-android/release/libVkLayer_PERF_gputime.so" "$OUT/android/"
strip="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
"$strip" --strip-debug "$OUT/android/libVkLayer_PERF_gputime.so"
readelf=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf
if "$readelf" -d --version-info "$OUT/android/libVkLayer_PERF_gputime.so" | grep -qE 'libc\.so\.6|GLIBC_'; then
    echo "android build depends on glibc" >&2; exit 1
fi
echo "built $OUT/host and $OUT/android"
