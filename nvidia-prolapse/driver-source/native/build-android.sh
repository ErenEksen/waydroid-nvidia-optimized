#!/usr/bin/env bash
set -euo pipefail
native_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$native_dir/../../.." && pwd)
ndk_dir=${ANDROID_NDK_HOME:-$repo_dir/build/hwcomposer-work/ndk/android-ndk-r29}
clang_dir=$ndk_dir/toolchains/llvm/prebuilt/linux-x86_64/bin
evidence_dir=${NV_NATIVE_EVIDENCE_DIR:-$repo_dir/build/native-evidence}
mkdir -p "$evidence_dir"
if [[ ! -x "$clang_dir/x86_64-linux-android33-clang" ]]; then
    echo "Missing NDK x86_64 API33 compiler; set ANDROID_NDK_HOME" >&2
    exit 1
fi
python3 "$native_dir/compiler/build.py" --target android
glslangValidator -V --target-env vulkan1.0 "$native_dir/shaders/pattern.comp" -o "$native_dir/shaders/pattern.spv"
spirv-val --target-env vulkan1.0 "$native_dir/shaders/pattern.spv"
export CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER=$clang_dir/x86_64-linux-android33-clang
cargo build --offline --release --manifest-path "$native_dir/Cargo.toml" --target x86_64-linux-android
artifact=$native_dir/target/x86_64-linux-android/release
cp "$repo_dir/build/nak-android/src/nouveau/native-bridge/libnv_native_nak.so" "$artifact/libnv_native_nak.so"
"$clang_dir/x86_64-linux-android33-clang" -std=c11 -Wall -Wextra -Werror     "$repo_dir/tools/native-backend-probe.c" -ldl -o "$artifact/native-backend-probe"
"$clang_dir/llvm-readelf" -h -l -d "$artifact/nv-native-check" > "$evidence_dir/native-android-elf.txt"
if ! rg -q '/system/bin/linker64' "$evidence_dir/native-android-elf.txt"; then
    echo "Artifact does not use Android linker64" >&2; exit 1
fi
if rg -q 'NEEDED.*(libc\.so\.6|libnvglibc|libnvidia|libEGL_nvidia)' "$evidence_dir/native-android-elf.txt"; then
    echo "Unexpected glibc/shim/NVIDIA userspace dependency" >&2; exit 1
fi
"$clang_dir/llvm-readelf" --version-info "$artifact/nv-native-check" > "$evidence_dir/native-android-versions.txt"
if rg -q 'GLIBC_' "$evidence_dir/native-android-versions.txt"; then
    echo "Unexpected GLIBC symbol version requirement" >&2; exit 1
fi
for name in libnv_bionic_native.so libnv_native_nak.so native-backend-probe; do
    "$clang_dir/llvm-readelf" -h -d --version-info "$artifact/$name" > "$evidence_dir/$name.elf.txt"
    if rg -q 'NEEDED.*(libc\.so\.6|libnvglibc|libnvidia|libEGL_nvidia)|GLIBC_' "$evidence_dir/$name.elf.txt"; then
        echo "Unexpected dependency or glibc version in $name" >&2; exit 1
    fi
done
"$clang_dir/llvm-nm" -D --defined-only "$artifact/libnv_bionic_native.so" > "$evidence_dir/native-android-exports.txt"
rg -q ' T nv_native_backend_check$' "$evidence_dir/native-android-exports.txt"
echo "Built bionic executable and library in $artifact"
