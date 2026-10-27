#!/usr/bin/env bash
# packaging/native/build-native.sh — build the whole native stack (nvidia-prolapse mode) from a clean
# checkout, then collect the release bundle. Independent targets build concurrently (dependency list below;
# NATIVE_PARALLEL=0: one at a time); NATIVE_JOBS sets ninja's -j (default: CPU count), NATIVE_LOG_DIR the
# per-target logs (default build/native-logs/<time>).
# One recipe for CI (.github/workflows/build.yml job "native", self-hosted runner) and developers.
#
#   packaging/native/build-native.sh [OUT_DIR]       default OUT_DIR=build/native-release
#
# Everything is pinned in the component recipes (hwcomposer/pins.env, native/mesa/build.py PIN,
# nvidia-prolapse/*/Makefile, media/vndk/build.sh); this script only sequences them. Rust profile:
# release. Network is needed on the first run (pinned downloads, checksum-verified by each recipe).
#
# It runs in the build-environment container packaging/native/Containerfile (pinned base digest, Arch
# Linux Archive snapshot, Rust toolchain): started on the host, it builds or reuses that image (tagged with
# the Containerfile's hash) and re-runs itself inside, as the calling user, at lowered CPU/IO priority, with
# the checkout (and its persistent build/ cache) mounted at the same path. The host needs only docker.
# The container keeps network access: a cold build/ cache downloads pinned sources (Mesa, AOSP headers,
# crates, NDKs, OTA images), each checked against its pin. NATIVE_BUILDENV=host runs on the host as before
# (a developer machine with every tool installed); a missing tool then fails by name.
set -euo pipefail
REPO=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)
OUT=$(realpath -m -- "${1:-$REPO/build/native-release}")
NATIVE=$REPO/nvidia-prolapse/driver-source/native
PY=${PYTHON:-python3}
say() { printf '\n== %s\n' "$*"; }
die() { echo "build-native: $*" >&2; exit 1; }
# every pinned download goes through tools/fetch.py (mirrors, retries with backoff, verified before kept);
# network commands (git fetch, cargo) through its `retry`
net() { "$PY" "$REPO/tools/fetch.py" retry -- "$@"; }
cd "$REPO"
# NDK r30: the Vulkan HAL's Vulkan 1.4 headers (native/mesa/build-hal*.py use /opt/android-ndk)
NDK_R30_URL=https://dl.google.com/android/repository/android-ndk-r30-linux.zip
NDK_R30_SHA256=753611f410d002cfcd3f3dc2ef49aad532089d3180b436c060a90bf0fcb64df2

if [ -z "${NATIVE_BUILDENV:-}" ]; then
    command -v docker >/dev/null && docker info >/dev/null 2>&1 \
        || die "docker is not usable by $(id -un): the build runs in packaging/native/Containerfile's image (or set NATIVE_BUILDENV=host)"
    BUILDENV_FILE=$REPO/packaging/native/Containerfile
    IMAGE=native-buildenv:$(sha256sum "$BUILDENV_FILE" | cut -c1-12)
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        say "build the build-environment image $IMAGE"
        nice -n 10 ionice -c2 -n7 docker build -q -t "$IMAGE" - < "$BUILDENV_FILE" || die "docker build of $BUILDENV_FILE failed"
    fi
    mkdir -p "$REPO/build/ndk-r30" "$REPO/build/.buildenv-home" "$(dirname "$OUT")"
    mounts=(-v "$REPO:$REPO" -v "$REPO/build/ndk-r30:/cache/ndk-r30")
    case "$OUT/" in "$REPO"/*) ;; *) mounts+=(-v "$(dirname "$OUT"):$(dirname "$OUT")") ;; esac
    if [ -n "${NATIVE_LOG_DIR:-}" ]; then
        mkdir -p "$NATIVE_LOG_DIR"
        case "$NATIVE_LOG_DIR/" in "$REPO"/*|"$(dirname "$OUT")"/*) ;; *) mounts+=(-v "$NATIVE_LOG_DIR:$NATIVE_LOG_DIR") ;; esac
    fi
    # a git worktree keeps its object store elsewhere (collect.py records the commit)
    common=$(cd "$REPO" && realpath -- "$(git rev-parse --git-common-dir)")
    case "$common/" in "$REPO"/*) ;; *) mounts+=(-v "$common:$common:ro") ;; esac
    say "running in $IMAGE as $(id -un)"
    exec docker run --rm --init --user "$(id -u):$(id -g)" "${mounts[@]}" -w "$REPO" \
        -e HOME="$REPO/build/.buildenv-home" -e CARGO_HOME="$REPO/build/.buildenv-home/cargo" \
        -e NATIVE_BUILDENV="$IMAGE" -e NATIVE_BUILDENV_FILE_SHA256="$(sha256sum "$BUILDENV_FILE" | cut -d' ' -f1)" \
        -e RELEASE_TAG="${RELEASE_TAG:-}" -e NATIVE_PARALLEL="${NATIVE_PARALLEL:-1}" \
        -e NATIVE_JOBS="${NATIVE_JOBS:-}" -e NATIVE_LOG_DIR="${NATIVE_LOG_DIR:-}" -e NATIVE_CCACHE="${NATIVE_CCACHE:-1}" \
        -e CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}" -e TERM=dumb "$IMAGE" \
        bash -c 'renice -n 10 -p $$ >/dev/null && ionice -c2 -n7 -p $$ || { echo "cannot lower the build priority" >&2; exit 1; }
                 exec packaging/native/build-native.sh "$0"' "$OUT"
fi

if [ "$NATIVE_BUILDENV" != host ]; then
    say "NDK r30 (sha256-checked into build/ndk-r30, mounted at /cache/ndk-r30 = /opt/android-ndk)"
    if [ ! -x /cache/ndk-r30/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang ]; then
        zip=/cache/ndk-r30/android-ndk-r30-linux.zip
        "$PY" tools/fetch.py --out "$zip" --sha256 "$NDK_R30_SHA256" "$NDK_R30_URL" || die "download failed: $NDK_R30_URL"
        unzip -q -o "$zip" -d /cache/ndk-r30
    fi
fi

if [ "$NATIVE_BUILDENV" != host ] && [ "${NATIVE_CCACHE:-1}" != 0 ]; then
    # ccache in front of every C/C++ compile of the Meson builds and the Codec2 service (tools/cc_launcher.py), cache
    # in the persistent build/ccache, size-capped (CCACHE_MAXSIZE, default 20G); NATIVE_CCACHE=0 builds without it.
    # A hit returns the object the same compiler made for the same preprocessed input and command line, so the
    # outputs are the same bytes (compared: docs/dev-setup.md "CI").
    command -v ccache >/dev/null || die "ccache missing from the build environment (packaging/native/Containerfile)"
    export NATIVE_CC_LAUNCHER=ccache CCACHE_DIR=$REPO/build/ccache CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-20G}
    mkdir -p "$CCACHE_DIR" && ccache -z >/dev/null
fi

say "preflight"
missing=()
for tool in git curl unzip debugfs meson ninja cmake cc c++ cargo rustup glslangValidator spirv-val readelf zstd "$PY"; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
[ ${#missing[@]} -eq 0 ] || die "missing tools: ${missing[*]}"
"$PY" -c 'import mako' 2>/dev/null || die "python mako missing (pip/pacman: python-mako)"
# the Vulkan HAL needs the Vulkan 1.4 headers of NDK r30 (native/mesa/build-hal.py); the rest uses the
# r29 that hwcomposer/provision.sh fetches
[ -x /opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/x86_64-linux-android33-clang ] \
    || die "Android NDK r30 missing at /opt/android-ndk (the Vulkan HAL build uses it)"
# llvmpipe references for the GL gate (build-gate.py): host Mesa EGL through glvnd
[ -f /usr/share/glvnd/egl_vendor.d/50_mesa.json ] || die "host Mesa EGL (glvnd vendor 50_mesa.json) missing: install mesa + libglvnd"
for t in x86_64-linux-android i686-linux-android; do
    rustup target list --installed | grep -qx "$t" || die "rust target $t missing: rustup target add $t"
done

say "hwcomposer toolchain + deps (NDK r29, AOSP headers, image link stubs)"
hwcomposer/provision.sh

say "Mesa object store build/mesa-git (worktrees are taken from it at the pins)"
MESA_URL=https://gitlab.freedesktop.org/mesa/mesa.git
NVK_PIN=$(sed -n 's/^PIN = "\([0-9a-f]\{40\}\)".*/\1/p' "$NATIVE/mesa/build.py")
HAL_PIN=$(sed -n 's/^MESA_SHA := \([0-9a-f]\{40\}\)$/\1/p' nvidia-prolapse/hal/Makefile)
GRALLOC_PIN=$(sed -n 's/^MESA_REV *:= \([0-9a-f]\{40\}\)$/\1/p' nvidia-prolapse/gralloc/Makefile)
[ -n "$NVK_PIN" ] && [ -n "$HAL_PIN" ] && [ -n "$GRALLOC_PIN" ] || die "could not read the Mesa pins"
[ -d build/mesa-git/.git ] || git init -q build/mesa-git
for pin in "$NVK_PIN" "$HAL_PIN" "$GRALLOC_PIN"; do
    git -C build/mesa-git cat-file -e "$pin^{commit}" 2>/dev/null || net git -C build/mesa-git fetch -q "$MESA_URL" "$pin"
done

say "bionic libdrm (both ABIs)"
# the blob census belongs to glibc-shim mode (NVIDIA's blobs call libdrm); here NVK, nvrm and gralloc do
CENSUS="nvidia-prolapse build: no NVIDIA userspace loads this libdrm"
make -C nvidia-prolapse/libdrm NO_BLOB_CENSUS="$CENSUS"
make -C nvidia-prolapse/libdrm ARCH=x86 NO_BLOB_CENSUS="$CENSUS"

say "Android runtime closure for the 32-bit host tests (read-only from the pinned image)"
# the image hwcomposer/provision.sh extracted its link stubs from: the pinned OTA download, or the
# local Waydroid images it preferred ($HWC_IMAGES / waydroid.cfg images_path)
SYSIMG=$(ls build/hwcomposer-work/deps/dl/system-img/*.img 2>/dev/null || true)
VENIMG=$(ls build/hwcomposer-work/deps/dl/vendor-img/*.img 2>/dev/null || true)
if [ -z "$SYSIMG" ] && [ "$NATIVE_BUILDENV" = host ]; then
    IMGDIR=${HWC_IMAGES:-$(sed -n 's/^images_path *= *//p' /var/lib/waydroid/waydroid.cfg 2>/dev/null | head -1)}
    SYSIMG=${IMGDIR:+$IMGDIR/system.img} VENIMG=${IMGDIR:+$IMGDIR/vendor.img}
elif [ -z "$SYSIMG" ] || [ -z "$VENIMG" ]; then   # container: the pinned OTA images (hwcomposer/pins.env), as provision.sh fetches them
    # shellcheck source=../../hwcomposer/pins.env
    source hwcomposer/pins.env
    dl=build/hwcomposer-work/deps/dl
    for part in system vendor; do
        ls "$dl/$part-img/"*.img >/dev/null 2>&1 && continue
        u=IMG_${part^^}_URL s=IMG_${part^^}_SHA256 a=IMG_${part^^}_URL_ALT
        "$PY" tools/fetch.py --out "$dl/$part.zip" --sha256 "${!s}" "${!u}" "${!a}" || die "download failed: ${!u}"
        rm -rf "$dl/$part-img" && unzip -oq "$dl/$part.zip" -d "$dl/$part-img"
    done
    SYSIMG=$(ls $dl/system-img/*.img) VENIMG=$(ls $dl/vendor-img/*.img)
fi
[ -n "$SYSIMG" ] && [ -r "$SYSIMG" ] && [ -r "$VENIMG" ] || die "no system/vendor image: neither the pinned OTA download nor local Waydroid images (HWC_IMAGES)"
# absolute: sub-builds run from their own directories (make -C nvidia-prolapse/gralloc dumps link stubs from them)
SYSIMG=$(realpath -- "$SYSIMG") VENIMG=$(realpath -- "$VENIMG")
export WAYDROID_SYSTEM_IMG=$SYSIMG WAYDROID_VENDOR_IMG=$VENIMG    # media/vndk/build.sh reads these
"$PY" "$NATIVE/mesa/prepare-runtime32.py" --image "$SYSIMG"

# ---------------------------------------------------------------- the targets, concurrently where independent
# Dependencies (everything else runs side by side):
#   nak-tools (bindgen/cbindgen) -> nak, nvk-host;  nvk-host -> nvk-android, nvk-android32, gl-prepare  (one Mesa worktree each for NVK and nvrm:
#          prepared once, then the targets build from it with --skip-prepare)
#   nvk-android + gl-prepare -> nvrm-android;  nvk-android32 + gl-prepare -> nvrm-android32  (nvrm links the
#          libnv_bionic_native.so of its ABI's NVK build)
#   nvk-android + media -> media-service  (the Codec2 service links the x86_64 backend)
#   hwcomposer, hal (-> HAL tests), gralloc (-> minigbm), media: only the steps above this section
#   gralloc (minigbm) -> glibc-shim (shims, the HAL over NVIDIA's ICD, gralloc backend, EGL front)
#   everything -> gate programs -> collect
# Each target logs to $LOGS/<target>.log; a failure names the target, prints the end of its log and stops
# the others. NATIVE_PARALLEL=0 runs the same targets one at a time, in declaration order. Each ninja keeps its -j
# and stops starting jobs while the load average exceeds the CPU count (NINJA_LOAD), so concurrent targets
# share the machine instead of multiplying it.
LOGS=${NATIVE_LOG_DIR:-$REPO/build/native-logs/$(date +%Y%m%d-%H%M%S)}
mkdir -p "$LOGS"
: > "$LOGS/TIMES"
PARALLEL=${NATIVE_PARALLEL:-1}
JOBS=${NATIVE_JOBS:-}
JOBS=${JOBS:-$(nproc)}
export NINJA_LOAD=${NINJA_LOAD:-$(nproc)}
set -m                                 # every target in its own process group: a failure stops whole trees
T_START=$SECONDS

stop_all() { local p; for p in "${!BYPID[@]}"; do kill -TERM -- "-$p" 2>/dev/null || true; done; }
trap 'stop_all' INT TERM
declare -A DEPS=() CMD=() STATE=() BYPID=() T0=()
ORDER=()
target() {  # target NAME "DEP..." COMMAND...: declare (declaration order = the sequential order)
    local name=$1 deps=$2; shift 2
    DEPS[$name]=$deps; CMD[$name]=$(printf '%q ' "$@"); STATE[$name]=pending; ORDER+=("$name")
}
launch() {
    local name=$1
    T0[$name]=$SECONDS; STATE[$name]=running
    ( set -e; eval "${CMD[$name]}" ) > "$LOGS/$name.log" 2>&1 &
    BYPID[$!]=$name
    printf -- '-- %-15s started  (+%ss)\n' "$name" $(( SECONDS - T_START ))
}
run_targets() {   # start every target whose dependencies are done; the first failure stops everything
    local name dep ready running pid rc
    while :; do
        running=${#BYPID[@]}
        for name in "${ORDER[@]}"; do
            [ "${STATE[$name]}" = pending ] || continue
            [ "$PARALLEL" = 1 ] || [ "$running" -eq 0 ] || break
            ready=1
            for dep in ${DEPS[$name]}; do [ "${STATE[$dep]}" = done ] || ready=0; done
            [ "$ready" = 1 ] || { [ "$PARALLEL" = 1 ] && continue || break; }
            launch "$name"; running=$(( running + 1 ))
        done
        [ "${#BYPID[@]}" -gt 0 ] || break
        rc=0; wait -n -p pid || rc=$?
        name=${BYPID[$pid]}; unset "BYPID[$pid]"
        if [ "$rc" -ne 0 ]; then
            stop_all
            echo "build-native: target $name FAILED (exit $rc); its log: $LOGS/$name.log (last lines below)" >&2
            tail -n 60 "$LOGS/$name.log" >&2
            exit 1
        fi
        STATE[$name]=done
        printf -- '-- %-15s done in %ss\n' "$name" $(( SECONDS - T0[$name] ))
        echo "$name $(( SECONDS - T0[$name] ))" >> "$LOGS/TIMES"
    done
    for name in "${ORDER[@]}"; do
        [ "${STATE[$name]}" = done ] || { echo "build-native: target $name never became ready (deps: ${DEPS[$name]})" >&2; exit 1; }
    done
}

rel() { "$PY" "$@" --rust-profile release; }
t_nak_tools()  { "$PY" "$NATIVE/compiler/build.py" --tools-only --download-deps; }
t_nak()        { "$PY" "$NATIVE/compiler/build.py" --target both --download-deps -j "$JOBS"; }
t_nvk_host()   { rel "$NATIVE/mesa/build.py" --target host --download-deps -j "$JOBS"; }   # also prepares the NVK worktree
t_nvk()        { rel "$NATIVE/mesa/build.py" --target "$1" --download-deps --skip-prepare -j "$JOBS"; }
t_gl_prepare() { PROLAPSE_ROOT=$REPO rel "$NATIVE/mesa/build.py" --gl --prepare-only --download-deps; }
t_nvrm()       { PROLAPSE_ROOT=$REPO rel "$NATIVE/mesa/build.py" --gl --target "$1" --skip-prepare -j "$JOBS"; }
t_hwcomposer() { hwcomposer/build.sh; }
t_hal()        { "$PY" "$NATIVE/mesa/build-hal.py"; "$PY" "$NATIVE/mesa/build-hal-tests.py"; }
t_gralloc()    { "$PY" "$NATIVE/mesa/build-gralloc.py" --target all
                 make -C nvidia-prolapse/gralloc minigbm-fetch
                 make -C nvidia-prolapse/gralloc minigbm SYSTEM_IMG="$SYSIMG" VENDOR_IMG="$VENIMG"; }
# glibc-shim mode (NVIDIA's own userspace from the user's host through our glibc-compat shims): the guest side only.
# The shims are cut from the per-branch reference import lists (nvidia-prolapse/shim/imports): no NVIDIA driver here.
t_glibc_shim() { local why="release build: the builder has no NVIDIA driver; the per-branch reference lists are the census"
                 make -C nvidia-prolapse/shim REFERENCE_ONLY="$why"
                 make -C nvidia-prolapse/shim ARCH=x86 REFERENCE_ONLY="$why"
                 make -C nvidia-prolapse/hal android android32
                 make -C nvidia-prolapse/gralloc android android32 check check-minigbm SYSTEM_IMG="$SYSIMG" VENDOR_IMG="$VENIMG"
                 make -C nvidia-prolapse/egl sources
                 make -C nvidia-prolapse/egl android android32 check; }
t_media()      { nvidia-prolapse/media/vndk/build.sh; make -C nvidia-prolapse/media test; }
t_media_svc()  { NV_VIDEO_LIB_DIR=$(dirname "$("$PY" -c "import sys; sys.path.insert(0, 'tools'); import native_rust_profile as p; print(p.rust_library(__import__('pathlib').Path('$REPO'), 'android', 'release'))")") \
                     nvidia-prolapse/media/vndk/build.sh service; }

say "targets ($([ "$PARALLEL" = 1 ] && echo concurrent || echo one at a time); ninja -j $JOBS -l $NINJA_LOAD; logs in $LOGS)"
# the Rust RM builds --offline: fill CARGO_HOME's registry from Cargo.lock (checksummed) first
net cargo fetch --locked --manifest-path "$NATIVE/Cargo.toml"
target nak-tools      ""                         t_nak_tools
target nak            "nak-tools"                t_nak
target nvk-host       "nak-tools"                t_nvk_host
target nvk-android    "nvk-host"                 t_nvk android
target nvk-android32  "nvk-host"                 t_nvk android32
target gl-prepare     "nvk-host"                 t_gl_prepare
target nvrm-android   "nvk-android gl-prepare"   t_nvrm android
target nvrm-android32 "nvk-android32 gl-prepare" t_nvrm android32
target hwcomposer     ""                         t_hwcomposer
target hal            ""                         t_hal
target gralloc        ""                         t_gralloc
target media          ""                         t_media
target glibc-shim     "gralloc"                  t_glibc_shim
target media-service  "media nvk-android"        t_media_svc
target gate           "nvrm-android nvrm-android32 hal media-service glibc-shim" \
                      "$PY" packaging/native/build-gate.py "$REPO/build/native-gate"
run_targets

say "collect the release bundle"
"$PY" packaging/native/collect.py "$OUT" "$REPO/build/native-gate"
echo "build-native: $(( SECONDS - T_START )) s for the targets; per target: $LOGS/TIMES"
[ -z "${NATIVE_CC_LAUNCHER:-}" ] || { echo "ccache ($CCACHE_DIR, max $CCACHE_MAXSIZE):"; ccache -s | sed 's/^/  /'; }
