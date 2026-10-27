#!/usr/bin/env bash
# hwcomposer/build.sh [OUTDIR] — build hwcomposer.waydroid.so (x86_64 vendor HAL) from
# android_hardware_waydroid at HWC_SHA + hwcomposer/patches/*.patch, standalone with the pinned NDK
# (no AOSP build system). Inputs come from hwcomposer/provision.sh. Adapted from
# build/hwcomposer/build.sh (MIT): same sources, flags and link line.
#
# Env:
#   HWC_PATCHES_UPTO=NNNN  apply patches/0001..NNNN only (0001 alone = the baseline reproduction)
#   HWC_SRC=DIR            build an already-patched tree instead (dev A/B; nothing is applied)
#   HWC_WORK               work tree (default <repo>/build/hwcomposer-work)
#
# Outputs in OUTDIR (default $HWC_WORK/out): hwcomposer.waydroid.so (unstripped, for addr2line),
# hwcomposer.waydroid.stripped.so (what is deployed), PATCHES (what went in).
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=pins.env
source "$HERE/pins.env"
WORK=${HWC_WORK:-$REPO/build/hwcomposer-work}
D=$WORK/deps
OUT=$(mkdir -p "${1:-$WORK/out}" && cd "${1:-$WORK/out}" && pwd)
BIN=$WORK/ndk/android-ndk-$NDK_VERSION/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX=$BIN/x86_64-linux-android34-clang++
CC=$BIN/x86_64-linux-android34-clang
STRIP=$BIN/llvm-strip
R=$D/image-rootfs
die() { echo "hwcomposer/build.sh: $*" >&2; exit 1; }

[ -x "$CXX" ] || die "NDK $NDK_VERSION missing: run hwcomposer/provision.sh"
for p in "$D/src/.provisioned" "$D/hidl-out" "$D/gen/wayland-android-protocol.c" \
         "$D/ndk-prefix/lib/libwayland-client.a" "$R/.provisioned"; do
    [ -e "$p" ] || die "$p missing: run hwcomposer/provision.sh"
done
[ "$(cat "$D/src/.provisioned")" = "$HWC_SHA" ] || die "$D/src is not at HWC_SHA $HWC_SHA: re-run provision.sh clones"

if [ -n "${HWC_SRC:-}" ]; then
    TREE=$(cd "$HWC_SRC" && pwd)
    echo "tree: $TREE (pre-patched, nothing applied)" | tee "$OUT/PATCHES"
else
    TREE=$WORK/tree
    rm -rf "$TREE"; mkdir -p "$TREE"
    git -C "$D/src" archive "$HWC_SHA" | tar -x -C "$TREE"
    # The tree must be its own repository: `git apply` run inside a non-repo directory that sits in
    # another work tree (build/ is inside this repo) silently skips every path. Each patch is
    # committed, so a patch that changes nothing fails below instead of building the base.
    git -C "$TREE" init -q && git -C "$TREE" add -A
    git -C "$TREE" -c user.name=hwc -c user.email=hwc@build commit -qm "base $HWC_SHA"
    : > "$OUT/PATCHES"
    echo "base: $HWC_UPSTREAM $HWC_SHA" >> "$OUT/PATCHES"
    for p in "$HERE"/patches/[0-9][0-9][0-9][0-9]-*.patch; do
        n=$(basename "$p"); n=${n%%-*}
        if [ -n "${HWC_PATCHES_UPTO:-}" ] && [ "$n" \> "$HWC_PATCHES_UPTO" ]; then break; fi
        git -C "$TREE" apply --whitespace=nowarn "$p" || die "patch does not apply: $p"
        git -C "$TREE" add -A
        git -C "$TREE" diff --cached --quiet && die "patch changed nothing: $p"
        git -C "$TREE" -c user.name=hwc -c user.email=hwc@build commit -qm "$(basename "$p")"
        echo "$(sha256sum "$p" | cut -c1-16)  $(basename "$p")" >> "$OUT/PATCHES"
    done
    cat "$OUT/PATCHES"
fi
SRC=$TREE/hwcomposer

INC=(
  -I"$SRC" -I"$D/gen" -I"$HERE/aidl-stubs" -I"$D/hidl-out" -I"$D/ndk-prefix/include"
  -I"$D/aosp_libhidl/transport/include" -I"$D/aosp_libhidl/base/include"
  -I"$D/aosp_libhwbinder/include" -I"$D/aosp_fmq/include" -I"$D/aosp_fmq/base"
  -I"$D/aosp_system_core/libcutils/include" -I"$D/aosp_system_core/libutils/include"
  -I"$D/aosp_system_core/libsync/include" -I"$D/aosp_system_core"
  -I"$D/aosp_system_core/libsystem/include" -I"$D/aosp_system_core/libprocessgroup/include"
  -I"$D/android_system_logging/liblog/include" -I"$D/aosp_libbase/include"
  -I"$D/aosp_frameworks_native/libs/ui/include" -I"$D/aosp_frameworks_native/libs/nativewindow/include"
  -I"$D/aosp_frameworks_native/libs/nativebase/include" -I"$D/aosp_frameworks_native/libs/arect/include"
  -I"$D/aosp_frameworks_native/libs/math/include" -I"$D/aosp_frameworks_native/libs/gui/include"
  -I"$D/aosp_libhardware/include" -I"$D/minigbm"
  -I"$D/libdrm-$LIBDRM_VERSION" -I"$D/libdrm-$LIBDRM_VERSION/include/drm" -I"$D/libdrm-$LIBDRM_VERSION/android"
)
DEFS=(-DLOG_TAG=\"hwcomposer\" -D__ANDROID_VNDK__ -D__ANDROID_USE_LIBLOG_SAFETY_NET=0)
CXXFLAGS=(-O2 -fPIC -std=c++17 -isystem "$HERE/libcxx-override" -Wall -Wno-unused-parameter
          -Wno-deprecated-declarations -fno-exceptions -fno-rtti
          # __FILE__ (ALOG_ASSERT/LOG_ALWAYS_FATAL) without the build host's path
          "-ffile-prefix-map=$TREE/=${HWC_FILE_PREFIX:-}")
XKBCOMMON_A=${HWC_XKBCOMMON_A:-$D/ndk-prefix/lib/libxkbcommon.a}
SRCS=(
  "$HERE/libcxx_compat.cpp"
  "$SRC/extension.cpp" "$SRC/gralloc_handler.cpp" "$SRC/hwcomposer.cpp" "$SRC/wayland-hwc.cpp"
  "$SRC/WaydroidClipboard.cpp" "$SRC/WaydroidWindow.cpp" "$SRC/egl-tools.cpp"
  "$SRC/modes/waydroid_mode.cpp" "$SRC/modes/closed.cpp" "$SRC/modes/full-ui.cpp"
  "$SRC/modes/multi-window.cpp" "$SRC/modes/single-window.cpp"
)
# patches/0014 adds the cross-GPU copy; absent when building fewer patches (HWC_PATCHES_UPTO)
[ -f "$SRC/prime-copy.cpp" ] && SRCS+=("$SRC/prime-copy.cpp")
# the window-following display resize (its own file: the only caller of hwc_procs_t::hotplug)
[ -f "$SRC/display-resize.cpp" ] && SRCS+=("$SRC/display-resize.cpp")
PROTOS=(wayland-android xdg-shell viewporter presentation-time linux-dmabuf-unstable-v1 tablet-unstable-v2
        relative-pointer-unstable-v1 pointer-constraints-unstable-v1 fractional-scale-v1
        idle-inhibit-unstable-v1 xdg-decoration-unstable-v1)

OBJ=$OUT/obj; rm -rf "$OBJ"; mkdir -p "$OBJ"
OBJS=()
for s in "${SRCS[@]}"; do
  o=$OBJ/$(basename "${s%.cpp}").o
  echo "CXX $(basename "$s")"
  "$CXX" "${CXXFLAGS[@]}" "${DEFS[@]}" "${INC[@]}" -c "$s" -o "$o"
  OBJS+=("$o")
done
for p in "${PROTOS[@]}"; do
  o=$OBJ/$p-protocol.o
  echo "CC  $p-protocol.c"
  "$CC" -O2 -fPIC "${INC[@]}" -c "$D/gen/$p-protocol.c" -o "$o"
  OBJS+=("$o")
done

echo "LINK hwcomposer.waydroid.so"
"$CXX" -shared -o "$OUT/hwcomposer.waydroid.so" "${OBJS[@]}" \
  -nostdlib++ \
  "$D/ndk-prefix/lib/libwayland-client.a" "$D/ndk-prefix/lib/libffi.a" "$XKBCOMMON_A" \
  -L"$R/system/lib64" -L"$R/apex/com.android.vndk.v33/lib64" -L"$R/vendor/lib64" \
  -lc++ -llog -lutils -lcutils -lhardware -lhidlbase -lsync -lui -ldrm -lEGL -lGLESv2 \
  -l:vendor.waydroid.clipboard@1.0.so \
  -l:vendor.waydroid.display@1.0.so -l:vendor.waydroid.display@1.1.so -l:vendor.waydroid.display@1.2.so \
  -l:vendor.waydroid.task@1.0.so \
  -l:vendor.waydroid.window@1.0.so -l:vendor.waydroid.window@1.1.so -l:vendor.waydroid.window@1.2.so \
  -Wl,--no-undefined -Wl,-soname,hwcomposer.waydroid.so
"$STRIP" -o "$OUT/hwcomposer.waydroid.stripped.so" "$OUT/hwcomposer.waydroid.so"
sha256sum "$OUT/hwcomposer.waydroid.stripped.so"
ls -l "$OUT/hwcomposer.waydroid.so" "$OUT/hwcomposer.waydroid.stripped.so"
