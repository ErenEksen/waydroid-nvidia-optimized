#!/usr/bin/env bash
# hwcomposer/provision.sh [stage...] — fetch and build every input of hwcomposer/build.sh from the
# pins in hwcomposer/pins.env. Adapted from build/hwcomposer/provision.sh (MIT).
#
#   stages: ndk clones hidlgen hidlout staticdeps protocols imagelibs   (default: all, in that order)
#
# Work tree: $HWC_WORK (default <repo>/build/hwcomposer-work, gitignored):
#   ndk/                     the pinned NDK (r29)
#   deps/                    AOSP header clones, hidl-gen + its output, static wayland/ffi/xkbcommon
#                            for bionic, libdrm headers, wayland-protocols, image link stubs
#   src/                     android_hardware_waydroid at HWC_SHA (patched by build.sh, not here)
# Every download is sha256-checked against pins.env; a missing or wrong pin fails loudly.
# Each stage is idempotent (a .provisioned stamp holds the pin it was made from).
set -euo pipefail
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
# shellcheck source=pins.env
source "$HERE/pins.env"
WORK=${HWC_WORK:-$REPO/build/hwcomposer-work}
D=$WORK/deps
NDK=$WORK/ndk/android-ndk-$NDK_VERSION
API=30
TARGET=x86_64-linux-android
mkdir -p "$D/dl"

die() { echo "provision: $*" >&2; exit 1; }
STAGES=("$@"); [ $# -gt 0 ] || STAGES=(ndk clones hidlgen hidlout staticdeps protocols imagelibs)
has_stage() { local s; for s in "${STAGES[@]}"; do [ "$s" = "$1" ] && return 0; done; return 1; }

# fetch URL SHA256 OUT [ALT_URL...]: download once (tools/fetch.py: alternative sources, retries with backoff,
# stalled or trickling sources given up, kept only when the hash matches), verify always
fetch() {
    local url=$1 sha=$2 out=$3; shift 3
    [ -n "$sha" ] || die "no sha256 pin for $url in pins.env"
    python3 "$REPO/tools/fetch.py" --out "$out" --sha256 "$sha" "$url" "$@" || die "download failed: $url $*"
    echo "$sha  $out" | sha256sum -c --quiet - || die "sha256 mismatch for $out (pin $sha)"
}
net() { python3 "$REPO/tools/fetch.py" retry -- "$@"; }   # a network command, retried with backoff
# git_pin DIR URL SHA: shallow fetch of exactly one commit
git_pin() {
    local dir=$1 url=$2 sha=$3
    if [ -e "$dir/.provisioned" ] && [ "$(cat "$dir/.provisioned")" = "$sha" ]; then return 0; fi
    rm -rf "$dir"; git init -q "$dir"
    net git -C "$dir" fetch -q --depth 1 "$url" "$sha"
    git -C "$dir" checkout -q FETCH_HEAD
    [ "$(git -C "$dir" rev-parse HEAD)" = "$sha" ] || die "$dir: HEAD is not $sha"
    echo "$sha" > "$dir/.provisioned"
    echo "  $(basename "$dir"): $sha"
}

if has_stage ndk; then
    echo "== stage ndk ($NDK_VERSION)"
    if [ ! -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]; then
        fetch "$NDK_URL" "$NDK_ZIP_SHA256" "$WORK/dl-ndk-$NDK_VERSION.zip"
        mkdir -p "$WORK/ndk"; unzip -q "$WORK/dl-ndk-$NDK_VERSION.zip" -d "$WORK/ndk"
    fi
    grep -q "Pkg.ReleaseName = $NDK_VERSION" "$NDK/source.properties" || die "$NDK is not $NDK_VERSION"
fi
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64

if has_stage clones; then
    echo "== stage clones"
    git_pin "$D/src"                     "$HWC_UPSTREAM"                      "$HWC_SHA"
    git_pin "$D/aosp_fmq"                "$AOSP_BASE/system/libfmq"            "$AOSP_FMQ_SHA"
    git_pin "$D/aosp_fmtlib"             "$AOSP_BASE/external/fmtlib"          "$AOSP_FMTLIB_SHA"
    git_pin "$D/aosp_frameworks_native"  "$AOSP_BASE/frameworks/native"        "$AOSP_FRAMEWORKS_NATIVE_SHA"
    git_pin "$D/aosp_hardware_interfaces" "$AOSP_BASE/hardware/interfaces"     "$AOSP_HARDWARE_INTERFACES_SHA"
    git_pin "$D/aosp_hidl"               "$AOSP_BASE/system/tools/hidl"        "$AOSP_HIDL_SHA"
    git_pin "$D/aosp_libbase"            "$AOSP_BASE/system/libbase"           "$AOSP_LIBBASE_SHA"
    git_pin "$D/aosp_libhardware"        "$AOSP_BASE/hardware/libhardware"     "$AOSP_LIBHARDWARE_SHA"
    git_pin "$D/aosp_libhidl"            "$AOSP_BASE/system/libhidl"           "$AOSP_LIBHIDL_SHA"
    git_pin "$D/aosp_libhwbinder"        "$AOSP_BASE/system/libhwbinder"       "$AOSP_LIBHWBINDER_SHA"
    git_pin "$D/aosp_system_core"        "$AOSP_BASE/system/core"              "$AOSP_SYSTEM_CORE_SHA"
    git_pin "$D/android_system_logging"  "$SYSTEM_LOGGING_URL"                 "$SYSTEM_LOGGING_SHA"
    git_pin "$D/minigbm"                 "$MINIGBM_UPSTREAM"                   "$MINIGBM_SHA"
fi

if has_stage hidlgen; then
    echo "== stage hidlgen"
    cmake -S "$HERE/hidl-gen" -B "$D/hidl-gen-build" -DCMAKE_BUILD_TYPE=Release -DWNV="$D" >/dev/null
    cmake --build "$D/hidl-gen-build" -j"$(nproc)" >/dev/null
fi

if has_stage hidlout; then
    echo "== stage hidlout"
    HG=$D/hidl-gen-build/hidl-gen
    [ -x "$HG" ] || die "hidl-gen not built (stage hidlgen)"
    rm -rf "$D/hidl-out"; mkdir -p "$D/hidl-out"
    ROOTS=(-r "vendor.waydroid:$D/src/interfaces"
           -r "android.hidl:$D/aosp_libhidl/transport"
           -r "android.hardware:$D/aosp_hardware_interfaces")
    for pkg in vendor.waydroid.clipboard@1.0 \
               vendor.waydroid.display@1.0 vendor.waydroid.display@1.1 vendor.waydroid.display@1.2 \
               vendor.waydroid.task@1.0 \
               vendor.waydroid.window@1.0 vendor.waydroid.window@1.1 vendor.waydroid.window@1.2 \
               android.hidl.base@1.0 \
               android.hidl.manager@1.0 android.hidl.manager@1.1 android.hidl.manager@1.2 \
               android.hidl.memory@1.0 \
               android.hardware.graphics.common@1.0 android.hardware.graphics.common@1.1 \
               android.hardware.graphics.common@1.2 \
               android.hardware.graphics.composer@2.1; do
        "$HG" -o "$D/hidl-out" -L c++-headers "${ROOTS[@]}" "$pkg"
    done
    echo "  $(find "$D/hidl-out" -name '*.h' | wc -l) headers"
fi

if has_stage staticdeps; then
    echo "== stage staticdeps"
    PREFIX=$D/ndk-prefix NATPREFIX=$D/native-prefix
    mkdir -p "$PREFIX"
    export CC="$TC/bin/${TARGET}${API}-clang" CXX="$TC/bin/${TARGET}${API}-clang++"
    export AR="$TC/bin/llvm-ar" RANLIB="$TC/bin/llvm-ranlib" STRIP="$TC/bin/llvm-strip"
    if [ ! -e "$PREFIX/lib/libffi.a" ]; then
        fetch "$LIBFFI_URL" "$LIBFFI_SHA256" "$D/dl/libffi-$LIBFFI_VERSION.tar.gz"
        rm -rf "$D/build-libffi"; mkdir -p "$D/build-libffi"
        tar -C "$D/build-libffi" --strip-components=1 -xzf "$D/dl/libffi-$LIBFFI_VERSION.tar.gz"
        (cd "$D/build-libffi" && ./configure --host=$TARGET --prefix="$PREFIX" \
            --disable-shared --enable-static --disable-docs >/dev/null && \
            make -j"$(nproc)" >/dev/null && make install >/dev/null)
        echo "  libffi $LIBFFI_VERSION"
    fi
    CROSS=$D/ndk-cross.ini
    cat > "$CROSS" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'
[built-in options]
pkg_config_path = '$PREFIX/lib/pkgconfig'
[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    if [ ! -e "$PREFIX/lib/libwayland-client.a" ] || [ ! -x "$NATPREFIX/bin/wayland-scanner" ]; then
        fetch "$WAYLAND_URL" "$WAYLAND_SHA256" "$D/dl/wayland-$WAYLAND_VERSION.tar.xz"
        rm -rf "$D/build-wayland"; mkdir -p "$D/build-wayland"
        tar -C "$D/build-wayland" --strip-components=1 -xJf "$D/dl/wayland-$WAYLAND_VERSION.tar.xz"
        # native scanner from the same tarball (the cross build needs scanner >= target)
        env -u CC -u CXX -u AR -u RANLIB -u STRIP \
            meson setup "$D/build-wayland/native" "$D/build-wayland" \
            -Dprefix="$NATPREFIX" -Dlibraries=false -Dscanner=true \
            -Ddocumentation=false -Dtests=false -Ddtd_validation=false >/dev/null
        env -u CC -u CXX -u AR -u RANLIB -u STRIP ninja -C "$D/build-wayland/native" install >/dev/null
        export PKG_CONFIG_PATH_FOR_BUILD="$NATPREFIX/lib/pkgconfig:$NATPREFIX/share/pkgconfig:$NATPREFIX/lib/x86_64-linux-gnu/pkgconfig"
        PATH="$NATPREFIX/bin:$PATH" meson setup "$D/build-wayland/b" "$D/build-wayland" --cross-file "$CROSS" \
            -Ddefault_library=static -Dprefix="$PREFIX" \
            -Ddocumentation=false -Dtests=false -Ddtd_validation=false -Dscanner=false >/dev/null
        PATH="$NATPREFIX/bin:$PATH" ninja -C "$D/build-wayland/b" install >/dev/null
        echo "  wayland $WAYLAND_VERSION (+ native scanner)"
    fi
    if [ ! -e "$PREFIX/lib/libxkbcommon.a" ]; then
        fetch "$XKBCOMMON_URL" "$XKBCOMMON_SHA256" "$D/dl/libxkbcommon-$XKBCOMMON_VERSION.tar.xz"
        rm -rf "$D/build-xkb"; mkdir -p "$D/build-xkb"
        tar -C "$D/build-xkb" --strip-components=1 -xJf "$D/dl/libxkbcommon-$XKBCOMMON_VERSION.tar.xz"
        meson setup "$D/build-xkb/b" "$D/build-xkb" --cross-file "$CROSS" \
            -Ddefault_library=static -Dprefix="$PREFIX" \
            -Dxkb-config-root=/usr/share/X11/xkb -Dxkb-config-extra-path=/etc/xkb \
            -Dx-locale-root=/usr/share/X11/locale \
            -Denable-x11=false -Denable-wayland=false -Denable-docs=false \
            -Denable-tools=false -Denable-xkbregistry=false >/dev/null
        # Config roots are fixed (upstream's /usr defaults) so the binary does not depend on the
        # build host's xkeyboard-config or carry the build path. None exist in the guest; hwc only
        # compiles the keymap text the compositor sends (as before: the baseline's roots were
        # build-host paths). Library target only: xkbcommon 1.7's tests need API-31 ICU.
        ninja -C "$D/build-xkb/b" libxkbcommon.a >/dev/null
        install -Dm644 "$D/build-xkb/b/libxkbcommon.a" "$PREFIX/lib/libxkbcommon.a"
        mkdir -p "$PREFIX/include"; cp -r "$D/build-xkb/include/xkbcommon" "$PREFIX/include/"
        echo "  xkbcommon $XKBCOMMON_VERSION (library only)"
    fi
    if [ ! -d "$D/libdrm-$LIBDRM_VERSION" ]; then
        fetch "$LIBDRM_URL" "$LIBDRM_SHA256" "$D/dl/libdrm-$LIBDRM_VERSION.tar.xz"
        tar -C "$D" -xJf "$D/dl/libdrm-$LIBDRM_VERSION.tar.xz"
        echo "  libdrm headers $LIBDRM_VERSION"
    fi
fi

if has_stage protocols; then
    echo "== stage protocols (wayland-protocols $WAYLAND_PROTOCOLS_VERSION, scanner $WAYLAND_VERSION)"
    SCANNER=$D/native-prefix/bin/wayland-scanner
    [ -x "$SCANNER" ] || die "native wayland-scanner missing (stage staticdeps)"
    fetch "$WAYLAND_PROTOCOLS_URL" "$WAYLAND_PROTOCOLS_SHA256" "$D/dl/wayland-protocols-$WAYLAND_PROTOCOLS_VERSION.tar.xz"
    WP=$D/wayland-protocols-$WAYLAND_PROTOCOLS_VERSION
    [ -d "$WP" ] || tar -C "$D" -xJf "$D/dl/wayland-protocols-$WAYLAND_PROTOCOLS_VERSION.tar.xz"
    G=$D/gen; rm -rf "$G"; mkdir -p "$G"
    gen() { "$SCANNER" private-code "$2" "$G/$1-protocol.c"; "$SCANNER" client-header "$2" "$G/$1-client-protocol.h"; }
    gen wayland-android                   "$D/src/hwcomposer/wayland-android.xml"
    gen xdg-shell                         "$WP/stable/xdg-shell/xdg-shell.xml"
    gen viewporter                        "$WP/stable/viewporter/viewporter.xml"
    gen presentation-time                 "$WP/stable/presentation-time/presentation-time.xml"
    gen linux-dmabuf-unstable-v1          "$WP/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"
    gen tablet-unstable-v2                "$WP/unstable/tablet/tablet-unstable-v2.xml"
    gen relative-pointer-unstable-v1      "$WP/unstable/relative-pointer/relative-pointer-unstable-v1.xml"
    gen pointer-constraints-unstable-v1   "$WP/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml"
    gen fractional-scale-v1               "$WP/staging/fractional-scale/fractional-scale-v1.xml"
    gen idle-inhibit-unstable-v1          "$WP/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
    gen xdg-decoration-unstable-v1        "$WP/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
    echo "  $(ls "$G" | wc -l) files"
fi

if has_stage imagelibs; then
    echo "== stage imagelibs"
    R=$D/image-rootfs
    if [ ! -e "$R/.provisioned" ]; then
        IMGDIR=${HWC_IMAGES:-}
        if [ -z "$IMGDIR" ] && [ -r /var/lib/waydroid/waydroid.cfg ]; then
            IMGDIR=$(sed -n 's/^images_path *= *//p' /var/lib/waydroid/waydroid.cfg | head -1)
        fi
        if [ -n "$IMGDIR" ] && [ -r "$IMGDIR/system.img" ] && [ -r "$IMGDIR/vendor.img" ]; then
            SYSIMG=$IMGDIR/system.img VENIMG=$IMGDIR/vendor.img
            echo "  link stubs from the local images in $IMGDIR"
        else
            for part in system vendor; do
                u=IMG_${part^^}_URL s=IMG_${part^^}_SHA256 a=IMG_${part^^}_URL_ALT
                fetch "${!u}" "${!s}" "$D/dl/$part.zip" "${!a}"
                rm -rf "$D/dl/$part-img"; unzip -oq "$D/dl/$part.zip" -d "$D/dl/$part-img"
            done
            SYSIMG=$(ls "$D"/dl/system-img/*.img) VENIMG=$(ls "$D"/dl/vendor-img/*.img)
            echo "  link stubs from the pinned OTA images"
        fi
        rm -rf "$R"; mkdir -p "$R/system" "$R/apex/com.android.vndk.v33" "$R/vendor"
        debugfs -R "rdump /system/lib64 $R/system" "$SYSIMG" 2>&1 | grep -v -e '^debugfs' -e 'Operation not permitted while changing ownership' || true
        debugfs -R "rdump /system/apex/com.android.vndk.v33/lib64 $R/apex/com.android.vndk.v33" "$SYSIMG" 2>&1 | grep -v -e '^debugfs' -e 'Operation not permitted while changing ownership' || true
        debugfs -R "rdump /lib64 $R/vendor" "$VENIMG" 2>&1 | grep -v -e '^debugfs' -e 'Operation not permitted while changing ownership' || true
        for lib in "$R/vendor/lib64/vendor.waydroid.display@1.0.so" "$R/system/lib64/libhidlbase.so"; do
            [ -f "$lib" ] || die "expected $lib missing after extraction"
        done
        touch "$R/.provisioned"
    fi
    echo "  $(find "$R" -name '*.so' | wc -l) shared objects"
fi
echo "== provisioned: $D"
