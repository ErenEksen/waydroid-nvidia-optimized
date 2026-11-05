#!/usr/bin/env bash
# packaging/arch/test-assembly.sh ASSETS_DIR OVERLAY.tar.gz WAYDROID.tar.gz
#
# Tests the PKGBUILD's packaging without its heavy part: makepkg runs the real PKGBUILD with only the Mesa and
# virglrenderer source steps and build() replaced; build() takes the Venus binaries from OVERLAY (a
# dev/make-bundle archive, e.g. perf-v1) instead of compiling them. prepare() otherwise (tree, NDK extraction from a
# stub zip, Waydroid patches, upstream prebuilts), check(), package() and the .install scriptlet are the real ones.
# Then packaging/arch/check-package.py checks the package, and pacman installs it into a scratch root, verifies
# every file against the package's mtree and removes it again.
#
#   ASSETS_DIR      upstream release assets named as in the PKGBUILD's source array (prebuilts, native, vendor
#                   zip and channel JSON), checked against the PKGBUILD's sha256sums by makepkg
#   WAYDROID.tar.gz waydroid at _waydroid_sha with top directory waydroid-<sha>/ (its sha256 is not checked: a
#                   local `git archive` differs from GitHub's tarball byte-wise)
# NVWD_KEEP_PKG=DIR keeps a copy of the package. Run as a user that may run makepkg (not root). Needs makepkg, pacman, bsdtar, zstd, patchelf, readelf, python3.
set -euo pipefail
die() { echo "test-assembly: $*" >&2; exit 1; }
[ $# -eq 3 ] || die "usage: test-assembly.sh ASSETS_DIR OVERLAY.tar.gz WAYDROID.tar.gz"
ASSETS=$(cd "$1" && pwd) OVERLAY=$(readlink -f "$2") WAYDROID_TGZ=$(readlink -f "$3")
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PKG=$HERE/waydroid-nvidia-optimized
W=$(mktemp -d "${TMPDIR:-/tmp}/nvwd-assembly.XXXXXX")
trap 'rm -rf "$W"' EXIT
mkdir -p "$W"/{pkgbuild,build,src,out,root,ov,ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin}

# --- sources makepkg must not download
# shellcheck disable=SC1091
_waydroid_sha=$(sed -n 's/^_waydroid_sha=\([0-9a-f]*\).*/\1/p' "$PKG/PKGBUILD")
_tag=$(sed -n 's/^_upstream_tag=//p' "$PKG/PKGBUILD")
for f in waydroid-nvidia-guest-prebuilts-$_tag.tar.zst waydroid-nvidia-native-x86_64-$_tag.tar.zst \
         waydroid-nvidia-vendor-x86_64-$_tag.zip waydroid-nvidia-vendor-ota-$_tag.json; do
    [ -f "$ASSETS/$f" ] || die "$ASSETS/$f missing"
    ln -s "$ASSETS/$f" "$W/src/$f"
done
cp "$WAYDROID_TGZ" "$W/src/waydroid-$_waydroid_sha.tar.gz"
echo 'Pkg.Revision = 29.0.0-stub' > "$W/ndk/android-ndk-r29/source.properties"
echo stub > "$W/ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin/clang"
mkdir -p "$W/ndk/android-ndk-r29/sources" && echo 'not extracted' > "$W/ndk/android-ndk-r29/sources/README"
(cd "$W/ndk" && bsdtar --format zip -cf "$W/src/android-ndk-r29-linux.zip" android-ndk-r29)
tar -xzf "$OVERLAY" -C "$W/ov"

# --- the real PKGBUILD, with the network-only steps replaced (appended definitions win)
cp "$PKG/PKGBUILD" "$PKG/waydroid-nvidia-optimized.install" "$W/pkgbuild/"
cat >> "$W/pkgbuild/PKGBUILD" <<EOF

# ---- test-assembly.sh overrides ----
source=("\${source[@]:0:6}")
sha256sums=('SKIP' "\${sha256sums[@]:1:4}" 'SKIP')
_repo() { echo '$REPO'; }
_prepare_mesa() { :; }
_prepare_virgl() { :; }
build() {
  local o='$W/ov'
  # prepare() kept only the NDK's toolchain directory
  [ -f "\$srcdir/android-ndk-\$_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ] || return 1
  [ ! -e "\$srcdir/android-ndk-\$_ndk/sources" ] || { error 'NDK extraction is not selective'; return 1; }
  install -Dm644 "\$o/vulkan-x86_64.so" "\$srcdir/mesa-build-x86_64/src/virtio/vulkan/libvulkan_virtio.so"
  install -Dm644 "\$o/vulkan-x86.so" "\$srcdir/mesa-build-x86/src/virtio/vulkan/libvulkan_virtio.so"
  install -Dm644 "\$o/libgbm_mesa_wrapper.so" "\$srcdir/gralloc-out/libgbm_mesa_wrapper.so"
  install -Dm755 "\$o/virgl_test_server" "\$srcdir/virgl-build/vtest/virgl_test_server"
  install -Dm755 "\$o/virgl_render_server" "\$srcdir/virgl-build/server/virgl_render_server"
  install -Dm755 "\$o/libvirglrenderer.so.1" "\$srcdir/virgl-build/src/libvirglrenderer.so.1"
  _check_elf "\$srcdir/mesa-build-x86_64/src/virtio/vulkan/libvulkan_virtio.so" ELF64 'Advanced Micro Devices X86-64' libvulkan_virtio.so
  _check_elf "\$srcdir/mesa-build-x86/src/virtio/vulkan/libvulkan_virtio.so" ELF32 'Intel 80386' libvulkan_virtio.so
  _check_elf "\$srcdir/gralloc-out/libgbm_mesa_wrapper.so" ELF64 'Advanced Micro Devices X86-64' libgbm_mesa_wrapper.so
  # the ABI check must reject a swapped pair
  if _check_elf "\$srcdir/mesa-build-x86/src/virtio/vulkan/libvulkan_virtio.so" ELF64 'Advanced Micro Devices X86-64' libvulkan_virtio.so 2>/dev/null; then
    error '_check_elf accepted an ELF32 library as ELF64'; return 1
  fi
}
EOF

cd "$W/pkgbuild"
BUILDDIR=$W/build SRCDEST=$W/src PKGDEST=$W/out PKGEXT=.pkg.tar.zst makepkg --nodeps --noconfirm --cleanbuild 2>&1 | sed 's/^/  makepkg: /'
pkg=$(ls "$W"/out/waydroid-nvidia-optimized-*.pkg.tar.zst)
echo "==> built $(basename "$pkg") ($(du -h "$pkg" | cut -f1))"
[ -z "${NVWD_KEEP_PKG:-}" ] || cp "$pkg" "$NVWD_KEEP_PKG/"   # keep a copy for inspection

echo "==> check-package.py"
python3 -I "$HERE/check-package.py" "$pkg"
# negative: these binaries are not this tree's build, --built-here must say so
if python3 -I "$HERE/check-package.py" "$pkg" --built-here >/dev/null; then
    die "check-package.py --built-here accepted binaries not built from this tree"
fi
echo "==> check-package.py --built-here rejects the overlay binaries (expected)"

echo "==> pacman: install into a scratch root, verify, remove"
R=$W/root
mkdir -p "$R/var/lib/pacman"
# pacman refuses to run unless root; one fakeroot session keeps the faked ownership consistent for -Qkk
fakeroot -- bash -euo pipefail -c '
    R=$1 pkg=$2
    pm() { pacman --root "$R" --dbpath "$R/var/lib/pacman" --config /dev/null --noconfirm "$@"; }
    pm --nodeps --nodeps --noscriptlet -U "$pkg" >/dev/null
    pm -Qi waydroid-nvidia-optimized | sed -n "1,3p;/^Provides/p;/^Conflicts/p"
    pm -Qkk waydroid-nvidia-optimized
    [ -x "$R/usr/lib/waydroid-nvidia/virgl_test_server" ] && [ -L "$R/usr/bin/waydroid" ] ||
        { echo "installed tree incomplete" >&2; exit 1; }
    grep -q "^fork_commit=" "$R/usr/lib/waydroid-nvidia/BUILD-INFO" || { echo "BUILD-INFO lacks fork_commit" >&2; exit 1; }
    pm --noscriptlet -R waydroid-nvidia-optimized >/dev/null
' test-assembly "$R" "$pkg"
left=$(cd "$R" && find . -mindepth 1 -not -path './var/lib/pacman*' -not -type d | head -5)
[ -z "$left" ] || die "files left after removal: $left"
echo "==> test-assembly: PASS"
